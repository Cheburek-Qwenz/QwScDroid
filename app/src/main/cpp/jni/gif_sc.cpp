// ── GIF ↔ SC2 ────────────────────────────────────────────────────────────────
// gif2sc: идея, формат файла и параметры — @Ghoul1337i (исходный Python-инструмент
// gifconv.py, портирован на C++ с разрешения автора).
//
// GIF превращается в SC2 (v5) с одним MovieClip-ом: каждый уникальный кадр —
// отдельный Shape-квад, кадры лежат в несжатых RGBA-текстурах (страницы до
// 4092x4092, по 12 кадров), таймлайн ссылается на шейпы.
// sc2gif: обратное преобразование — рендер кадров MovieClip-а и запись GIF.

#include "sc_core.h"
#include "sc2_reader.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "astc_decoder.h"
bool astc_encode_fast(const uint8_t* rgba, int width, int height, int block_w, int block_h, std::vector<uint8_t>& output);
#include "zstd.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <sys/stat.h>

// ── минимальный аналог flatbuffers.Builder (Python) ──
// Минимальный аналог Python flatbuffers.Builder (тот же порядок записи и
// дедупликация vtable), чтобы вывод совпадал с эталонным gifconv.py.
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>

class FBBuilder {
public:
    explicit FBBuilder(size_t initial) : buf_(initial < 1 ? 1 : initial, 0), head_(buf_.size()) {}

    size_t offset() const { return buf_.size() - head_; }
    const uint8_t* data() const { return buf_.data() + head_; }
    size_t size() const { return buf_.size() - head_; }

    void prep(size_t size, size_t additional) {
        if (size > minalign_) minalign_ = size;
        size_t align = (~(buf_.size() - head_ + additional) + 1) & (size - 1);
        while (head_ < align + size + additional) grow();
        pad(align);
    }

    // ---- векторы / строки ----
    size_t startVector(size_t elemSize, size_t num, size_t alignment) {
        nested_ = true;
        vecNum_ = num;
        prep(4, elemSize * num);
        prep(alignment, elemSize * num);
        return offset();
    }
    size_t endVector() {
        nested_ = false;
        placeU32((uint32_t)vecNum_);
        return offset();
    }
    size_t createString(const std::string& s) {
        nested_ = true;
        prep(4, s.size() + 1);
        placeU8(0);
        head_ -= s.size();
        if (!s.empty()) memcpy(buf_.data() + head_, s.data(), s.size());
        vecNum_ = s.size();
        return endVector();
    }
    // Вектор байт без копирования: возвращает смещение, *dst указывает на область для заполнения.
    size_t createByteVector(const uint8_t* src, size_t len, uint8_t** dst = nullptr) {
        nested_ = true;
        prep(4, len);
        head_ -= len;
        if (src && len) memcpy(buf_.data() + head_, src, len);
        if (dst) *dst = buf_.data() + head_;
        vecNum_ = len;
        return endVector();
    }

    // ---- примитивы ----
    void prependU8(uint8_t v)  { prep(1, 0); placeU8(v); }
    void prependU16(uint16_t v){ prep(2, 0); placeU16(v); }
    void prependU32(uint32_t v){ prep(4, 0); placeU32(v); }
    void prependF32(float v)   { prep(4, 0); head_ -= 4; memcpy(buf_.data() + head_, &v, 4); }
    void prependUOffset(size_t off) {
        prep(4, 0);
        if (off > offset()) throw std::runtime_error("fb: bad offset");
        placeU32((uint32_t)(offset() - off + 4));
    }

    // ---- таблицы ----
    void startObject(size_t numFields) {
        nested_ = true;
        cur_.assign(numFields, 0);
        objectEnd_ = offset();
    }
    void slot(size_t i) { cur_[i] = offset(); }
    void prependU8Slot(size_t i, uint8_t v, int64_t d)   { if ((int64_t)v != d) { prependU8(v);  slot(i); } }
    void prependU16Slot(size_t i, uint16_t v, int64_t d) { if ((int64_t)v != d) { prependU16(v); slot(i); } }
    void prependU32Slot(size_t i, uint32_t v, int64_t d) { if ((int64_t)v != d) { prependU32(v); slot(i); } }
    void prependUOffsetSlot(size_t i, size_t v, size_t d){ if (v != d) { prependUOffset(v); slot(i); } }

    size_t endObject() {
        // placeholder soffset
        prep(4, 0);
        placeU32(0);
        size_t objectOffset = offset();
        size_t n = cur_.size();
        while (n > 0 && cur_[n - 1] == 0) n--;
        cur_.resize(n);
        for (size_t i = n; i-- > 0;) {
            uint16_t off = cur_[i] ? (uint16_t)(objectOffset - cur_[i]) : 0;
            prependU16(off);
        }
        prependU16((uint16_t)(objectOffset - objectEnd_));
        prependU16((uint16_t)((n + 2) * 2));

        size_t existing = 0;
        for (size_t k = vtables_.size(); k-- > 0;) {
            size_t vt2Off = vtables_[k];
            size_t vt2Start = buf_.size() - vt2Off;
            uint16_t vt2Len; memcpy(&vt2Len, buf_.data() + vt2Start, 2);
            if ((size_t)vt2Len != (n + 2) * 2) continue;
            bool eq = true;
            for (size_t i = 0; i < n && eq; i++) {
                uint16_t x; memcpy(&x, buf_.data() + vt2Start + 4 + i * 2, 2);
                if (x == 0 && cur_[i] == 0) continue;
                if ((size_t)x != objectOffset - cur_[i]) eq = false;
            }
            if (eq) { existing = vt2Off; break; }
        }
        if (existing) {
            head_ = buf_.size() - objectOffset;
            int32_t so = (int32_t)((int64_t)existing - (int64_t)objectOffset);
            memcpy(buf_.data() + head_, &so, 4);
        } else {
            vtables_.push_back(offset());
            int32_t so = (int32_t)((int64_t)offset() - (int64_t)objectOffset);
            memcpy(buf_.data() + (buf_.size() - objectOffset), &so, 4);
        }
        nested_ = false;
        return objectOffset;
    }

    void finish(size_t root) {
        prep(minalign_, 4);
        prependUOffset(root);
    }

private:
    void grow() {
        size_t old = buf_.size();
        std::vector<uint8_t> nb(old * 2, 0);
        memcpy(nb.data() + old, buf_.data(), old);
        buf_.swap(nb);
        head_ += old;
    }
    void pad(size_t n) { head_ -= n; if (n) memset(buf_.data() + head_, 0, n); }
    void placeU8(uint8_t v)  { head_ -= 1; buf_[head_] = v; }
    void placeU16(uint16_t v){ head_ -= 2; memcpy(buf_.data() + head_, &v, 2); }
    void placeU32(uint32_t v){ head_ -= 4; memcpy(buf_.data() + head_, &v, 4); }

    std::vector<uint8_t> buf_;
    size_t head_;
    size_t minalign_ = 1;
    bool nested_ = false;
    size_t vecNum_ = 0;
    size_t objectEnd_ = 0;
    std::vector<size_t> cur_;
    std::vector<size_t> vtables_;
};

namespace gifsc {

static const int    PAGE_MAX_DIM    = 4092;
static const int    FRAMES_PER_PAGE = 12;
static const double QUAD_WIDTH      = 640.0;
static const int    SCREEN_2048[2]  = {2048, 1152};
static const int    SCREEN_1848[2]  = {1848, 1112};
static const double AUTO_MARGIN_PCT = 3.0;
static const double AUTO_STRETCH_TOL= 0.02;
static const int    UV_INSET_PX     = 2;
static const int    MAX_SHAPES      = 3000;
static const int    MAX_PAGES       = 255;
static const int    MAX_TIMELINE    = 30000;
static const double HARD_RAW_LIMIT  = 1900000000.0;
static const int    TILE_CANDIDATES[] = {2048,1848,1536,1280,1024,896,768,640,512,384,256,192,128};
static const int    N_TILE_CAND     = 13;
static const int    MIN_AUTO_TILE   = 256;
static const int    USER_MAX_FPS    = 60;
static const int    DEFAULT_MAX_FPS = 30;
static const int    MIN_GIF_DELAY_MS= 10;
static const int    FALLBACK_DELAY_MS = 100;
static const int    ZSTD_LEVEL      = 5;      // оригинал: 10; на телефоне быстрее и почти не больше
static const double RAW_BUDGET      = 400e6;  // оригинал: 1.05e9; ограничение по памяти телефона
enum { FIT_CONTAIN = 1, FIT_COVER = 2, FIT_STRETCH = 3 };

struct Img {
    int w = 0, h = 0, ch = 3;
    std::vector<uint8_t> d;
    Img() {}
    Img(int w_, int h_, int ch_) : w(w_), h(h_), ch(ch_), d((size_t)w_ * h_ * ch_, 0) {}
};

// ── ресемплинг (Lanczos3 / bicubic) ─────────────────────────────────────────
static double k_lanczos(double x) {
    if (x <= -3.0 || x >= 3.0) return 0.0;
    if (x > -1e-9 && x < 1e-9) return 1.0;
    double px = M_PI * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}
static double k_bicubic(double x) {
    const double a = -0.5;
    if (x < 0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0) return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    return 0.0;
}

struct Coeffs { std::vector<int> start, count; std::vector<float> w; int maxc = 0; };

static Coeffs make_coeffs(int in, int out, bool lanczos) {
    Coeffs c;
    double scale = (double)in / out;
    double fscale = std::max(scale, 1.0);
    double support = (lanczos ? 3.0 : 2.0) * fscale;
    c.maxc = (int)std::ceil(support) * 2 + 1;
    c.start.resize(out); c.count.resize(out); c.w.assign((size_t)out * c.maxc, 0.f);
    for (int x = 0; x < out; x++) {
        double center = (x + 0.5) * scale;
        int xmin = std::max(0, (int)(center - support + 0.5));
        int xmax = std::min(in, (int)(center + support + 0.5));
        int n = xmax - xmin;
        double sum = 0;
        for (int i = 0; i < n; i++) {
            double v = (xmin + i + 0.5 - center) / fscale;
            double wv = lanczos ? k_lanczos(v) : k_bicubic(v);
            c.w[(size_t)x * c.maxc + i] = (float)wv;
            sum += wv;
        }
        if (sum != 0) for (int i = 0; i < n; i++) c.w[(size_t)x * c.maxc + i] = (float)(c.w[(size_t)x * c.maxc + i] / sum);
        c.start[x] = xmin; c.count[x] = n;
    }
    return c;
}

static inline uint8_t clamp8(float v) { int i = (int)std::floor(v + 0.5f); return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i)); }

