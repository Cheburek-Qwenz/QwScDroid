/**
 * tex_ops.cpp — SCTX↔PNG, KTX↔PNG, PNG→SC
 * Исправлено: png2ktx принимает block size
 * Исправлено: extract KTX показывает какие индексы есть
 */
#include "sc_core.h"
#include "sctx_parser.h"
#include "astc_decoder.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include <android/log.h>
#include <algorithm>
#include <sstream>
#include <cstring>

#define TAG "TexOps"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const uint8_t KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

// GL формат → блок ASTC
struct ASTCFmt { uint32_t gl; int bw, bh; };
static const ASTCFmt ASTC_FMTS[] = {
    {0x93B0,4,4},{0x93B1,5,4},{0x93B2,5,5},{0x93B3,6,5},
    {0x93B4,6,6},{0x93B5,8,5},{0x93B6,8,6},{0x93B7,8,8},
    {0x93B8,10,5},{0x93B9,10,6},{0x93BA,10,8},{0x93BB,10,10},
    {0x93BC,12,10},{0x93BD,12,12},
    // sRGB
    {0x93D0,4,4},{0x93D1,5,4},{0x93D2,5,5},{0x93D3,6,5},
    {0x93D4,6,6},{0x93D5,8,5},{0x93D6,8,6},{0x93D7,8,8},
    {0x93D8,10,5},{0x93D9,10,6},{0x93DA,10,8},{0x93DB,10,10},
    {0x93DC,12,10},{0x93DD,12,12},
    {0,0,0}
};

// Найти GL формат по размеру блока
static uint32_t find_gl(int bw, int bh, bool srgb = false) {
    uint32_t base = srgb ? 0x93D0 : 0x93B0;
    for (auto* f = ASTC_FMTS; f->gl; f++)
        if (f->bw == bw && f->bh == bh && (srgb ? f->gl >= 0x93D0 : f->gl < 0x93D0))
            return f->gl;
    // fallback: 8x8
    return srgb ? 0x93D7 : 0x93B7;
}

// Парсить строку блока "8x8" → bw=8, bh=8
static bool parse_block(const std::string& s, int& bw, int& bh) {
    // поддерживаем "8x8", "8X8", "8х8" (кирилл)
    size_t pos = s.find_first_of("xXхХ");
    if (pos == std::string::npos) { bw=8; bh=8; return false; }
    try {
        bw = std::stoi(s.substr(0, pos));
        bh = std::stoi(s.substr(pos+1));
        return true;
    } catch (...) { bw=8; bh=8; return false; }
}

// ── SCTX → PNG ────────────────────────────────────────────────────────────────
std::string sctx2png(const std::string& sctx_path, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sctx_path, data)) return "Error: cannot open: " + sctx_path;

    SCTXFile sctx;
    std::string err;
    if (!parse_sctx(data.data(), data.size(), sctx, err))
        return "Error: SCTX parse failed: " + err;

    int w = sctx.width, h = sctx.height;
    uint32_t offset = sctx.levels.empty() ? 0 : sctx.levels[0].offset;
    if (offset >= sctx.texture_data.size()) return "Error: invalid mip offset";

    const uint8_t* mip = sctx.texture_data.data() + offset;
    size_t mip_avail   = sctx.texture_data.size() - offset;

    std::vector<uint8_t> rgba;
    if (pixel_is_astc(sctx.pixel_type)) {
        int bw, bh;
        pixel_astc_blocks(sctx.pixel_type, bw, bh);
        size_t sz = astc_level_size(w, h, bw, bh);
        if (sz > mip_avail) sz = mip_avail;
        if (!astc_decode(mip, sz, w, h, bw, bh, rgba, pixel_is_astc_srgb(sctx.pixel_type)))
            return "Error: ASTC decode failed";
    } else if (pixel_is_raw(sctx.pixel_type)) {
        size_t needed = (size_t)w * h * 4;
        if (needed > mip_avail) return "Error: not enough raw data";
        rgba.assign(mip, mip + needed);
        if (sctx.pixel_type == PT_BGRA8Unorm || sctx.pixel_type == 81)
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i+2]);
    } else {
        return "Error: unsupported pixel_type: " + pixel_type_name(sctx.pixel_type);
    }

    std::string stem     = sc_basename_no_ext(sctx_path);
    std::string png_path = out_dir + "/" + stem + ".png";
    if (!stbi_write_png(png_path.c_str(), w, h, 4, rgba.data(), w * 4))
        return "Error: PNG write failed: " + png_path;

    std::ostringstream r;
    r << "OK: SCTX to PNG\n";
    r << "  " << stem << ".png\n";
    r << "  " << w << "x" << h << "  " << pixel_type_name(sctx.pixel_type) << "\n";
    r << "  Dir: " << out_dir;
    return r.str();
}

