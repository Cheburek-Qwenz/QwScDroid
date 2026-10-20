#include "sctx_parser.h"
#include <cstring>
#include <algorithm>
#include "zstd/lib/zstd.h"

static inline uint16_t r16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void w16(uint8_t* p, uint16_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF;
}
static inline void w32(uint8_t* p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

// ─── FlatBuffers reader ───────────────────────────────────────────────────────
struct FbReader {
    const uint8_t* buf;
    size_t bsz;
    size_t obj;

    bool init(const uint8_t* data, size_t size) {
        buf = data; bsz = size;
        if(size < 4) return false;
        obj = r32(data);
        if(obj + 4 > size) return false;
        return true;
    }
    size_t vtable() const {
        int32_t soff = (int32_t)r32(buf + obj);
        return (size_t)((ptrdiff_t)obj - (ptrdiff_t)soff);
    }
    int num_fields() const {
        size_t vt = vtable();
        if(vt + 2 > bsz) return 0;
        return (int)((r16(buf + vt) - 4) / 2);
    }
    size_t field_abs(int idx) const {
        int nf = num_fields();
        if(idx >= nf) return 0;
        size_t vt = vtable();
        if(vt + 4 + idx*2 + 2 > bsz) return 0;
        uint16_t fo = r16(buf + vt + 4 + idx * 2);
        if(fo == 0) return 0;
        return obj + fo;
    }
    uint8_t  u8 (int i) const { size_t o=field_abs(i); return o?buf[o]:0; }
    uint16_t u16(int i) const { size_t o=field_abs(i); return o?r16(buf+o):0; }
    uint32_t u32(int i) const { size_t o=field_abs(i); return o?r32(buf+o):0; }
    const uint8_t* vec(int i, uint32_t& count) const {
        count = 0;
        size_t o = field_abs(i);
        if(!o) return nullptr;
        uint32_t rel = r32(buf + o);
        size_t vpos = o + rel;
        if(vpos + 4 > bsz) return nullptr;
        count = r32(buf + vpos);
        if(vpos + 4 + count > bsz) return nullptr;
        return buf + vpos + 4;
    }
};

// ─── Pixel helpers ────────────────────────────────────────────────────────────
bool pixel_is_astc_srgb(uint32_t type) {
    return (type >= PT_ASTC_SRGB_4x4 && type <= PT_ASTC_SRGB_12x12);
}

bool pixel_is_astc(uint32_t type) {
    return (type >= PT_ASTC_4x4 && type <= PT_ASTC_12x12) || pixel_is_astc_srgb(type);
}

bool pixel_is_raw(uint32_t type) {
    return type == PT_RGBA8Unorm || type == PT_RGBA8Unorm_sRGB ||
           type == PT_BGRA8Unorm || type == PT_BGRA8Unorm_sRGB;
}

// Нормализует sRGB тип в linear эквивалент для получения размера блока
static uint32_t normalize_astc_type(uint32_t type) {
    if(pixel_is_astc_srgb(type)) {
        // sRGB 186-200 → linear 204-218 (с учётом пропуска 209/191)
        // 186-190 → 204-208, 192-200 → 210-218
        if(type <= 190) return PT_ASTC_4x4 + (type - 186);    // 186→204, 187→205...
        else            return PT_ASTC_8x5  + (type - 192);    // 192→210, 193→211...
    }
    return type;
}

void pixel_astc_blocks(uint32_t type, int& bw, int& bh) {
    type = normalize_astc_type(type);
    switch(type) {
    case PT_ASTC_4x4:  bw=4;  bh=4;  return;
    case PT_ASTC_5x4:  bw=5;  bh=4;  return;
    case PT_ASTC_5x5:  bw=5;  bh=5;  return;
    case PT_ASTC_6x5:  bw=6;  bh=5;  return;
    case PT_ASTC_6x6:  bw=6;  bh=6;  return;
    case PT_ASTC_8x5:  bw=8;  bh=5;  return;
    case PT_ASTC_8x6:  bw=8;  bh=6;  return;
    case PT_ASTC_8x8:  bw=8;  bh=8;  return;
    case PT_ASTC_10x5: bw=10; bh=5;  return;
    case PT_ASTC_10x6: bw=10; bh=6;  return;
    case PT_ASTC_10x8: bw=10; bh=8;  return;
    case PT_ASTC_10x10:bw=10; bh=10; return;
    case PT_ASTC_12x10:bw=12; bh=10; return;
    case PT_ASTC_12x12:bw=12; bh=12; return;
    default:           bw=8;  bh=8;  return;
    }
}

size_t astc_level_size(int w, int h, int bw, int bh) {
    return (size_t)((w + bw-1)/bw) * ((h + bh-1)/bh) * 16;
}

size_t raw_level_size(int w, int h, uint32_t /*type*/) {
    return (size_t)w * h * 4;
}

std::string pixel_type_name(uint32_t t) {
    switch(t) {
    case PT_RGBA8Unorm:       return "RGBA8Unorm";
    case PT_RGBA8Unorm_sRGB:  return "RGBA8Unorm_sRGB";
    case PT_BGRA8Unorm:       return "BGRA8Unorm";
    case PT_BGRA8Unorm_sRGB:  return "BGRA8Unorm_sRGB";
    case PT_ASTC_SRGB_4x4:   return "ASTC_SRGBA8_4x4";
    case PT_ASTC_SRGB_5x4:   return "ASTC_SRGBA8_5x4";
    case PT_ASTC_SRGB_5x5:   return "ASTC_SRGBA8_5x5";
    case PT_ASTC_SRGB_6x5:   return "ASTC_SRGBA8_6x5";
    case PT_ASTC_SRGB_6x6:   return "ASTC_SRGBA8_6x6";
    case PT_ASTC_SRGB_8x5:   return "ASTC_SRGBA8_8x5";
    case PT_ASTC_SRGB_8x6:   return "ASTC_SRGBA8_8x6";
    case PT_ASTC_SRGB_8x8:   return "ASTC_SRGBA8_8x8";
    case PT_ASTC_SRGB_10x5:  return "ASTC_SRGBA8_10x5";
    case PT_ASTC_SRGB_10x6:  return "ASTC_SRGBA8_10x6";
    case PT_ASTC_SRGB_10x8:  return "ASTC_SRGBA8_10x8";
    case PT_ASTC_SRGB_10x10: return "ASTC_SRGBA8_10x10";
    case PT_ASTC_SRGB_12x10: return "ASTC_SRGBA8_12x10";
    case PT_ASTC_SRGB_12x12: return "ASTC_SRGBA8_12x12";
    case PT_ASTC_4x4:  return "ASTC_RGBA8_4x4";
    case PT_ASTC_5x4:  return "ASTC_RGBA8_5x4";
    case PT_ASTC_5x5:  return "ASTC_RGBA8_5x5";
    case PT_ASTC_6x5:  return "ASTC_RGBA8_6x5";
    case PT_ASTC_6x6:  return "ASTC_RGBA8_6x6";
    case PT_ASTC_8x5:  return "ASTC_RGBA8_8x5";
    case PT_ASTC_8x6:  return "ASTC_RGBA8_8x6";
    case PT_ASTC_8x8:  return "ASTC_RGBA8_8x8";
    case PT_ASTC_10x5: return "ASTC_RGBA8_10x5";
    case PT_ASTC_10x6: return "ASTC_RGBA8_10x6";
    case PT_ASTC_10x8: return "ASTC_RGBA8_10x8";
    case PT_ASTC_10x10:return "ASTC_RGBA8_10x10";
    case PT_ASTC_12x10:return "ASTC_RGBA8_12x10";
    case PT_ASTC_12x12:return "ASTC_RGBA8_12x12";
    default: char buf[32]; snprintf(buf,32,"Unknown(%u)",t); return buf;
    }
}

static uint32_t total_tex_size(const SCTXFile& f) {
    uint32_t total = 0;
    bool is_astc = pixel_is_astc(f.pixel_type);
    int bw=8, bh=8;
    if(is_astc) pixel_astc_blocks(f.pixel_type, bw, bh);
    for(int i = 0; i < f.levels_count; i++) {
        int w = std::max(1, (int)f.width  >> i);
        int h = std::max(1, (int)f.height >> i);
        total += (uint32_t)(is_astc ? astc_level_size(w,h,bw,bh)
                                    : raw_level_size(w,h,f.pixel_type));
    }
    return total;
}

// ─── parse_sctx ──────────────────────────────────────────────────────────────
bool parse_sctx(const uint8_t* data, size_t size, SCTXFile& out, std::string& error) {
    size_t pos = 0;
    out = SCTXFile{};

    if(pos + 4 > size) { error = "Слишком маленький файл"; return false; }
    uint32_t td_sz = r32(data + pos); pos += 4;
    if(pos + td_sz > size) { error = "Файл обрезан (TextureData)"; return false; }

    if(td_sz >= 8 && memcmp(data + pos + 4, "SCTX", 4) != 0) {
        error = "Неверный magic (ожидается SCTX)";
        return false;
    }

    {
        FbReader td;
        if(!td.init(data + pos, td_sz)) { error = "Ошибка FlatBuffer TextureData"; return false; }
        out.pixel_type   = td.u32(1);
        out.width        = td.u16(2);
        out.height       = td.u16(3);
        out.levels_count = td.u8(4);
        out.flags        = td.u32(6);
        out.tex_len      = td.u32(7);
    }
    pos += td_sz;

    if(out.width == 0 || out.height == 0) {
        error = "Нулевые размеры текстуры";
        return false;
    }

    if(pos + 4 > size) { error = "Файл обрезан (mip_maps_length)"; return false; }
    uint32_t mip_chunk_sz = r32(data + pos); pos += 4;
    size_t mip_end = pos + mip_chunk_sz;
    if(mip_end > size) { error = "Файл обрезан (mip_maps)"; return false; }

    out.levels.reserve(out.levels_count);
    while(pos + 4 <= mip_end) {
        uint32_t mm_sz = r32(data + pos); pos += 4;
        if(pos + mm_sz > mip_end) break;
        FbReader mm;
        if(mm.init(data + pos, mm_sz)) {
            MipLevel lv;
            lv.width  = mm.u16(0);
            lv.height = mm.u16(1);
            lv.offset = mm.u32(2);
            uint32_t hlen = 0;
            const uint8_t* hdata = mm.vec(3, hlen);
            if(hdata && hlen) lv.hash.assign(hdata, hdata + hlen);
            out.levels.push_back(lv);
        }
        pos += mm_sz;
    }
    pos = mip_end;

    if(out.use_padding()) {
        pos = (pos + 15) & ~(size_t)15;
    }

    if(pos > size) { error = "Нет места для данных текстуры"; return false; }
    size_t tex_available = size - pos;
    size_t expected = (out.tex_len > 0) ? out.tex_len : (size_t)total_tex_size(out);

    static const uint8_t ZSTD_MAGIC[] = {0x28,0xB5,0x2F,0xFD};
    bool is_zstd = (tex_available >= 4 && memcmp(data + pos, ZSTD_MAGIC, 4) == 0);

    if(is_zstd) {
        size_t content = ZSTD_getFrameContentSize(data + pos, tex_available);
        if(content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR)
            content = expected;
        if(content == 0 || content > 256*1024*1024) {
            error = "Некорректный размер ZSTD контента"; return false;
        }
        out.texture_data.resize(content);
        size_t r = ZSTD_decompress(out.texture_data.data(), content,
                                   data + pos, tex_available);
        if(ZSTD_isError(r)) {
            error = std::string("ZSTD: ") + ZSTD_getErrorName(r); return false;
        }
        out.texture_data.resize(r);
    } else {
        size_t take = (expected > 0 && expected <= tex_available) ? expected : tex_available;
        out.texture_data.assign(data + pos, data + pos + take);
    }

    return true;
}

// ─── FlatBuffers builder ──────────────────────────────────────────────────────
static void a32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x&0xFF); v.push_back((x>>8)&0xFF);
    v.push_back((x>>16)&0xFF); v.push_back((x>>24)&0xFF);
}
static void a16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x&0xFF); v.push_back((x>>8)&0xFF);
}