static Img resample(const Img& s, int dw, int dh, bool lanczos = true) {
    if (s.w == dw && s.h == dh) return s;
    const int ch = s.ch;
    Img mid(dw, s.h, ch);
    if (dw == s.w) mid = s;
    else {
        Coeffs cx = make_coeffs(s.w, dw, lanczos);
        for (int y = 0; y < s.h; y++) {
            const uint8_t* row = &s.d[(size_t)y * s.w * ch];
            uint8_t* o = &mid.d[(size_t)y * dw * ch];
            for (int x = 0; x < dw; x++) {
                const float* w = &cx.w[(size_t)x * cx.maxc];
                int st = cx.start[x], n = cx.count[x];
                for (int c = 0; c < ch; c++) {
                    float acc = 0;
                    for (int i = 0; i < n; i++) acc += w[i] * row[(size_t)(st + i) * ch + c];
                    o[(size_t)x * ch + c] = clamp8(acc);
                }
            }
        }
    }
    if (dh == s.h) return mid;
    Img out(dw, dh, ch);
    Coeffs cy = make_coeffs(s.h, dh, lanczos);
    for (int y = 0; y < dh; y++) {
        const float* w = &cy.w[(size_t)y * cy.maxc];
        int st = cy.start[y], n = cy.count[y];
        uint8_t* o = &out.d[(size_t)y * dw * ch];
        for (int x = 0; x < dw * ch; x++) {
            float acc = 0;
            for (int i = 0; i < n; i++) acc += w[i] * mid.d[(size_t)(st + i) * dw * ch + x];
            o[x] = clamp8(acc);
        }
    }
    return out;
}

static Img gaussian_blur(const Img& s, double sigma) {
    int r = std::max(1, (int)std::ceil(sigma * 3.0));
    std::vector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; i++) { k[i + r] = (float)std::exp(-(i * i) / (2.0 * sigma * sigma)); sum += k[i + r]; }
    for (auto& v : k) v /= sum;
    const int ch = s.ch;
    std::vector<float> tmp((size_t)s.w * s.h * ch);
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++)
            for (int c = 0; c < ch; c++) {
                float acc = 0;
                for (int i = -r; i <= r; i++) {
                    int xx = std::min(std::max(x + i, 0), s.w - 1);
                    acc += k[i + r] * s.d[((size_t)y * s.w + xx) * ch + c];
                }
                tmp[((size_t)y * s.w + x) * ch + c] = acc;
            }
    Img out(s.w, s.h, ch);
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++)
            for (int c = 0; c < ch; c++) {
                float acc = 0;
                for (int i = -r; i <= r; i++) {
                    int yy = std::min(std::max(y + i, 0), s.h - 1);
                    acc += k[i + r] * tmp[((size_t)yy * s.w + x) * ch + c];
                }
                out.d[((size_t)y * s.w + x) * ch + c] = clamp8(acc);
            }
    return out;
}

static Img cover(const Img& img, int w, int h) {
    double scale = std::max((double)w / img.w, (double)h / img.h);
    int nw = std::max(w, (int)std::ceil(img.w * scale));
    int nh = std::max(h, (int)std::ceil(img.h * scale));
    Img big = resample(img, nw, nh, true);
    int left = (nw - w) / 2, top = (nh - h) / 2;
    Img out(w, h, img.ch);
    for (int y = 0; y < h; y++)
        memcpy(&out.d[(size_t)y * w * img.ch], &big.d[((size_t)(y + top) * nw + left) * img.ch], (size_t)w * img.ch);
    return out;
}

static Img fit_img(const Img& img, int w, int h, int mode) {
    if (mode == FIT_STRETCH) return resample(img, w, h, true);
    if (mode == FIT_COVER) return cover(img, w, h);
    double scale = std::min((double)w / img.w, (double)h / img.h);
    return resample(img, std::max(1, (int)std::nearbyint(img.w * scale)),
                         std::max(1, (int)std::nearbyint(img.h * scale)), true);
}

// Тайл RGB tw x th: изображение по центру, по краям размытая подложка.
static Img render_tile(const Img& img, int tw, int th, int mode, double margin) {
    int box_w = std::max(2, (int)std::nearbyint(tw * (1.0 - 2.0 * margin)));
    int box_h = std::max(2, (int)std::nearbyint(th * (1.0 - 2.0 * margin)));
    Img fg = fit_img(img, box_w, box_h, mode);
    if (fg.w >= tw - 1 && fg.h >= th - 1) {
        if (fg.w != tw || fg.h != th) fg = resample(fg, tw, th, true);
        return fg;
    }
    int sw = std::max(32, tw / 4), sh = std::max(32, th / 4);
    Img bg = cover(img, sw, sh);
    bg = gaussian_blur(bg, std::max(2.0, std::max(8, std::max(tw, th) / 24) / 4.0));
    Img canvas = resample(bg, tw, th, false);
    for (auto& v : canvas.d) v = (uint8_t)(int)(v * 0.55);
    int ox = (tw - fg.w) / 2, oy = (th - fg.h) / 2;
    for (int y = 0; y < fg.h; y++)
        memcpy(&canvas.d[((size_t)(y + oy) * tw + ox) * 3], &fg.d[(size_t)y * fg.w * 3], (size_t)fg.w * 3);
    return canvas;
}

// ── планирование ─────────────────────────────────────────────────────────────
static std::vector<int> effective_delays(const std::vector<int>& raw) {
    std::vector<int> r;
    for (int d : raw) r.push_back(d <= MIN_GIF_DELAY_MS ? FALLBACK_DELAY_MS : d);
    return r;
}

static int suggest_fps(const std::vector<double>& delays) {
    std::vector<double> bounds; double acc = 0;
    for (double d : delays) { acc += d; bounds.push_back(acc); }
    for (int fps = 1; fps <= USER_MAX_FPS; fps++) {
        double worst = 0;
        for (double b : bounds) {
            double x = b * fps / 1000.0;
            worst = std::max(worst, std::fabs(x - std::nearbyint(x)));
        }
        if (worst / fps * 1000.0 <= 1.5) return fps;
    }
    return DEFAULT_MAX_FPS;
}

static std::vector<int> plan_timeline(const std::vector<double>& delays, int fps, bool pingpong) {
    std::vector<double> starts; double acc = 0;
    for (double d : delays) { starts.push_back(acc); acc += d; }
    int n_slots = std::max(1, (int)std::nearbyint(acc * fps / 1000.0));
    std::vector<int> slots;
    for (int k = 0; k < n_slots; k++) {
        double t = (k + 0.5) * 1000.0 / fps;
        int i = (int)(std::upper_bound(starts.begin(), starts.end(), t) - starts.begin()) - 1;
        slots.push_back(std::min(std::max(i, 0), (int)delays.size() - 1));
    }
    if (pingpong && slots.size() > 2) {
        std::vector<int> ext = slots;
        for (int i = (int)slots.size() - 2; i >= 1; i--) ext.push_back(slots[i]);
        slots = ext;
    }
    return slots;
}

struct Digest { uint64_t a, b; bool operator<(const Digest& o) const { return a != o.a ? a < o.a : b < o.b; } };

static void build_plan(const std::vector<int>& slots, const std::vector<Digest>& digests,
                       std::vector<int>& stored, std::vector<int>& timeline) {
    std::map<Digest, int> shape_of;
    stored.clear(); timeline.clear();
    for (int src : slots) {
        auto it = shape_of.find(digests[src]);
        if (it == shape_of.end()) {
            int id = (int)stored.size();
            shape_of[digests[src]] = id;
            stored.push_back(src);
            timeline.push_back(id);
        } else timeline.push_back(it->second);
    }
}

static int frame_height(int tw, double aspect) {
    return std::max(2, (int)std::nearbyint(tw / aspect / 2.0) * 2);
}

static bool best_grid(int n, int tw, int th, int& bc, int& br) {
    bool found = false; long bk1 = 0, bk2 = 0;
    for (int cols = 1; cols <= n; cols++) {
        int rows = (n + cols - 1) / cols;
        if ((long)cols * tw > PAGE_MAX_DIM || (long)rows * th > PAGE_MAX_DIM) continue;
        long k1 = (long)cols * rows - n, k2 = std::max((long)cols * tw, (long)rows * th);
        if (!found || k1 < bk1 || (k1 == bk1 && k2 < bk2)) { found = true; bk1 = k1; bk2 = k2; bc = cols; br = rows; }
    }
    return found;
}