// ── PNG → SCTX ────────────────────────────────────────────────────────────────
std::string png2sctx(const std::string& png_path, const std::string& out_dir) {
    int w, h, ch;
    uint8_t* img = stbi_load(png_path.c_str(), &w, &h, &ch, 4);
    if (!img) return "Error: cannot load PNG: " + png_path;

    int bw = 8, bh = 8;
    uint32_t ptype = PT_ASTC_8x8;
    int levels = 1;

    SCTXFile sctx;
    sctx.pixel_type   = ptype;
    sctx.width        = (uint16_t)w;
    sctx.height       = (uint16_t)h;
    sctx.levels_count = (uint8_t)levels;
    sctx.flags        = FLAG_COMPRESSION | FLAG_UNKNOWN2;

    std::vector<uint8_t> tex_data;
    MipLevel ml; ml.width=(uint16_t)w; ml.height=(uint16_t)h; ml.offset=0;
    std::vector<uint8_t> enc;
    std::vector<uint8_t> raw_img(img, img + (size_t)w*h*4);
    stbi_image_free(img);

    if (!astc_encode(raw_img.data(), w, h, bw, bh, enc))
        return "Error: ASTC encode failed";

    tex_data.insert(tex_data.end(), enc.begin(), enc.end());
    sctx.levels.push_back(ml);

    std::vector<uint8_t> out_data;
    std::string build_err;
    if (!build_sctx(sctx, tex_data, out_data, build_err))
        return "Error: build SCTX: " + build_err;

    std::string stem     = sc_basename_no_ext(png_path);
    std::string out_path = out_dir + "/" + stem + ".sctx";
    if (!sc_write_file(out_path, out_data)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: PNG to SCTX\n  " << stem << ".sctx\n  " << w << "x" << h << "  ASTC_8x8\n  Dir: " << out_dir;
    return r.str();
}

// ── KTX → PNG ────────────────────────────────────────────────────────────────
std::string ktx2png(const std::string& ktx_path, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(ktx_path, data)) return "Error: cannot open: " + ktx_path;
    if (data.size() < 64 || memcmp(data.data(), KTX_MAGIC, 12) != 0)
        return "Error: not a KTX file";

    size_t p = 12;
    auto rd32 = [&]() -> uint32_t {
        if (p+4>data.size()) return 0;
        uint32_t v; memcpy(&v,data.data()+p,4); p+=4; return v;
    };
    rd32(); // endianness
    uint32_t glType=rd32(); rd32(); uint32_t glFormat=rd32();
    uint32_t glInt=rd32(); rd32();
    int w=(int)rd32(), h=(int)rd32();
    rd32();rd32();rd32();rd32();
    uint32_t kvd=rd32(); p+=kvd;
    uint32_t imgSize=rd32();
    if (p+imgSize>data.size()) imgSize=(uint32_t)(data.size()-p);

    std::vector<uint8_t> rgba;
    bool is_astc = (glType==0 && glFormat==0);
    if (is_astc) {
        int bw=8,bh=8;
        for (auto* f=ASTC_FMTS; f->gl; f++)
            if (f->gl==glInt){bw=f->bw;bh=f->bh;break;}
        bool srgb=(glInt>=0x93D0);
        if (!astc_decode(data.data()+p, imgSize, w, h, bw, bh, rgba, srgb))
            return "Error: ASTC decode failed";
    } else {
        // Uncompressed RGBA8
        size_t needed=(size_t)w*h*4;
        if (p+needed>data.size()) return "Error: not enough data";
        rgba.assign(data.begin()+p, data.begin()+p+needed);
    }

    std::string stem     = sc_basename_no_ext(ktx_path);
    std::string png_path = out_dir + "/" + stem + ".png";
    if (!stbi_write_png(png_path.c_str(), w, h, 4, rgba.data(), w*4))
        return "Error: PNG write failed: " + png_path;

    std::ostringstream r;
    r << "OK: KTX to PNG\n  " << stem << ".png\n  " << w << "x" << h << "\n  Dir: " << out_dir;
    return r.str();
}

// ── PNG → KTX (с выбором блока) ───────────────────────────────────────────────
std::string png2ktx(const std::string& png_path, const std::string& block_str, const std::string& out_dir) {
    int bw=8, bh=8;
    parse_block(block_str, bw, bh);

    int w, h, ch;
    uint8_t* img = stbi_load(png_path.c_str(), &w, &h, &ch, 4);
    if (!img) return "Error: cannot load PNG: " + png_path;

    std::vector<uint8_t> raw(img, img+(size_t)w*h*4);
    stbi_image_free(img);

    std::vector<uint8_t> astc;
    if (!astc_encode(raw.data(), w, h, bw, bh, astc))
        return "Error: ASTC encode failed";

    uint32_t gl = find_gl(bw, bh, false);

    // Строим KTX
    std::vector<uint8_t> out_data;
    out_data.insert(out_data.end(), KTX_MAGIC, KTX_MAGIC+12);
    auto w32 = [&](uint32_t v){
        uint8_t b[4]; memcpy(b,&v,4);
        out_data.insert(out_data.end(),b,b+4);
    };
    w32(0x04030201); // endianness
    w32(0);          // glType
    w32(1);          // glTypeSize
    w32(0);          // glFormat
    w32(gl);         // glInternalFormat
    w32(0x1908);     // glBaseInternalFormat GL_RGBA
    w32((uint32_t)w);
    w32((uint32_t)h);
    w32(0); w32(0); w32(1); w32(1); w32(0); // depth,array,faces,mips,kvd
    w32((uint32_t)astc.size());
    out_data.insert(out_data.end(), astc.begin(), astc.end());

    std::string stem     = sc_basename_no_ext(png_path);
    std::string out_path = out_dir + "/" + stem + ".ktx";
    if (!sc_write_file(out_path, out_data)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: PNG to KTX\n";
    r << "  " << stem << ".ktx\n";
    r << "  " << w << "x" << h << "  ASTC_" << bw << "x" << bh << "\n";
    r << "  Dir: " << out_dir;
    return r.str();
}

// ── PNG → SC ──────────────────────────────────────────────────────────────────
// Объявление — реализация в png2sc_op.cpp
std::string png2sc(const std::string& png_path, const std::string& orig_sc,
                   int idx, const std::string& out_dir);
