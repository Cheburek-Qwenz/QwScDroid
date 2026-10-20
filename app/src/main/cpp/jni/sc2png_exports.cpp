#include "sc_core.h"
#include "stb/stb_image_write.h"
#include "sctx_parser.h"
#include "astc_decoder.h"
#include <sstream>
#include <cstring>
#include <android/log.h>

#define TAG "SC2PNG"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const uint8_t KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

// ── Вспомогалка: декодировать KTX блок в RGBA ────────────────────────────────
static bool decode_ktx_block(const uint8_t* data, size_t size,
                              int& w, int& h, std::vector<uint8_t>& rgba) {
    if (size < 64) return false;
    if (memcmp(data, KTX_MAGIC, 12) != 0) return false;

    size_t p = 16; // skip magic + endianness
    auto rd32 = [&]() -> uint32_t {
        if (p + 4 > size) return 0;
        uint32_t v; memcpy(&v, data+p, 4); p+=4; return v;
    };
    uint32_t glType           = rd32();
    rd32(); // glTypeSize
    uint32_t glFormat         = rd32();
    uint32_t glInternalFormat = rd32();
    rd32(); // glBaseInternalFormat
    w = (int)rd32();
    h = (int)rd32();
    rd32(); // depth
    rd32(); // numArrayElements
    rd32(); // numFaces
    rd32(); // numMips
    uint32_t kvd = rd32();
    p += kvd;
    uint32_t imgSize = rd32();

    if (p + imgSize > size) imgSize = (uint32_t)(size - p);

    bool is_astc = (glType == 0 && glFormat == 0);
    if (is_astc) {
        struct ASTCFmt { uint32_t gl; int bw, bh; };
        static const ASTCFmt fmts[] = {
            {0x93B0,4,4},{0x93B1,5,4},{0x93B2,5,5},{0x93B3,6,5},
            {0x93B4,6,6},{0x93B5,8,5},{0x93B6,8,6},{0x93B7,8,8},
            {0x93B8,10,5},{0x93B9,10,6},{0x93BA,10,8},{0x93BB,10,10},
            {0x93BC,12,10},{0x93BD,12,12},
            {0x93D0,4,4},{0x93D1,5,4},{0x93D2,5,5},{0x93D3,6,5},
            {0x93D4,6,6},{0x93D5,8,5},{0x93D6,8,6},{0x93D7,8,8},
            {0x93D8,10,5},{0x93D9,10,6},{0x93DA,10,8},{0x93DB,10,10},
            {0x93DC,12,10},{0x93DD,12,12},{0,0,0}
        };
        int bw = 0, bh = 0;
        for (auto* f = fmts; f->gl; f++)
            if (f->gl == glInternalFormat) { bw=f->bw; bh=f->bh; break; }
        if (!bw) return false;
        bool srgb = (glInternalFormat >= 0x93D0);
        return astc_decode(data+p, imgSize, w, h, bw, bh, rgba, srgb);
    }

    // Uncompressed RGBA8
    size_t needed = (size_t)w * h * 4;
    if (p + needed > size) return false;
    rgba.assign(data+p, data+p+needed);
    return true;
}

// ── SC → PNG (все текстуры) ───────────────────────────────────────────────────
std::string sc2png(const std::string& sc_path, int idx, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "Error: cannot read " + sc_path;

    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "Error: not an SC file";

    // Декомпрессия
    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: decompress: ") + e.what(); }

    // Находим все KTX блоки
    std::vector<size_t> ktx_offsets;
    for (size_t i = 0; i + 12 <= raw.size(); i++) {
        if (memcmp(raw.data()+i, KTX_MAGIC, 12) == 0)
            ktx_offsets.push_back(i);
    }

    if (ktx_offsets.empty()) return "Error: no KTX textures found in SC";

    std::string stem = sc_basename_no_ext(sc_path);
    std::ostringstream result;
    int saved = 0;

    // Если idx >= 0 — только одну
    int from_idx = (idx >= 0 && idx < (int)ktx_offsets.size()) ? idx : 0;
    int to_idx   = (idx >= 0) ? from_idx + 1 : (int)ktx_offsets.size();

    for (int i = from_idx; i < to_idx; i++) {
        size_t start = ktx_offsets[i];
        size_t end   = (i+1 < (int)ktx_offsets.size()) ? ktx_offsets[i+1] : raw.size();
        const uint8_t* block = raw.data() + start;
        size_t blk_size = end - start;

        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (!decode_ktx_block(block, blk_size, w, h, rgba)) {
            result << "Warning: KTX[" << i << "] decode failed, skipping\n";
            continue;
        }

        std::string suffix = (ktx_offsets.size() > 1) ? "_" + std::to_string(i) : "";
        std::string out_path = out_dir + "/" + stem + suffix + ".png";
        if (!stbi_write_png(out_path.c_str(), w, h, 4, rgba.data(), w*4)) {
            result << "Error: write failed: " << out_path << "\n";
            continue;
        }
        result << stem << suffix << ".png  " << w << "x" << h << "\n";
        saved++;
    }

    if (saved == 0) return "Error: no textures extracted";

    std::ostringstream r;
    r << "OK: SC to PNG\n";
    r << "  Extracted: " << saved << " of " << ktx_offsets.size() << " textures\n";
    r << result.str();
    r << "  Dir: " << out_dir;
    return r.str();
}

// ── Список экспортов ──────────────────────────────────────────────────────────
std::string sc_exports(const std::string& sc_path, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "Error: cannot read " + sc_path;

    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "Error: not an SC file";

    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: ") + e.what(); }

    // Парсим хедер SC — counts + export names
    if (raw.size() < 15) return "Error: SC too small";

    size_t pos = 0;
    auto r16 = [&]() -> uint16_t {
        if (pos+2 > raw.size()) return 0;
        uint16_t v; memcpy(&v, raw.data()+pos, 2); pos+=2; return v;
    };
    auto r8 = [&]() -> uint8_t {
        if (pos >= raw.size()) return 0;
        return raw[pos++];
    };
    auto r32 = [&]() -> uint32_t {
        if (pos+4 > raw.size()) return 0;
        uint32_t v; memcpy(&v, raw.data()+pos, 4); pos+=4; return v;
    };
    auto read_str = [&]() -> std::string {
        uint8_t len = r8();
        uint16_t slen = len;
        if (len == 0xFF) slen = r16();
        if (pos+slen > raw.size()) return "";
        std::string s((char*)raw.data()+pos, slen);
        pos += slen; return s;
    };

    uint16_t shape_count = r16();
    uint16_t mc_count    = r16();
    uint16_t tex_count   = r16();
    uint16_t tf_count    = r16();
    uint16_t mat_count   = r16();
    uint16_t ct_count    = r16();
    r32(); // unknown
    r8();  // unknown
    uint16_t export_count = r16();

    if (export_count > 10000) return "Error: invalid export count";

    std::vector<uint16_t> ids(export_count);
    for (auto& id : ids) id = r16();

    std::vector<std::string> names(export_count);
    for (auto& n : names) n = read_str();

    std::ostringstream r;
    r << "OK: SC Exports\n";
    r << "  File: " << sc_basename_no_ext(sc_path) << ".sc\n";
    r << "  Shapes: " << shape_count << "  MC: " << mc_count
      << "  Tex: " << tex_count << "\n";
    r << "  Exports: " << export_count << "\n\n";
    for (int i = 0; i < (int)export_count; i++) {
        r << "  [" << ids[i] << "] " << names[i] << "\n";
    }
    return r.str();
}