static int frames_per_page_for(int tw, int th) {
    int c, r;
    for (int n = FRAMES_PER_PAGE; n >= 1; n--) if (best_grid(n, tw, th, c, r)) return n;
    return 1;
}
static int pages_needed(int n, int tw, int th) {
    int f = frames_per_page_for(tw, th);
    return (n + f - 1) / f;
}
static double estimate_raw(int n, int tw, int th) {
    int fpp = frames_per_page_for(tw, th);
    double total = 0; int left = n;
    while (left > 0) {
        int k = std::min(fpp, left), c, r;
        if (!best_grid(k, tw, th, c, r)) return 1e18;
        total += (double)c * tw * r * th * 4;
        left -= k;
    }
    return total;
}
static int source_tile_cap(int sw, int sh) {
    int side = std::max(sw, sh), cap = 0;
    for (int i = N_TILE_CAND - 1; i >= 0; i--) if (TILE_CANDIDATES[i] >= side) { cap = TILE_CANDIDATES[i]; break; }
    if (!cap) cap = TILE_CANDIDATES[0];
    return std::max(cap, MIN_AUTO_TILE);
}
static int pick_tile(int n, int cap, double aspect, double budget) {
    for (int i = 0; i < N_TILE_CAND; i++) {
        int t = TILE_CANDIDATES[i];
        if (t > cap) continue;
        int th = frame_height(t, aspect);
        if (estimate_raw(n, t, th) <= budget && pages_needed(n, t, th) <= MAX_PAGES) return t;
    }
    return TILE_CANDIDATES[N_TILE_CAND - 1];
}

// ── FlatBuffers-секции ───────────────────────────────────────────────────────
struct Layout { double half_w, half_h; int u0, v0, u1, v1; };

static std::vector<uint8_t> bytes_of(FBBuilder& b) { return std::vector<uint8_t>(b.data(), b.data() + b.size()); }
static void put_u32(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 4); }

static std::vector<uint8_t> build_export_names_chunk(int movieclip_id, uint32_t name_ref_id = 1) {
    FBBuilder b(128);
    b.startVector(2, 1, 2);  b.prependU16((uint16_t)movieclip_id);  size_t oid = b.endVector();
    b.startVector(4, 1, 4);  b.prependU32(name_ref_id);             size_t nrf = b.endVector();
    b.startObject(2);
    b.prependUOffsetSlot(1, nrf, 0);
    b.prependUOffsetSlot(0, oid, 0);
    size_t root = b.endObject();
    b.finish(root);
    std::vector<uint8_t> out; put_u32(out, (uint32_t)b.size());
    out.insert(out.end(), b.data(), b.data() + b.size());
    return out;
}

static std::vector<uint8_t> build_data_storage(const std::vector<Layout>& layouts, const std::vector<int>& timeline,
                                               const std::string& export_name) {
    size_t n_shapes = layouts.size(), n_slots = timeline.size();
    FBBuilder b(1024 + n_shapes * 64 + n_slots * 8);
    size_t s_empty = b.createString("");
    size_t s_name = b.createString(export_name);
    b.startVector(4, 2, 4);
    b.prependUOffset(s_name);
    b.prependUOffset(s_empty);
    size_t strings_vec = b.endVector();

    std::vector<uint8_t> vb;
    auto pk = [&](float x, float y, uint16_t u, uint16_t v) {
        vb.insert(vb.end(), (uint8_t*)&x, (uint8_t*)&x + 4);
        vb.insert(vb.end(), (uint8_t*)&y, (uint8_t*)&y + 4);
        vb.insert(vb.end(), (uint8_t*)&u, (uint8_t*)&u + 2);
        vb.insert(vb.end(), (uint8_t*)&v, (uint8_t*)&v + 2);
    };
    for (auto& L : layouts) {
        float hw = (float)L.half_w, hh = (float)L.half_h;
        pk(-hw, -hh, (uint16_t)L.u0, (uint16_t)L.v0);
        pk(-hw,  hh, (uint16_t)L.u0, (uint16_t)L.v1);
        pk( hw, -hh, (uint16_t)L.u1, (uint16_t)L.v0);
        pk( hw,  hh, (uint16_t)L.u1, (uint16_t)L.v1);
    }
    size_t bitmap_vec = b.createByteVector(vb.data(), vb.size());

    b.startVector(2, n_slots * 3, 2);
    for (size_t k = n_slots; k-- > 0;) {
        b.prependU16(0xFFFF);
        b.prependU16(0xFFFF);
        b.prependU16((uint16_t)timeline[k]);
    }
    size_t frame_elems = b.endVector();

    b.startVector(24, 0, 4); size_t matrices = b.endVector();
    b.startVector(7, 0, 1);  size_t colors = b.endVector();
    b.startObject(3);
    b.prependUOffsetSlot(0, matrices, 0);
    b.prependUOffsetSlot(1, colors, 0);
    size_t bank = b.endObject();
    b.startVector(4, 1, 4);
    b.prependUOffset(bank);
    size_t banks = b.endVector();

    b.startObject(7);
    b.prependUOffsetSlot(0, strings_vec, 0);
    b.prependUOffsetSlot(4, frame_elems, 0);
    b.prependUOffsetSlot(5, bitmap_vec, 0);
    b.prependUOffsetSlot(6, banks, 0);
    size_t root = b.endObject();
    b.finish(root);
    return bytes_of(b);
}

static std::vector<uint8_t> build_shapes(const std::vector<int>& page_idx) {
    size_t n = page_idx.size();
    FBBuilder b(1024 + n * 32);
    std::vector<size_t> offs;
    for (size_t i = 0; i < n; i++) {
        b.startVector(16, 1, 4);
        b.prep(4, 16);
        b.prependU32((uint32_t)(i * 4));
        b.prependU32(4);
        b.prependU32((uint32_t)page_idx[i]);
        b.prependU32(0);
        size_t cmds = b.endVector();
        b.startObject(2);
        b.prependU16Slot(0, (uint16_t)i, 0);
        b.prependUOffsetSlot(1, cmds, 0);
        offs.push_back(b.endObject());
    }
    b.startVector(4, n, 4);
    for (size_t i = n; i-- > 0;) b.prependUOffset(offs[i]);
    size_t shapes_vec = b.endVector();
    b.startObject(1);
    b.prependUOffsetSlot(0, shapes_vec, 0);
    size_t root = b.endObject();
    b.finish(root);
    return bytes_of(b);
}

static std::vector<uint8_t> build_movieclips(int n_shapes, int n_slots, int framerate, int movieclip_id) {
    FBBuilder b(1024 + (size_t)n_shapes * 8 + (size_t)n_slots * 16);
    b.startVector(2, n_shapes, 2);
    for (int i = n_shapes; i-- > 0;) b.prependU16((uint16_t)i);
    size_t children = b.endVector();
    b.startVector(1, n_shapes, 1);
    for (int i = 0; i < n_shapes; i++) b.prependU8(0);
    size_t blending = b.endVector();
    b.startVector(8, n_slots, 4);
    for (int i = 0; i < n_slots; i++) { b.prep(4, 8); b.prependU32(0); b.prependU32(1); }
    size_t frames = b.endVector();

    b.startObject(13);
    b.prependU16Slot(0, (uint16_t)movieclip_id, 0);
    b.prependU32Slot(1, 1, -1);
    b.prependU8Slot(2, (uint8_t)framerate, 0);
    b.prependU16Slot(3, (uint16_t)n_slots, 0);
    b.prependU8Slot(4, 0, 0);
    b.prependUOffsetSlot(5, children, 0);
    b.prependUOffsetSlot(7, blending, 0);
    b.prependUOffsetSlot(8, frames, 0);
    b.prependU32Slot(9, 0, 4294967295LL);
    b.prependU32Slot(10, 0, 0);
    size_t clip = b.endObject();
    b.startVector(4, 1, 4);
    b.prependUOffset(clip);
    size_t clips = b.endVector();
    b.startObject(1);
    b.prependUOffsetSlot(0, clips, 0);
    size_t root = b.endObject();
    b.finish(root);
    return bytes_of(b);
}

static std::vector<uint8_t> build_file_descriptor(const std::string& export_name, uint32_t shape_count,
        uint32_t movieclip_count, uint32_t texture_count, uint32_t resources_offset,
        uint32_t textures_length, uint32_t compressed_size) {
    FBBuilder b(512);
    size_t name = b.createString(export_name);
    b.startObject(2);
    b.prependUOffsetSlot(0, name, 0);
    size_t exp = b.endObject();
    b.startVector(4, 1, 4);
    b.prependUOffset(exp);
    size_t exports = b.endVector();

    b.startObject(13);
    b.prependU32Slot(0, 1, 0);
    b.prependU32Slot(1, 1, 0);
    b.prependU32Slot(2, shape_count, 0);
    b.prependU32Slot(3, movieclip_count, 0);
    b.prependU32Slot(4, texture_count, 0);
    b.prependU32Slot(5, 0, 0);
    b.prependU32Slot(6, 0, 0);
    b.prependU32Slot(7, 0, 0);
    b.prependU32Slot(8, resources_offset, 0);
    b.prependU32Slot(9, textures_length, 0);
    b.prependUOffsetSlot(10, exports, 0);
    b.prependU32Slot(11, compressed_size, 0);
    b.prependU32Slot(12, 0, 0);
    size_t root = b.endObject();
    b.finish(root);
    return bytes_of(b);
}

static const uint8_t EMPTY_CHUNK[16] = {12,0,0,0, 8,0,0,0, 4,0,4,0, 4,0,0,0};  // TextFields / Modifiers (пустые)

static std::string unique_path(const std::string& dir, const std::string& stem, const std::string& ext) {
    struct stat st; std::string cand = stem; int n = 2;
    while (stat((dir + "/" + cand + ext).c_str(), &st) == 0) cand = stem + "_" + std::to_string(n++);
    return cand;
}

static std::string base_no_ext(const std::string& p) {
    size_t s = p.find_last_of('/');
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}


static void zstd_feed(ZSTD_CCtx* cctx, std::vector<uint8_t>& out, const uint8_t* p, size_t n, bool last) {
    std::vector<uint8_t> tmp(1 << 20);
    ZSTD_inBuffer in = {p, n, 0};
    ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
    size_t rem;
    do {
        ZSTD_outBuffer ob = {tmp.data(), tmp.size(), 0};
        rem = ZSTD_compressStream2(cctx, &ob, &in, mode);
        if (ZSTD_isError(rem)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(rem));
        out.insert(out.end(), tmp.begin(), tmp.begin() + ob.pos);
    } while (last ? (rem != 0) : (in.pos < in.size));
}

