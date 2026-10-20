#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Compression
enum class CompKind { NONE, ZSTD, ZLIB };

struct CompInfo {
    size_t   offset;
    CompKind kind;
};

CompInfo   sc_detect_compression(const std::vector<uint8_t>& data);
std::vector<uint8_t> sc_decompress(const std::vector<uint8_t>& data, const CompInfo& ci);
std::vector<uint8_t> sc_compress  (const std::vector<uint8_t>& raw,  CompKind kind);

// SC header
struct ScHeader {
    uint8_t  version;
    CompInfo comp;
    bool     valid;
};
ScHeader sc_parse_header(const std::vector<uint8_t>& data);

// Find split point between logic and texture blocks
// Returns split offset, -1 if not found
// mode: 0=compressed boundary, 1=decompressed KTX boundary
ssize_t sc_find_split(const std::vector<uint8_t>& data, int* mode_out);

// File helpers
bool sc_read_file (const std::string& path, std::vector<uint8_t>& out);
bool sc_write_file(const std::string& path, const std::vector<uint8_t>& data);
std::string sc_basename_no_ext(const std::string& path);
std::string sc_dirname(const std::string& path);
