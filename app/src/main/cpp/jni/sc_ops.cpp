/**
 * SC operations: split, join, info, downgrade
 * Called from sc_jni.cpp
 */
#include "sc_core.h"
#include <android/log.h>
#include <sstream>
#include <cstring>

#define TAG "SCOps"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)

static const uint8_t KTX_MAGIC[12] = {0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a};

// ── Split ─────────────────────────────────────────────────────────────────────
std::string sc_split(const std::string& sc_path, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data))
        return "❌ Cannot read: " + sc_path;

    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid)
        return "❌ Not an SC file";

    int mode = -1;
    ssize_t sp = sc_find_split(data, &mode);
    if (sp < 0)
        return "ℹ No texture block found in this SC file";

    std::string stem   = sc_basename_no_ext(sc_path);
    std::string lo_out = out_dir + "/" + stem + ".sc";
    std::string tex_out= out_dir + "/" + stem + "_tex.sc";

    if (mode == 0) {
        // Compressed boundary — just slice raw bytes
        std::vector<uint8_t> lo (data.begin(), data.begin()+sp);
        std::vector<uint8_t> tex(data.begin()+sp, data.end());
        if (!sc_write_file(lo_out,  lo))  return "❌ Write failed: " + lo_out;
        if (!sc_write_file(tex_out, tex)) return "❌ Write failed: " + tex_out;
    } else {
        // Decompressed KTX boundary — decompress, split raw, recompress each part
        try {
            auto raw  = sc_decompress(data, hdr.comp);
            auto raw_lo  = std::vector<uint8_t>(raw.begin(), raw.begin()+sp);
            auto raw_tex = std::vector<uint8_t>(raw.begin()+sp, raw.end());

            // Header bytes before compressed data
            std::vector<uint8_t> header(data.begin(), data.begin()+hdr.comp.offset);
            auto comp_lo  = sc_compress(raw_lo,  hdr.comp.kind);
            auto comp_tex = sc_compress(raw_tex, hdr.comp.kind);

            auto out_lo_data  = header; out_lo_data.insert(out_lo_data.end(), comp_lo.begin(),  comp_lo.end());
            auto out_tex_data = header; out_tex_data.insert(out_tex_data.end(),comp_tex.begin(), comp_tex.end());

            if (!sc_write_file(lo_out,  out_lo_data))  return "❌ Write failed: " + lo_out;
            if (!sc_write_file(tex_out, out_tex_data)) return "❌ Write failed: " + tex_out;
        } catch (std::exception& e) {
            return std::string("❌ ") + e.what();
        }
    }

    std::ostringstream r;
    r << "✅ Split complete\n";
    r << "  Logic:   " << stem << ".sc\n";
    r << "  Texture: " << stem << "_tex.sc\n";
    r << "  Output:  " << out_dir;
    return r.str();
}

// ── Join ──────────────────────────────────────────────────────────────────────
std::string sc_join(const std::string& logic_path, const std::string& tex_path,
                    const std::string& out_dir) {
    std::vector<uint8_t> dl, dt;
    if (!sc_read_file(logic_path, dl)) return "❌ Cannot read: " + logic_path;
    if (!sc_read_file(tex_path,   dt)) return "❌ Cannot read: " + tex_path;

    ScHeader hl = sc_parse_header(dl);
    ScHeader ht = sc_parse_header(dt);
    if (!hl.valid) return "❌ Not an SC file: " + logic_path;
    if (!ht.valid) return "❌ Not an SC file: " + tex_path;

    std::string stem    = sc_basename_no_ext(logic_path);
    std::string out_path= out_dir + "/" + stem + "_combined.sc";

    if (hl.comp.kind == ht.comp.kind && hl.comp.kind != CompKind::NONE) {
        try {
            auto raw_l = sc_decompress(dl, hl.comp);
            auto raw_t = sc_decompress(dt, ht.comp);
            raw_l.insert(raw_l.end(), raw_t.begin(), raw_t.end());

            std::vector<uint8_t> header(dl.begin(), dl.begin()+hl.comp.offset);
            auto comp = sc_compress(raw_l, hl.comp.kind);
            header.insert(header.end(), comp.begin(), comp.end());

            if (!sc_write_file(out_path, header)) return "❌ Write failed";
        } catch (std::exception& e) {
            return std::string("❌ ") + e.what();
        }
    } else {
        // Different or no compression — just concatenate
        dl.insert(dl.end(), dt.begin(), dt.end());
        if (!sc_write_file(out_path, dl)) return "❌ Write failed";
    }

    std::ostringstream r;
    r << "✅ Join complete\n  Output: " << stem << "_combined.sc\n  Dir: " << out_dir;
    return r.str();
}

// ── Info ──────────────────────────────────────────────────────────────────────
std::string sc_info(const std::string& sc_path) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "❌ Cannot read: " + sc_path;

    std::ostringstream r;

    // SCTX?
    if (data.size() >= 12 && memcmp(data.data()+8, "SCTX", 4) == 0) {
        r << "Type: SCTX\nSize: " << data.size() << " bytes";
        return r.str();
    }
    // KTX?
    if (data.size() >= 12 && memcmp(data.data(), KTX_MAGIC, 12) == 0) {
        uint32_t w, h, gl;
        memcpy(&gl, data.data()+28, 4);
        memcpy(&w,  data.data()+36, 4);
        memcpy(&h,  data.data()+40, 4);
        r << "Type: KTX\nSize: " << data.size() << " bytes\n";
        r << "Dims: " << w << "×" << h << "\nGL format: 0x" << std::hex << gl;
        return r.str();
    }

    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) { r << "❌ Unknown file format"; return r.str(); }

    const char* comp_str = "none";
    if (hdr.comp.kind == CompKind::ZSTD) comp_str = "zstd";
    if (hdr.comp.kind == CompKind::ZLIB) comp_str = "zlib";

    r << "Type: SC v" << (int)hdr.version << "\n";
    r << "Size: " << data.size() << " bytes\n";
    r << "Compression: " << comp_str << "\n";
    r << "Comp offset: " << hdr.comp.offset << "\n";

    int mode = -1;
    ssize_t sp = sc_find_split(data, &mode);
    if (sp >= 0) {
        r << "Texture block: YES\n";
        r << "Split mode: " << (mode == 0 ? "compressed boundary" : "KTX in decompressed") << "\n";
        r << "Split offset: " << sp;
    } else {
        r << "Texture block: NO (single-file SC)";
    }
    return r.str();
}

// ── Downgrade ─────────────────────────────────────────────────────────────────
std::string sc_downgrade(const std::string& sc_path, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "❌ Cannot read: " + sc_path;

    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "❌ Not an SC file";
    if (hdr.version <= 3) return "ℹ Already at minimum version";

    // Decompress
    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("❌ Decompress: ") + e.what(); }

    // Recompress with zlib (older clients use zlib)
    std::vector<uint8_t> header(data.begin(), data.begin()+hdr.comp.offset);
    header[2] = 3;  // downgrade version byte to 3

    std::vector<uint8_t> comp;
    try { comp = sc_compress(raw, CompKind::ZLIB); }
    catch (std::exception& e) { return std::string("❌ Compress: ") + e.what(); }

    header.insert(header.end(), comp.begin(), comp.end());

    std::string stem    = sc_basename_no_ext(sc_path);
    std::string out_path= out_dir + "/" + stem + "_v3.sc";
    if (!sc_write_file(out_path, header)) return "❌ Write failed";

    std::ostringstream r;
    r << "✅ Downgraded to v3\n  Output: " << stem << "_v3.sc";
    return r.str();
}