static void rgba_page_write(uint8_t* dst, int pw, const Img& tile, int left, int top) {
    for (int y = 0; y < tile.h; y++) {
        uint8_t* o = dst + ((size_t)(top + y) * pw + left) * 4;
        const uint8_t* s = &tile.d[(size_t)y * tile.w * 3];
        for (int x = 0; x < tile.w; x++) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; o += 4; s += 3; }
    }
}

static Digest digest_of(const uint8_t* p, size_t n) {
    uint64_t a = 1469598103934665603ULL, b = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < n; i++) {
        a = (a ^ p[i]) * 1099511628211ULL;
        b = (b + p[i] + 1) * 0xBF58476D1CE4E5B9ULL; b ^= b >> 29;
    }
    return Digest{a, b ^ n};
}

struct GifMeta {
    int w = 0, h = 0, n = 0;
    std::vector<uint8_t> px;           // n кадров RGBA
    std::vector<int> delays;
    std::vector<Digest> digests;
    bool has_alpha = false;
};

static Img flatten_frame(const GifMeta& m, int idx, const int rgb[3]) {
    const uint8_t* f = &m.px[(size_t)m.w * m.h * 4 * idx];
    Img src(m.w, m.h, 3);
    for (size_t k = 0, npx = (size_t)m.w * m.h; k < npx; k++) {
        const uint8_t* s = f + k * 4; uint8_t* d = &src.d[k * 3];
        if (m.has_alpha) {
            int a = s[3];
            d[0] = (uint8_t)((s[0] * a + rgb[0] * (255 - a) + 127) / 255);
            d[1] = (uint8_t)((s[1] * a + rgb[1] * (255 - a) + 127) / 255);
            d[2] = (uint8_t)((s[2] * a + rgb[2] * (255 - a) + 127) / 255);
        } else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    }
    return src;
}

static void draw_rect(Img& im, double x0, double y0, double x1, double y1, const uint8_t col[3], int width) {
    for (int t = 0; t < width; t++) {
        int ax = (int)std::lround(x0) + t, ay = (int)std::lround(y0) + t;
        int bx = (int)std::lround(x1) - t, by = (int)std::lround(y1) - t;
        for (int x = ax; x <= bx; x++) for (int y : {ay, by})
            if (x >= 0 && x < im.w && y >= 0 && y < im.h) memcpy(&im.d[((size_t)y * im.w + x) * 3], col, 3);
        for (int y = ay; y <= by; y++) for (int x : {ax, bx})
            if (x >= 0 && x < im.w && y >= 0 && y < im.h) memcpy(&im.d[((size_t)y * im.w + x) * 3], col, 3);
    }
}

}  // namespace gifsc

using namespace gifsc;