static std::vector<uint8_t> build_texture_data_fb(const SCTXFile& f) {
    uint32_t tex_len = total_tex_size(f);

    // Layout: root_off(4) + file_id(4) + vtable(26) + object(40)
    const uint32_t OBJ_OFF = 34;
    std::vector<uint8_t> fb;
    a32(fb, OBJ_OFF);
    fb.push_back('S'); fb.push_back('C'); fb.push_back('T'); fb.push_back('X');
    // vtable: size=26, obj_size=40, 11 field offsets
    a16(fb, 26); a16(fb, 40);
    uint16_t fo[11] = {0, 4, 32, 34, 36, 8, 12, 16, 20, 24, 0};
    for(int i=0;i<11;i++) a16(fb, fo[i]);
    // object
    a32(fb, (uint32_t)(OBJ_OFF - 8)); // vsoffset=26
    a32(fb, f.pixel_type);
    a32(fb, 0);           // unk3
    a32(fb, f.flags);
    a32(fb, tex_len);
    a32(fb, 0);           // unk5
    a32(fb, 0);           // unk6
    a32(fb, 0);           // pad (field[0]=unk1 offset=0)
    a16(fb, f.width);
    a16(fb, f.height);
    fb.push_back(f.levels_count);
    fb.push_back(0); a16(fb, 0); // pad to 40 bytes

    std::vector<uint8_t> result;
    a32(result, (uint32_t)fb.size());
    result.insert(result.end(), fb.begin(), fb.end());
    return result;
}

