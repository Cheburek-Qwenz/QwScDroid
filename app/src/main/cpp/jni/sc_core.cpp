#include "sc_core.h"
#include "zstd/lib/zstd.h"
#include <fstream>
#include <cstring>
#include <android/log.h>
#include <zlib.h>

#define TAG "SCCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const uint8_t ZSTD_MAGIC[4] = {0x28,0xb5,0x2f,0xfd};
static const uint8_t SC_MAGIC[2]   = {'S','C'};
static const uint8_t SC_V5[6]      = {'S','C',0x05,0x00,0x00,0x00};
static const uint8_t ZLIB_HEADS[][2] = {{0x78,0x01},{0x78,0x5e},{0x78,0x9c},{0x78,0xda}};
static const uint8_t KTX_MAGIC[12] = {0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a};

// ── File I/O ──────────────────────────────────────────────────────────────────
bool sc_read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
bool sc_write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), data.size());
    return f.good();
}
std::string sc_basename_no_ext(const std::string& path) {
    size_t slash = path.rfind('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash+1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // strip _tex suffix
    if (name.size() > 4 && name.substr(name.size()-4) == "_tex")
        name = name.substr(0, name.size()-4);
    return name;
}
std::string sc_dirname(const std::string& path) {
    size_t slash = path.rfind('/');
    return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

// ── Compression detection ─────────────────────────────────────────────────────
CompInfo sc_detect_compression(const std::vector<uint8_t>& d) {
    // SC v5: SC\x05\x00\x00\x00 + u32 id_size, then zstd
    if (d.size() >= 10 && memcmp(d.data(), SC_V5, 6) == 0) {
        uint32_t id_size;
        memcpy(&id_size, d.data()+6, 4);
        size_t off = 10 + id_size;
        if (off+4 <= d.size() && memcmp(d.data()+off, ZSTD_MAGIC, 4) == 0)
            return {off, CompKind::ZSTD};
    }
    // Scan for zstd magic
    for (size_t i = 0; i+4 <= d.size(); i++) {
        if (memcmp(d.data()+i, ZSTD_MAGIC, 4) == 0)
            return {i, CompKind::ZSTD};
    }
    // Scan for zlib
    for (size_t i = 4; i+2 <= d.size() && i < 512; i++) {
        for (auto& h : ZLIB_HEADS) {
            if (d[i] == h[0] && d[i+1] == h[1])
                return {i, CompKind::ZLIB};
        }
    }
    return {d.size(), CompKind::NONE};
}

// ── Decompress ────────────────────────────────────────────────────────────────
std::vector<uint8_t> sc_decompress(const std::vector<uint8_t>& data, const CompInfo& ci) {
    const uint8_t* src  = data.data() + ci.offset;
    size_t         slen = data.size() - ci.offset;

    if (ci.kind == CompKind::ZSTD) {

        /*
         * Некоторые файлы содержат лишний trailing-байт
         * (padding/выравнивание) сразу после конца реального
         * zstd-frame — если передать в ZSTD_decompress весь
         * остаток файла как есть, она честно падает с
         * "Src size is incorrect", даже если сами сжатые
         * данные полностью корректны.
         *
         * ZSTD_findFrameCompressedSize точно вычисляет
         * границу одного frame'а по его внутренней
         * структуре (блоки/чексумма), независимо от того,
         * что идёт после него в буфере — это надёжнее,
         * чем полагаться на заявленный в SC-заголовке
         * compressed_size, который у части файлов оказывается
         * неточным (off-by-one или больше).
         */
        size_t exact_size =
            ZSTD_findFrameCompressedSize(src, slen);

        if (!ZSTD_isError(exact_size) &&
            exact_size <= slen) {

            slen = exact_size;
        }

        size_t dsz = ZSTD_getFrameContentSize(src, slen);
        if (dsz == ZSTD_CONTENTSIZE_UNKNOWN) dsz = slen * 8;
        if (dsz == ZSTD_CONTENTSIZE_ERROR)   dsz = slen * 8;
        for (int attempt = 0; attempt < 6; attempt++) {
            std::vector<uint8_t> out(dsz);
            size_t r = ZSTD_decompress(out.data(), dsz, src, slen);
            if (!ZSTD_isError(r)) { out.resize(r); return out; }
            dsz *= 4;
        }
        throw std::runtime_error("ZSTD decompress failed");
    }
    if (ci.kind == CompKind::ZLIB) {
        std::vector<uint8_t> out(slen * 8);
        uLongf dlen = out.size();
        while (uncompress(out.data(), &dlen, src, slen) == Z_BUF_ERROR) {
            out.resize(out.size()*2); dlen = out.size();
        }
        out.resize(dlen); return out;
    }
    // NONE — return as-is from offset
    return std::vector<uint8_t>(data.begin()+ci.offset, data.end());
}

// ── Compress ──────────────────────────────────────────────────────────────────
std::vector<uint8_t> sc_compress(const std::vector<uint8_t>& raw, CompKind kind) {
    if (kind == CompKind::ZSTD) {
        size_t bound = ZSTD_compressBound(raw.size());
        std::vector<uint8_t> out(bound);
        size_t r = ZSTD_compress(out.data(), bound, raw.data(), raw.size(), 3);
        if (ZSTD_isError(r)) throw std::runtime_error("ZSTD compress failed");
        out.resize(r); return out;
    }
    if (kind == CompKind::ZLIB) {
        uLongf bound = compressBound(raw.size());
        std::vector<uint8_t> out(bound);
        if (compress2(out.data(), &bound, raw.data(), raw.size(), Z_DEFAULT_COMPRESSION) != Z_OK)
            throw std::runtime_error("zlib compress failed");
        out.resize(bound); return out;
    }
    return raw;
}

// ── SC header ─────────────────────────────────────────────────────────────────
ScHeader sc_parse_header(const std::vector<uint8_t>& data) {
    ScHeader h{};
    if (data.size() < 3 || memcmp(data.data(), SC_MAGIC, 2) != 0) return h;
    h.valid   = true;
    h.version = data[2];
    h.comp    = sc_detect_compression(data);
    return h;
}

// ── Find split point ──────────────────────────────────────────────────────────
ssize_t sc_find_split(const std::vector<uint8_t>& data, int* mode_out) {
    if (mode_out) *mode_out = -1;
    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid || hdr.comp.kind == CompKind::NONE) return -1;

    size_t co = hdr.comp.offset;

    // Try: find end of first zstd frame, check if next bytes are SC magic
    if (hdr.comp.kind == CompKind::ZSTD) {
        size_t fsz = ZSTD_findFrameCompressedSize(data.data()+co, data.size()-co);
        if (!ZSTD_isError(fsz)) {
            size_t sp = co + fsz;
            while (sp < data.size() && data[sp] == 0) sp++;
            if (sp+2 <= data.size() && memcmp(data.data()+sp, SC_MAGIC, 2) == 0) {
                if (mode_out) *mode_out = 0;  // compressed boundary
                return (ssize_t)sp;
            }
        }
    }

    // Try: decompress and find KTX magic
    try {
        auto raw = sc_decompress(data, hdr.comp);
        for (size_t i = 0; i+12 <= raw.size(); i++) {
            if (memcmp(raw.data()+i, KTX_MAGIC, 12) == 0) {
                if (mode_out) *mode_out = 1;  // decompressed KTX boundary
                return (ssize_t)i;
            }
        }
    } catch (...) {}

    return -1;
}