std::string gif2sc_impl(const std::string& gif_path, const std::string& export_name_in, int fps_req,
                        int screen_mode, int bg_rgb, const std::string& out_dir, double raw_budget, int tex_format) {
    std::vector<uint8_t> file;
    if (!sc_read_file(gif_path, file)) return "Error: cannot read GIF: " + gif_path;

    GifMeta m;
    {
        int* delays = nullptr; int x = 0, y = 0, z = 0, comp = 0;
        uint8_t* data = stbi_load_gif_from_memory(file.data(), (int)file.size(), &delays, &x, &y, &z, &comp, 4);
        if (!data) return "Error: не удалось открыть файл как GIF";
        if (z < 2) { stbi_image_free(data); if (delays) stbi_image_free(delays);
                     return "Error: в файле один кадр, это не анимация. Нужен GIF с несколькими кадрами"; }
        if ((double)x * y * z * 4 > 700e6) {
            stbi_image_free(data); if (delays) stbi_image_free(delays);
            return "Error: GIF слишком большой для обработки на телефоне (" + std::to_string(x) + "x" + std::to_string(y) +
                   ", кадров " + std::to_string(z) + ")";
        }
        m.w = x; m.h = y; m.n = z;
        size_t fsz = (size_t)x * y * 4;
        m.px.assign(data, data + fsz * z);
        for (int i = 0; i < z; i++) m.delays.push_back(delays ? delays[i] : 0);
        stbi_image_free(data); if (delays) stbi_image_free(delays);
        for (int i = 0; i < z; i++) {
            const uint8_t* f = &m.px[fsz * i];
            if (!m.has_alpha) for (size_t k = 3; k < fsz; k += 4) if (f[k] < 255) { m.has_alpha = true; break; }
            m.digests.push_back(digest_of(f, fsz));
        }
    }
    file.clear(); file.shrink_to_fit();

    std::string export_name = export_name_in.empty() ? "bgr_anim" : export_name_in;
    int rgb[3] = {0, 0, 0};
    if (m.has_alpha && bg_rgb >= 0) { rgb[0] = (bg_rgb >> 16) & 255; rgb[1] = (bg_rgb >> 8) & 255; rgb[2] = bg_rgb & 255; }

    const int* frame_res = (screen_mode == 2) ? SCREEN_1848 : SCREEN_2048;
    double aspect = (double)frame_res[0] / frame_res[1];
    int fit_mode = (std::fabs(((double)m.w / m.h) / aspect - 1.0) <= AUTO_STRETCH_TOL) ? FIT_STRETCH : FIT_CONTAIN;
    double margin = std::max(0.0, std::min(AUTO_MARGIN_PCT, 20.0)) / 100.0;
    bool pingpong = true;

    std::vector<int> eff = effective_delays(m.delays);
    std::vector<double> delays; for (int d : eff) delays.push_back((double)d);
    int suggested = suggest_fps(delays);
    int fps = fps_req > 0 ? std::min(fps_req, USER_MAX_FPS) : suggested;

    std::vector<int> slots, stored, timeline;
    for (;;) {
        slots = plan_timeline(delays, fps, pingpong);
        build_plan(slots, m.digests, stored, timeline);
        if ((int)slots.size() > MAX_TIMELINE || (int)stored.size() > MAX_SHAPES) {
            if (fps <= 1) return "Error: анимация слишком длинная или в ней слишком много разных кадров";
            if (fps_req > 0)
                return "Error: при " + std::to_string(fps) + " кадр/с получается " + std::to_string(slots.size()) +
                       " слотов и " + std::to_string(stored.size()) + " разных кадров (максимум " +
                       std::to_string(MAX_TIMELINE) + " и " + std::to_string(MAX_SHAPES) + "). Выберите FPS поменьше";
            double k1 = (double)MAX_TIMELINE / slots.size(), k2 = (double)MAX_SHAPES / stored.size();
            fps = std::max(1, (int)(fps * std::min(k1, k2) * 0.9));
            continue;
        }
        break;
    }
    int n_shapes = (int)stored.size(), n_slots = (int)timeline.size();

    int cap = source_tile_cap(m.w, m.h);
    int tile = pick_tile(n_shapes, cap, aspect, raw_budget);
    int tile_h = frame_height(tile, aspect);
    double est = estimate_raw(n_shapes, tile, tile_h);
    if (est > HARD_RAW_LIMIT) return "Error: текстуры слишком большие, уменьшите FPS или длительность";

    int fpp = frames_per_page_for(tile, tile_h);
    int n_pages = (n_shapes + fpp - 1) / fpp;
    struct PageInfo { int first, count, cols, rows, pw, ph; };
    std::vector<PageInfo> pages;
    std::vector<Layout> layouts; std::vector<int> shape_page;
    double total_payload = 0;
    int inset = std::min(tile, tile_h) >= 128 ? UV_INSET_PX : 1;
    for (int p = 0; p < n_pages; p++) {
        int first = p * fpp, cnt = std::min(fpp, n_shapes - first), c = 0, r = 0;
        if (!best_grid(cnt, tile, tile_h, c, r)) return "Error: не удалось разложить кадры по текстурам";
        PageInfo pi{first, cnt, c, r, c * tile, r * tile_h};
        pages.push_back(pi);
        if (tex_format == 1) total_payload += (double)pi.pw * pi.ph * 4;
        else total_payload += 68.0 + (double)((pi.pw + 5) / 6) * ((pi.ph + 5) / 6) * 16;
        for (int i = 0; i < cnt; i++) {
            int col = i % c, row = i / c, left = col * tile, top = row * tile_h;
            Layout L;
            L.half_w = QUAD_WIDTH / 2.0;
            L.half_h = QUAD_WIDTH * tile_h / tile / 2.0;
            L.u0 = (int)std::nearbyint((double)(left + inset) / pi.pw * 65535);
            L.v0 = (int)std::nearbyint((double)(top + inset) / pi.ph * 65535);
            L.u1 = (int)std::nearbyint((double)(left + tile - inset) / pi.pw * 65535);
            L.v1 = (int)std::nearbyint((double)(top + tile_h - inset) / pi.ph * 65535);
            layouts.push_back(L); shape_page.push_back(p);
        }
    }
    if (total_payload > HARD_RAW_LIMIT) return "Error: текстуры весят больше предела flatbuffers (2 ГБ)";

    std::vector<uint8_t> ds = build_data_storage(layouts, timeline, export_name);
    std::vector<uint8_t> shapes = build_shapes(shape_page);
    int movieclip_id = n_shapes;
    std::vector<uint8_t> mc = build_movieclips(n_shapes, n_slots, fps, movieclip_id);
    std::vector<uint8_t> exp_chunk = build_export_names_chunk(movieclip_id);

    std::vector<uint8_t> tex_bytes;
    {
        FBBuilder tb((size_t)total_payload + 4096 + (size_t)n_pages * 256);
        std::vector<size_t> set_offs;
        for (int p = 0; p < n_pages; p++) {
            const PageInfo& pi = pages[p];
            uint8_t* dst = nullptr;
            const size_t raw_len = (size_t)pi.pw * pi.ph * 4;
            std::vector<uint8_t> pagebuf;
            size_t data_off = 0;
            if (tex_format == 1) {
                data_off = tb.createByteVector(nullptr, raw_len, &dst);
                memset(dst, 0, raw_len);
            } else {
                pagebuf.assign(raw_len, 0);
                dst = pagebuf.data();
            }
            for (int i = 0; i < pi.count; i++) {
                Img src = flatten_frame(m, stored[pi.first + i], rgb);
                Img t = render_tile(src, tile, tile_h, fit_mode, margin);
                rgba_page_write(dst, pi.pw, t, (i % pi.cols) * tile, (i / pi.cols) * tile_h);
            }
            if (tex_format != 1) {
                // ASTC 6x6 в контейнере KTX — как в оригинальных файлах игры (формат 8)
                std::vector<uint8_t> astc;
                if (!astc_encode_fast(pagebuf.data(), pi.pw, pi.ph, 6, 6, astc)) return "Error: ASTC encode failed";
                pagebuf.clear(); pagebuf.shrink_to_fit();
                uint8_t* kd = nullptr;
                data_off = tb.createByteVector(nullptr, 68 + astc.size(), &kd);
                static const uint8_t KTX_MAGIC[12] = {0xAB,'K','T','X',' ','1','1',0xBB,0x0D,0x0A,0x1A,0x0A};
                memcpy(kd, KTX_MAGIC, 12);
                uint32_t hdr[14] = {0x04030201u, 0, 1, 0, 0x93B4u, 0x1908u, (uint32_t)pi.pw, (uint32_t)pi.ph,
                                    0, 0, 1, 1, 0, (uint32_t)astc.size()};
                memcpy(kd + 12, hdr, sizeof hdr);
                memcpy(kd + 68, astc.data(), astc.size());
            }
            tb.startObject(6);
            tb.prependU8Slot(0, tex_format == 1 ? 0 : 8, 0);
            tb.prependU8Slot(1, 0, 0);
            tb.prependU16Slot(2, (uint16_t)pi.pw, 0);
            tb.prependU16Slot(3, (uint16_t)pi.ph, 0);
            tb.prependUOffsetSlot(4, data_off, 0);
            size_t texdata = tb.endObject();
            tb.startObject(2);
            tb.prependUOffsetSlot(1, texdata, 0);
            set_offs.push_back(tb.endObject());
        }
        tb.startVector(4, set_offs.size(), 4);
        for (size_t i = set_offs.size(); i-- > 0;) tb.prependUOffset(set_offs[i]);
        size_t tv = tb.endVector();
        tb.startObject(1);
        tb.prependUOffsetSlot(0, tv, 0);
        size_t root = tb.endObject();
        tb.finish(root);
        tex_bytes.assign(tb.data(), tb.data() + tb.size());
    }

    // превью: кадр 0 с рамками, показывающими видимую область на каждом экране
    std::string preview_stem;
    {
        int pw = 1280, ph = std::max(2, (int)std::nearbyint((double)pw * tile_h / tile));
        Img pv = render_tile(flatten_frame(m, 0, rgb), pw, ph, fit_mode, margin);
        const int* scr[2] = {SCREEN_2048, SCREEN_1848};
        const uint8_t pal[2][3] = {{0, 255, 0}, {255, 200, 0}};
        for (int n = 0; n < 2; n++) {
            double sa = (double)scr[n][0] / scr[n][1], fa = (double)pw / ph;
            double vw = fa > sa ? ph * sa : pw, vh = fa > sa ? ph : pw / sa;
            double x0 = (pw - vw) / 2.0, y0 = (ph - vh) / 2.0;
            draw_rect(pv, x0 + n, y0 + n, x0 + vw - 1 - n, y0 + vh - 1 - n, pal[n], 3);
        }
        preview_stem = unique_path(out_dir, base_no_ext(gif_path) + "_preview", ".png");
        stbi_write_png((out_dir + "/" + preview_stem + ".png").c_str(), pw, ph, 3, pv.d.data(), pw * 3);
    }
    m.px.clear(); m.px.shrink_to_fit();

    std::vector<uint8_t> h_ds, h_sh, h_mc, h_tx;
    put_u32(h_ds, (uint32_t)ds.size());
    put_u32(h_sh, (uint32_t)shapes.size());
    put_u32(h_mc, (uint32_t)mc.size());
    put_u32(h_tx, (uint32_t)tex_bytes.size());
    uint32_t resources_offset = 4 + (uint32_t)ds.size();
    size_t inner_len = h_ds.size() + ds.size() + exp_chunk.size() + 16 + h_sh.size() + shapes.size() +
                       h_mc.size() + mc.size() + 16 + h_tx.size() + tex_bytes.size();
    std::vector<uint8_t> compressed;
    try {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, ZSTD_LEVEL);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);
        ZSTD_CCtx_setPledgedSrcSize(cctx, inner_len);
        zstd_feed(cctx, compressed, h_ds.data(), h_ds.size(), false);
        zstd_feed(cctx, compressed, ds.data(), ds.size(), false);
        zstd_feed(cctx, compressed, exp_chunk.data(), exp_chunk.size(), false);
        zstd_feed(cctx, compressed, EMPTY_CHUNK, 16, false);
        zstd_feed(cctx, compressed, h_sh.data(), h_sh.size(), false);
        zstd_feed(cctx, compressed, shapes.data(), shapes.size(), false);
        zstd_feed(cctx, compressed, h_mc.data(), h_mc.size(), false);
        zstd_feed(cctx, compressed, mc.data(), mc.size(), false);
        zstd_feed(cctx, compressed, EMPTY_CHUNK, 16, false);
        zstd_feed(cctx, compressed, h_tx.data(), h_tx.size(), false);
        zstd_feed(cctx, compressed, tex_bytes.data(), tex_bytes.size(), true);
        ZSTD_freeCCtx(cctx);
    } catch (std::exception& e) { return std::string("Error: ") + e.what(); }

    std::vector<uint8_t> fd = build_file_descriptor(export_name, n_shapes, 1, n_pages, resources_offset,
                                                   (uint32_t)tex_bytes.size() + 4, (uint32_t)compressed.size());
    std::vector<uint8_t> out;
    out.reserve(12 + fd.size() + compressed.size());
    out.push_back('S'); out.push_back('C');
    put_u32(out, 5);
    put_u32(out, (uint32_t)fd.size());
    out.insert(out.end(), fd.begin(), fd.end());
    out.insert(out.end(), compressed.begin(), compressed.end());
    compressed.clear(); compressed.shrink_to_fit();

    std::string stem = unique_path(out_dir, base_no_ext(gif_path), ".sc");
    std::string out_path = out_dir + "/" + stem + ".sc";
    if (!sc_write_file(out_path, out)) return "Error: write failed: " + out_path;

    char sz[32]; snprintf(sz, sizeof sz, "%.1f", (double)out.size() / 1048576.0);
    char du[32]; snprintf(du, sizeof du, "%.2f", (double)n_slots / fps);
    std::ostringstream r;
    r << "OK: GIF -> SC2 (" << m.w << "x" << m.h << ", " << m.n << " frames)\n";
    r << "  Slots: " << n_slots << " (" << du << " s @ " << fps << " fps)\n";
    r << "  Frames stored: " << n_shapes << ", tile " << tile << "x" << tile_h << ", textures " << n_pages
      << (tex_format == 1 ? " (RGBA)" : " (ASTC 6x6)") << " (~" << (int)(total_payload / 1e6) << " MB)\n";
    r << "  ExportName: " << export_name << "\n";
    r << "  Size: " << sz << " MB\n";
    r << "  Preview: " << preview_stem << ".png\n";
    r << "  Output: " << stem << ".sc\n";
    r << "  Dir: " << out_dir << "\n";
    r << "  Mod: \"FileName\": \"sc/" << stem << ".sc\", \"ExportName\": \"" << export_name << "\"";
    return r.str();
}

// ═════════════════════════════════════════════════════════════════════════════
//  SC2 → GIF
// ═════════════════════════════════════════════════════════════════════════════
bool sc2_decode_texture_rgba(const Sc2File& sc, size_t tex_index, int& w, int& h, std::vector<uint8_t>& rgba);