static std::vector<uint8_t> build_mipmap_fb(uint16_t width, uint16_t height,
                                              uint32_t offset,
                                              const std::vector<uint8_t>& hash) {
    bool has_hash = !hash.empty();
    const uint16_t VT_SZ = 12;
    const uint16_t OB_SZ = has_hash ? 16 : 12;
    const uint32_t OBJ_OFF = 4 + VT_SZ;

    std::vector<uint8_t> fb;
    a32(fb, OBJ_OFF);
    a16(fb, VT_SZ); a16(fb, OB_SZ);
    a16(fb, 8); a16(fb, 10); a16(fb, 4);
    a16(fb, has_hash ? (uint16_t)12 : (uint16_t)0);
    // object
    a32(fb, (uint32_t)(OBJ_OFF - 4));
    a32(fb, offset);
    a16(fb, width); a16(fb, height);
    if(has_hash) {
        a32(fb, 4); // offset to vector
        a32(fb, (uint32_t)hash.size());
        fb.insert(fb.end(), hash.begin(), hash.end());
        while(fb.size() % 4) fb.push_back(0);
    }

    std::vector<uint8_t> result;
    a32(result, (uint32_t)fb.size());
    result.insert(result.end(), fb.begin(), fb.end());
    return result;
}

bool build_sctx(const SCTXFile& file,
                const std::vector<uint8_t>& texture_data,
                std::vector<uint8_t>& out_file,
                std::string& error) {
    out_file.clear();

    auto td = build_texture_data_fb(file);
    out_file.insert(out_file.end(), td.begin(), td.end());

    std::vector<uint8_t> mip_chunk;
    for(const auto& lv : file.levels) {
        auto mm = build_mipmap_fb(lv.width, lv.height, lv.offset, lv.hash);
        mip_chunk.insert(mip_chunk.end(), mm.begin(), mm.end());
    }
    a32(out_file, (uint32_t)mip_chunk.size());
    out_file.insert(out_file.end(), mip_chunk.begin(), mip_chunk.end());

    if(file.use_padding()) {
        size_t aligned = (out_file.size() + 15) & ~(size_t)15;
        out_file.resize(aligned, 0);
    }

    if(file.use_compression()) {
        size_t bound = ZSTD_compressBound(texture_data.size());
        size_t prev  = out_file.size();
        out_file.resize(prev + bound);
        size_t r = ZSTD_compress(out_file.data() + prev, bound,
                                 texture_data.data(), texture_data.size(), 16);
        if(ZSTD_isError(r)) {
            error = std::string("ZSTD compress: ") + ZSTD_getErrorName(r);
            return false;
        }
        out_file.resize(prev + r);
    } else {
        out_file.insert(out_file.end(), texture_data.begin(), texture_data.end());
    }
    return true;
}