namespace gifsc {

struct Aff {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;   // x' = a*x + c*y + tx ; y' = b*x + d*y + ty
};
static Aff aff_mul(const Aff& p, const Aff& q) {       // сначала q, затем p
    Aff r;
    r.a = p.a * q.a + p.c * q.b;  r.c = p.a * q.c + p.c * q.d;
    r.b = p.b * q.a + p.d * q.b;  r.d = p.b * q.c + p.d * q.d;
    r.tx = p.a * q.tx + p.c * q.ty + p.tx;
    r.ty = p.b * q.tx + p.d * q.ty + p.ty;
    return r;
}

struct CT {                     // цветовая трансформация: out = tex*mul + add, alpha *= a
    float mr = 1, mg = 1, mb = 1, a = 1, ar = 0, ag = 0, ab = 0;
    bool identity() const { return mr == 1 && mg == 1 && mb == 1 && a == 1 && ar == 0 && ag == 0 && ab == 0; }
};
static CT ct_compose(const CT& p, const CT& c) {        // сначала c, затем p
    CT r;
    r.mr = c.mr * p.mr; r.mg = c.mg * p.mg; r.mb = c.mb * p.mb;
    r.ar = c.ar * p.mr + p.ar; r.ag = c.ag * p.mg + p.ag; r.ab = c.ab * p.mb + p.ab;
    r.a = c.a * p.a;
    return r;
}

struct Scene {
    const Sc2File& sc;
    std::map<uint16_t, size_t> shape_by_id, mc_by_id;
    std::vector<std::vector<size_t>> frame_start;      // для каждого MovieClip: индекс первого элемента кадра (в тройках)
    struct Tex { int w = 0, h = 0; std::vector<uint8_t> rgba; bool tried = false, ok = false; };
    std::vector<Tex> tex;
    std::map<uint16_t, int> mod_by_id;                 // id модификатора -> тип (38 маска, 39 в маске, 40 конец)
    std::function<void(int)> on_mod;                   // вызывается при встрече модификатора в кадре
    explicit Scene(const Sc2File& s) : sc(s) {
        for (size_t i = 0; i < sc.modifier_ids.size() && i < sc.modifier_types.size(); i++) mod_by_id[sc.modifier_ids[i]] = sc.modifier_types[i];
        for (size_t i = 0; i < sc.shapes.size(); i++) shape_by_id[sc.shapes[i].id] = i;
        for (size_t i = 0; i < sc.movieclips.size(); i++) mc_by_id[sc.movieclips[i].id] = i;
        size_t cursor = 0;
        frame_start.resize(sc.movieclips.size());
        for (size_t i = 0; i < sc.movieclips.size(); i++) {
            for (uint32_t cnt : sc.movieclips[i].frame_elem_counts) {
                frame_start[i].push_back(cursor);
                cursor += cnt;
            }
        }
        tex.resize(sc.textures.size());
    }
    Aff matrix_at(uint16_t idx) const {
        Aff m;
        if (idx == 0xFFFF || (size_t)(idx + 1) * 6 > sc.bank_matrices.size()) return m;
        const float* f = &sc.bank_matrices[(size_t)idx * 6];
        m.a = f[0]; m.b = f[1]; m.c = f[2]; m.d = f[3]; m.tx = f[4]; m.ty = f[5];
        return m;
    }
    CT color_at(uint16_t idx) const {
        CT c;
        if (idx == 0xFFFF || (size_t)(idx + 1) * 7 > sc.bank_colors.size()) return c;
        const uint8_t* b = &sc.bank_colors[(size_t)idx * 7];
        c.mr = b[0] / 255.f; c.mg = b[1] / 255.f; c.mb = b[2] / 255.f; c.a = b[3] / 255.f;
        c.ar = b[4] / 255.f; c.ag = b[5] / 255.f; c.ab = b[6] / 255.f;
        return c;
    }
    Tex& texture(size_t i) {
        Tex& t = tex[i];
        if (!t.tried) { t.tried = true; t.ok = sc2_decode_texture_rgba(sc, i, t.w, t.h, t.rgba); }
        return t;
    }
    // Самая длинная анимация среди клипа и всех вложенных: (число кадров, fps клипа с этой длиной)
    void longest_anim(size_t mc, int depth, std::set<size_t>& seen, int& frames, int& fps) {
        if (depth > 8 || mc >= sc.movieclips.size() || !seen.insert(mc).second) return;
        const Sc2MovieClip& c = sc.movieclips[mc];
        if ((int)c.frame_elem_counts.size() > frames) { frames = (int)c.frame_elem_counts.size(); fps = c.framerate; }
        for (uint16_t id : c.children_ids) {
            auto it = mc_by_id.find(id);
            if (it != mc_by_id.end()) longest_anim(it->second, depth + 1, seen, frames, fps);
        }
    }
    template <class F>
    void walk(size_t mc, size_t frame, const Aff& m, const CT& ct, int blend, int depth, F&& fn) {
        if (depth > 8 || mc >= sc.movieclips.size()) return;
        const Sc2MovieClip& clip = sc.movieclips[mc];
        if (frame_start[mc].empty()) return;
        const size_t abs_frame = frame;                       // вложенные клипы крутятся по общему времени
        frame = frame % frame_start[mc].size();
        size_t st = frame_start[mc][frame], cnt = clip.frame_elem_counts[frame];
        for (size_t k = 0; k < cnt; k++) {
            size_t e = (st + k) * 3;
            if (e + 3 > sc.frame_elements.size()) return;
            uint16_t child = sc.frame_elements[e], mi = sc.frame_elements[e + 1], ci = sc.frame_elements[e + 2];
            if (child >= clip.children_ids.size()) continue;
            uint16_t id = clip.children_ids[child];
            Aff cm = aff_mul(m, matrix_at(mi));
            CT cc = ct_compose(ct, color_at(ci));
            int cb = blend | (child < clip.children_blending.size() ? clip.children_blending[child] : 0);
            auto mdi = mod_by_id.find(id);
            if (mdi != mod_by_id.end()) { if (on_mod) on_mod(mdi->second); continue; }
            auto si = shape_by_id.find(id);
            if (si != shape_by_id.end()) { fn(sc.shapes[si->second], cm, cc, cb); continue; }
            auto mi2 = mc_by_id.find(id);
            if (mi2 != mc_by_id.end()) walk(mi2->second, abs_frame, cm, cc, cb, depth + 1, fn);
        }
    }
};

struct Pt { float x, y, u, v; };

// Вершины команды, отсортированные по углу в UV-пространстве (как и в редакторе).
static std::vector<Pt> ordered_poly(const Sc2Command& cmd, const Aff& m) {
    std::vector<Pt> p;
    for (auto& v : cmd.vertices) p.push_back({m.a * v.x + m.c * v.y + m.tx, m.b * v.x + m.d * v.y + m.ty, v.u, v.v});
    if (p.size() > 3) {
        float cu = 0, cv = 0;
        for (auto& q : p) { cu += q.u; cv += q.v; }
        cu /= p.size(); cv /= p.size();
        std::stable_sort(p.begin(), p.end(), [&](const Pt& a, const Pt& b) {
            return std::atan2(a.v - cv, a.u - cu) < std::atan2(b.v - cv, b.u - cu);
        });
    }
    return p;
}

static void sample_bilinear(const Scene::Tex& t, float u, float v, float out[4]) {
    float fx = u * t.w - 0.5f, fy = v * t.h - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - x0, ty = fy - y0;
    auto px = [&](int x, int y, int c) -> float {
        x = std::min(std::max(x, 0), t.w - 1); y = std::min(std::max(y, 0), t.h - 1);
        return (float)t.rgba[((size_t)y * t.w + x) * 4 + c];
    };
    float a3[4];
    for (int c = 0; c < 4; c++) a3[c] = 0;
    // интерполяция в премультиплицированном виде
    float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
    float al = px(x0, y0, 3) * w00 + px(x0 + 1, y0, 3) * w10 + px(x0, y0 + 1, 3) * w01 + px(x0 + 1, y0 + 1, 3) * w11;
    for (int c = 0; c < 3; c++) {
        a3[c] = px(x0, y0, c) * px(x0, y0, 3) * w00 + px(x0 + 1, y0, c) * px(x0 + 1, y0, 3) * w10 +
                px(x0, y0 + 1, c) * px(x0, y0 + 1, 3) * w01 + px(x0 + 1, y0 + 1, c) * px(x0 + 1, y0 + 1, 3) * w11;
    }
    out[3] = al / 255.f;
    for (int c = 0; c < 3; c++) out[c] = a3[c] / 255.f / 255.f;   // премульт 0..1
}

static void raster_tri(std::vector<float>& cv, int W, int H, Scene::Tex& tex, const CT& ct, bool additive, const float* mask,
                       const Pt& A, const Pt& B, const Pt& C) {
    float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
    if (std::fabs(area) < 1e-6f) return;
    int minx = std::max(0, (int)std::floor(std::min({A.x, B.x, C.x}) - 0.5f));
    int maxx = std::min(W - 1, (int)std::ceil(std::max({A.x, B.x, C.x}) + 0.5f));
    int miny = std::max(0, (int)std::floor(std::min({A.y, B.y, C.y}) - 0.5f));
    int maxy = std::min(H - 1, (int)std::ceil(std::max({A.y, B.y, C.y}) + 0.5f));
    float inv = 1.f / area;
    const float eps = -1e-4f;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((B.x - px) * (C.y - py) - (B.y - py) * (C.x - px)) * inv;
            float w1 = ((C.x - px) * (A.y - py) - (C.y - py) * (A.x - px)) * inv;
            float w2 = 1.f - w0 - w1;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            float u = w0 * A.u + w1 * B.u + w2 * C.u, v = w0 * A.v + w1 * B.v + w2 * C.v;
            float s[4];
            sample_bilinear(tex, u, v, s);
            if (!ct.identity() && s[3] > 0.f) {
                float r = s[0] / s[3], g = s[1] / s[3], b = s[2] / s[3];
                r = std::min(1.f, std::max(0.f, r * ct.mr + ct.ar));
                g = std::min(1.f, std::max(0.f, g * ct.mg + ct.ag));
                b = std::min(1.f, std::max(0.f, b * ct.mb + ct.ab));
                float a = s[3] * ct.a;
                s[0] = r * a; s[1] = g * a; s[2] = b * a; s[3] = a;
            }
            if (mask) { float ma = mask[((size_t)y * W + x) * 4 + 3]; s[0] *= ma; s[1] *= ma; s[2] *= ma; s[3] *= ma; }
            float* d = &cv[((size_t)y * W + x) * 4];
            if (additive) {     // свечение: цвет прибавляется к тому, что уже нарисовано
                d[0] += s[0]; d[1] += s[1]; d[2] += s[2]; d[3] = std::min(1.f, d[3] + s[3]);
                continue;
            }
            float ia = 1.f - s[3];
            d[0] = s[0] + d[0] * ia; d[1] = s[1] + d[1] * ia; d[2] = s[2] + d[2] * ia; d[3] = s[3] + d[3] * ia;
        }
    }
}

// ── квантование ──────────────────────────────────────────────────────────────
struct Quant {
    uint8_t pal[256][3];
    int ncol = 0;
    bool exact = false;
    int transparent = -1;
    std::map<uint32_t, int> exact_map;
    std::vector<uint8_t> lut;     // 32768 -> индекс
};

static void median_cut(const std::vector<uint32_t>& hist, int target, uint8_t out[][3], int& n) {
    struct Item { int key; uint32_t cnt; };
    std::vector<Item> items;
    for (size_t k = 0; k < hist.size(); k++) if (hist[k]) items.push_back({(int)k, hist[k]});
    struct Box { size_t lo, hi; };
    std::vector<Box> boxes{{0, items.size()}};
    auto chan = [](int key, int c) { return c == 0 ? (key >> 10) & 31 : (c == 1 ? (key >> 5) & 31 : key & 31); };
    while ((int)boxes.size() < target) {
        int bi = -1, bc = 0, br = 0;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (boxes[i].hi - boxes[i].lo < 2) continue;
            for (int c = 0; c < 3; c++) {
                int mn = 31, mx = 0;
                for (size_t k = boxes[i].lo; k < boxes[i].hi; k++) { int v = chan(items[k].key, c); mn = std::min(mn, v); mx = std::max(mx, v); }
                if (mx - mn > br) { br = mx - mn; bi = (int)i; bc = c; }
            }
        }
        if (bi < 0) break;
        Box b = boxes[bi];
        std::sort(items.begin() + b.lo, items.begin() + b.hi,
                  [&](const Item& x, const Item& y) { return chan(x.key, bc) < chan(y.key, bc); });
        uint64_t total = 0;
        for (size_t k = b.lo; k < b.hi; k++) total += items[k].cnt;
        uint64_t acc = 0; size_t mid = b.lo;
        for (size_t k = b.lo; k < b.hi; k++) { acc += items[k].cnt; mid = k + 1; if (acc * 2 >= total) break; }
        if (mid <= b.lo) mid = b.lo + 1;
        if (mid >= b.hi) mid = b.hi - 1;
        boxes[bi] = {b.lo, mid};
        boxes.push_back({mid, b.hi});
    }
    n = 0;
    for (auto& b : boxes) {
        double r = 0, g = 0, bl = 0, t = 0;
        for (size_t k = b.lo; k < b.hi; k++) {
            r += chan(items[k].key, 0) * (double)items[k].cnt; g += chan(items[k].key, 1) * (double)items[k].cnt;
            bl += chan(items[k].key, 2) * (double)items[k].cnt; t += items[k].cnt;
        }
        if (t <= 0) continue;
        auto e = [](double v) { int i = (int)std::lround(v); return (uint8_t)((i << 3) | (i >> 2)); };
        out[n][0] = e(r / t); out[n][1] = e(g / t); out[n][2] = e(bl / t); n++;
    }
}

static inline int key15(int r, int g, int b) { return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3); }

static void build_quant(Quant& q, const std::vector<std::vector<uint8_t>>& samples, bool want_transparent) {
    int target = want_transparent ? 255 : 256;
    std::map<uint32_t, int> distinct;
    bool overflow = false;
    std::vector<uint32_t> hist(32768, 0);
    for (auto& f : samples)
        for (size_t i = 0; i + 3 < f.size(); i += 4) {
            if (f[i + 3] < 128) continue;
            hist[key15(f[i], f[i + 1], f[i + 2])]++;
            if (!overflow) {
                uint32_t k = (f[i] << 16) | (f[i + 1] << 8) | f[i + 2];
                distinct[k] = 0;
                if ((int)distinct.size() > target) overflow = true;
            }
        }
    q.exact = !overflow;
    if (q.exact) {
        int n = 0;
        for (auto& kv : distinct) {
            q.pal[n][0] = (kv.first >> 16) & 255; q.pal[n][1] = (kv.first >> 8) & 255; q.pal[n][2] = kv.first & 255;
            q.exact_map[kv.first] = n; n++;
        }
        q.ncol = n;
    } else {
        median_cut(hist, target, q.pal, q.ncol);
    }
    if (q.ncol == 0) { q.pal[0][0] = q.pal[0][1] = q.pal[0][2] = 0; q.ncol = 1; }
    if (want_transparent) { q.transparent = q.ncol; q.pal[q.ncol][0] = q.pal[q.ncol][1] = q.pal[q.ncol][2] = 0; q.ncol++; }
    q.lut.assign(32768, 0);
    int search = want_transparent ? q.ncol - 1 : q.ncol;
    for (int k = 0; k < 32768; k++) {
        int r = ((k >> 10) & 31), g = ((k >> 5) & 31), b = k & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < search; i++) {
            int dr = r - q.pal[i][0], dg = g - q.pal[i][1], db = b - q.pal[i][2];
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) { bd = d; best = i; }
        }
        q.lut[k] = (uint8_t)best;
    }
}

static void quantize_frame(const Quant& q, const std::vector<uint8_t>& rgba, int W, int H, std::vector<uint8_t>& idx) {
    static const int bayer[8][8] = {
        { 0,32, 8,40, 2,34,10,42},{48,16,56,24,50,18,58,26},{12,44, 4,36,14,46, 6,38},{60,28,52,20,62,30,54,22},
        { 3,35,11,43, 1,33, 9,41},{51,19,59,27,49,17,57,25},{15,47, 7,39,13,45, 5,37},{63,31,55,23,61,29,53,21}};
    idx.assign((size_t)W * H, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* p = &rgba[((size_t)y * W + x) * 4];
            uint8_t* o = &idx[(size_t)y * W + x];
            if (p[3] < 128) { *o = (uint8_t)(q.transparent >= 0 ? q.transparent : 0); continue; }
            if (q.exact) {
                auto it = q.exact_map.find((p[0] << 16) | (p[1] << 8) | p[2]);
                if (it != q.exact_map.end()) { *o = (uint8_t)it->second; continue; }
                *o = q.lut[key15(p[0], p[1], p[2])];
                continue;
            }
            int i0 = q.lut[key15(p[0], p[1], p[2])];
            int er = p[0] - q.pal[i0][0], eg = p[1] - q.pal[i0][1], eb = p[2] - q.pal[i0][2];
            if (er * er + eg * eg + eb * eb <= 60) { *o = (uint8_t)i0; continue; }   // близко к палитре: без шума
            int d = (bayer[y & 7][x & 7] - 32) / 3;     // -10..10
            int r = std::min(255, std::max(0, p[0] + d));
            int g = std::min(255, std::max(0, p[1] + d));
            int b = std::min(255, std::max(0, p[2] + d));
            *o = q.lut[key15(r, g, b)];
        }
}

// ── запись GIF ───────────────────────────────────────────────────────────────
class GifOut {
public:
    explicit GifOut(const std::string& path) { f_ = fopen(path.c_str(), "wb"); }
    ~GifOut() { if (f_) fclose(f_); }
    bool ok() const { return f_ != nullptr; }
    void header(int W, int H, const Quant& q) {
        fwrite("GIF89a", 1, 6, f_);
        w16(W); w16(H);
        fputc(0xF7, f_); fputc(0, f_); fputc(0, f_);          // GCT 256, bg 0
        for (int i = 0; i < 256; i++) {
            if (i < q.ncol) fwrite(q.pal[i], 1, 3, f_);
            else { fputc(0, f_); fputc(0, f_); fputc(0, f_); }
        }
        const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0', 0x03, 0x01, 0x00, 0x00, 0x00};
        fwrite(loop, 1, sizeof loop, f_);
    }
    void frame(int W, int H, const std::vector<uint8_t>& idx, int delay_cs, int transparent) {
        fputc(0x21, f_); fputc(0xF9, f_); fputc(4, f_);
        fputc(transparent >= 0 ? (0x08 | 0x01) : 0x04, f_);   // disposal: 2 при прозрачности, иначе 1
        w16(delay_cs);
        fputc(transparent >= 0 ? transparent : 0, f_); fputc(0, f_);
        fputc(0x2C, f_); w16(0); w16(0); w16(W); w16(H); fputc(0, f_);
        lzw(idx);
    }
    void finish() { fputc(0x3B, f_); }

private:
    void w16(int v) { fputc(v & 255, f_); fputc((v >> 8) & 255, f_); }
    void lzw(const std::vector<uint8_t>& data) {
        const int minbits = 8;
        fputc(minbits, f_);
        std::vector<uint8_t> out; out.reserve(data.size() / 2);
        uint32_t cur = 0; int curbits = 0;
        auto emit = [&](int code, int nb) {
            cur |= (uint32_t)code << curbits; curbits += nb;
            while (curbits >= 8) { out.push_back(cur & 255); cur >>= 8; curbits -= 8; }
        };
        const int clear = 256, eoi = 257;
        std::vector<int32_t> table(5003 * 1, -1);
        std::vector<int32_t> tcode(5003), tprefix(5003); std::vector<uint8_t> tchar(5003);
        int nextcode = 258, nbits = 9;
        auto reset = [&]() { std::fill(table.begin(), table.end(), -1); nextcode = 258; nbits = 9; };
        emit(clear, nbits);
        int prefix = data[0];
        for (size_t i = 1; i < data.size(); i++) {
            uint8_t c = data[i];
            int h = ((int)c << 4) ^ prefix; h %= 5003; if (h < 0) h += 5003;
            int found = -1;
            while (table[h] != -1) {
                int e = table[h];
                if (tprefix[e] == prefix && tchar[e] == c) { found = tcode[e]; break; }
                h++; if (h >= 5003) h = 0;
            }
            if (found >= 0) { prefix = found; continue; }
            emit(prefix, nbits);
            if (nextcode < 4096) {
                table[h] = h; tprefix[h] = prefix; tchar[h] = c; tcode[h] = nextcode;
                nextcode++;
                if (nextcode > (1 << nbits) && nbits < 12) nbits++;
            } else {
                emit(clear, nbits);
                reset();
            }
            prefix = c;
        }
        emit(prefix, nbits);
        emit(eoi, nbits);
        if (curbits > 0) out.push_back(cur & 255);
        for (size_t p = 0; p < out.size(); p += 255) {
            size_t n = std::min<size_t>(255, out.size() - p);
            fputc((int)n, f_); fwrite(&out[p], 1, n, f_);
        }
        fputc(0, f_);
    }
    FILE* f_ = nullptr;
};

}  // namespace gifsc

using namespace gifsc;

std::string sc2gif_impl(const std::string& sc_path, const std::string& export_name, int max_side,
                        int fps_override, const std::string& out_dir) {
    std::vector<uint8_t> file;
    if (!sc_read_file(sc_path, file)) return "Error: cannot read SC: " + sc_path;
    if (file.size() < 3 || file[0] != 'S' || file[1] != 'C' || file[2] < 5)
        return "Error: нужен файл SC2 (версия 5 или 6)";
    Sc2File sc; std::string err;
    if (!sc2_parse(file, sc, &err)) return "Error: SC2 parse failed: " + err;
    file.clear(); file.shrink_to_fit();
    if (sc.frame_elements.empty()) return "Error: в файле нет данных анимации";

    Scene scene(sc);

    // ── выбор MovieClip ──
    int root_mc = -1; std::string root_name;
    if (!export_name.empty()) {
        for (auto& e : sc.exports)
            if (e.name == export_name) {
                auto it = scene.mc_by_id.find(e.object_id);
                if (it != scene.mc_by_id.end()) { root_mc = (int)it->second; root_name = e.name; }
                break;
            }
        if (root_mc < 0) return "Error: экспортнейм \"" + export_name + "\" не найден или это не MovieClip";
    } else {
        size_t best = 0;
        for (auto& e : sc.exports) {
            auto it = scene.mc_by_id.find(e.object_id);
            if (it == scene.mc_by_id.end()) continue;
            size_t fc = sc.movieclips[it->second].frame_elem_counts.size();
            if (fc > best) { best = fc; root_mc = (int)it->second; root_name = e.name; }
        }
        if (root_mc < 0 || best < 2) return "Error: в файле нет анимированных экспортов (укажите экспортнейм)";
    }
    const Sc2MovieClip& clip = sc.movieclips[root_mc];
    // у корня может быть один кадр, а анимация — во вложенных клипах: берём самую длинную
    int nframes = 0, nested_fps = 0;
    { std::set<size_t> seen; scene.longest_anim(root_mc, 0, seen, nframes, nested_fps); }
    if (nframes < 2) return "Error: у выбранного MovieClip один кадр";

    // ── границы по всем кадрам ──
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    bool any = false;
    std::vector<char> bstack;    // 1 = собираем маску, 0 = содержимое под маской (не расширяет границы)
    for (int f = 0; f < nframes; f++) {
        bstack.clear();
        scene.on_mod = [&](int t) {
            if (t == 38) bstack.push_back(1);
            else if (t == 39) { if (!bstack.empty()) bstack.back() = 0; }
            else if (t == 40) { if (!bstack.empty()) bstack.pop_back(); }
        };
        scene.walk(root_mc, f, Aff(), CT(), 0, 0, [&](const Sc2Shape& sh, const Aff& m, const CT&, int) {
            for (char c : bstack) if (!c) return;        // под активной маской
            for (auto& cmd : sh.commands) for (auto& v : cmd.vertices) {
                float x = m.a * v.x + m.c * v.y + m.tx, y = m.b * v.x + m.d * v.y + m.ty;
                minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y);
                any = true;
            }
        });
    }
    if (!any || maxx - minx < 1e-3f || maxy - miny < 1e-3f) return "Error: нет видимой графики в выбранной анимации";
    float bw = maxx - minx, bh = maxy - miny;
    float scale = max_side > 0 ? (float)max_side / std::max(bw, bh) : 1.0f;
    if (max_side <= 0 && std::max(bw, bh) > 2048.f) scale = 2048.f / std::max(bw, bh);
    int W = std::max(2, (int)std::lround(bw * scale)), H = std::max(2, (int)std::lround(bh * scale));
    if ((double)W * H > 6e6) {
        float k = std::sqrt(6e6f / ((float)W * H));
        W = std::max(2, (int)(W * k)); H = std::max(2, (int)(H * k));
    }
    // точная подгонка масштаба под целые размеры, чтобы у краёв не оставалось пустых пикселей
    const float sx = (float)W / bw, sy = (float)H / bh;

    // ── частота вывода: GIF-плееры не уважают задержки < 2 cs ──
    int fps = fps_override > 0 ? fps_override : (clip.framerate && clip.frame_elem_counts.size() > 1 ? clip.framerate : (nested_fps ? nested_fps : (clip.framerate ? clip.framerate : 30)));
    int step = 1;
    while (fps / step > 50) step++;
    double out_fps = (double)fps / step;

    std::vector<float> canvas((size_t)W * H * 4);
    // стек вложенных масок: cov — то, что нарисовано в маску; eff — итоговое покрытие с учётом внешних масок
    struct MaskFrame { std::vector<float> cov, eff; bool collecting = true; };
    std::vector<MaskFrame> mstack;
    auto render = [&](int f, std::vector<uint8_t>& rgba) {
        std::fill(canvas.begin(), canvas.end(), 0.f);
        mstack.clear();
        scene.on_mod = [&](int t) {
            if (t == 38) {                                   // начало маски: дальше рисуется сама маска
                if (mstack.size() >= 8) return;
                mstack.emplace_back();
                mstack.back().cov.assign((size_t)W * H * 4, 0.f);
            } else if (t == 39) {                            // дальше — содержимое, режется по маске
                if (mstack.empty() || !mstack.back().collecting) return;
                MaskFrame& fr = mstack.back();
                fr.collecting = false;
                fr.eff = fr.cov;
                if (mstack.size() >= 2 && !mstack[mstack.size() - 2].collecting) {
                    const std::vector<float>& par = mstack[mstack.size() - 2].eff;
                    for (size_t i = 3; i < fr.eff.size(); i += 4) fr.eff[i] *= par[i];
                }
            } else if (t == 40) {                            // конец маски
                if (!mstack.empty()) mstack.pop_back();
            }
        };
        scene.walk(root_mc, f, Aff(), CT(), 0, 0, [&](const Sc2Shape& sh, const Aff& m0, const CT& ct, int blend) {
            Aff m = m0;
            for (auto& cmd : sh.commands) {
                if (cmd.texture_index >= scene.tex.size() || cmd.vertices.size() < 3) continue;
                Scene::Tex& tx = scene.texture(cmd.texture_index);
                if (!tx.ok) continue;
                // вершины команды — triangle strip (так же рисует игра); сортировка по углу ломала невыпуклые фигуры
                std::vector<float>* target = &canvas;
                const float* clip = nullptr;
                bool additive = (blend & 8) != 0;
                if (!mstack.empty()) {
                    if (mstack.back().collecting) {         // рисуем форму маски (с учётом внешней маски)
                        target = &mstack.back().cov; additive = false;
                        if (mstack.size() >= 2 && !mstack[mstack.size() - 2].collecting) clip = mstack[mstack.size() - 2].eff.data();
                    } else clip = mstack.back().eff.data();
                }
                std::vector<Pt> poly;
                for (auto& v : cmd.vertices)
                    poly.push_back({(m.a * v.x + m.c * v.y + m.tx - minx) * sx, (m.b * v.x + m.d * v.y + m.ty - miny) * sy, v.u, v.v});
                for (size_t i = 0; i + 2 < poly.size(); i++) raster_tri(*target, W, H, tx, ct, additive, clip, poly[i], poly[i + 1], poly[i + 2]);
            }
        });
        rgba.resize((size_t)W * H * 4);
        for (size_t i = 0; i < (size_t)W * H; i++) {
            const float* c = &canvas[i * 4];
            float a = c[3];
            uint8_t* o = &rgba[i * 4];
            if (a <= 1e-4f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)std::min(255.f, std::max(0.f, c[k] / a * 255.f + 0.5f));
            o[3] = (uint8_t)std::min(255.f, a * 255.f + 0.5f);
        }
    };

    // ── палитра по выборке кадров ──
    std::vector<std::vector<uint8_t>> samples;
    bool has_transp = false;
    int nsamp = std::min(nframes, 12);
    for (int s = 0; s < nsamp; s++) {
        std::vector<uint8_t> px;
        render((int)((long)s * nframes / nsamp), px);
        for (size_t i = 3; i < px.size() && !has_transp; i += 4) if (px[i] < 128) has_transp = true;
        samples.push_back(std::move(px));
    }
    Quant q;
    build_quant(q, samples, has_transp);
    samples.clear(); samples.shrink_to_fit();

    std::string stem = unique_path(out_dir, base_no_ext(sc_path) + "_" + root_name, ".gif");
    std::string out_path = out_dir + "/" + stem + ".gif";
    GifOut gif(out_path);
    if (!gif.ok()) return "Error: write failed: " + out_path;
    gif.header(W, H, q);

    std::vector<uint8_t> rgba, idx, prev_idx;
    int written = 0;
    double acc_cs = 0; long emitted_cs = 0;
    int pending_cs = 0;
    auto flush_pending = [&]() {
        if (!prev_idx.empty()) { gif.frame(W, H, prev_idx, std::max(2, pending_cs), q.transparent); written++; }
    };
    for (int f = 0; f < nframes; f += step) {
        render(f, rgba);
        quantize_frame(q, rgba, W, H, idx);
        // задержка накапливается без дрейфа
        acc_cs += 100.0 * step / fps;
        long target = (long)std::lround(acc_cs);
        int cs = (int)(target - emitted_cs); emitted_cs = target;
        if (!prev_idx.empty() && prev_idx == idx) { pending_cs += cs; continue; }
        flush_pending();
        prev_idx = idx; pending_cs = cs;
    }
    flush_pending();
    gif.finish();

    char du[32]; snprintf(du, sizeof du, "%.2f", nframes / (double)fps);
    std::ostringstream r;
    r << "OK: SC2 -> GIF (" << root_name << ")\n";
    r << "  Frames: " << nframes << " @ " << fps << " fps (" << du << " s), GIF frames written: " << written << "\n";
    r << "  Size: " << W << "x" << H << (has_transp ? ", transparent" : "") << "\n";
    r << "  Output: " << stem << ".gif\n";
    r << "  Dir: " << out_dir;
    (void)out_fps;
    return r.str();
}
