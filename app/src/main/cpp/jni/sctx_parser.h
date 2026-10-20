#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ─── Pixel types (точно из ScPixel.hpp оригинала) ─────────────────────────────

// RAW
static const uint32_t PT_RGBA8Unorm      = 70;
static const uint32_t PT_RGBA8Unorm_sRGB = 71;
static const uint32_t PT_BGRA8Unorm      = 80;
static const uint32_t PT_BGRA8Unorm_sRGB = 81;

// ASTC sRGB (186-200)
static const uint32_t PT_ASTC_SRGB_4x4  = 186;
static const uint32_t PT_ASTC_SRGB_5x4  = 187;
static const uint32_t PT_ASTC_SRGB_5x5  = 188;
static const uint32_t PT_ASTC_SRGB_6x5  = 189;
static const uint32_t PT_ASTC_SRGB_6x6  = 190;
static const uint32_t PT_ASTC_SRGB_8x5  = 192;
static const uint32_t PT_ASTC_SRGB_8x6  = 193;
static const uint32_t PT_ASTC_SRGB_8x8  = 194;
static const uint32_t PT_ASTC_SRGB_10x5 = 195;
static const uint32_t PT_ASTC_SRGB_10x6 = 196;
static const uint32_t PT_ASTC_SRGB_10x8 = 197;
static const uint32_t PT_ASTC_SRGB_10x10= 198;
static const uint32_t PT_ASTC_SRGB_12x10= 199;
static const uint32_t PT_ASTC_SRGB_12x12= 200;

// ASTC linear (204-218)
static const uint32_t PT_ASTC_4x4  = 204;
static const uint32_t PT_ASTC_5x4  = 205;
static const uint32_t PT_ASTC_5x5  = 206;
static const uint32_t PT_ASTC_6x5  = 207;
static const uint32_t PT_ASTC_6x6  = 208;
// 209 не используется
static const uint32_t PT_ASTC_8x5  = 210;
static const uint32_t PT_ASTC_8x6  = 211;
static const uint32_t PT_ASTC_8x8  = 212;
static const uint32_t PT_ASTC_10x5 = 213;
static const uint32_t PT_ASTC_10x6 = 214;  // был пропущен!
static const uint32_t PT_ASTC_10x8 = 215;
static const uint32_t PT_ASTC_10x10= 216;
static const uint32_t PT_ASTC_12x10= 217;
static const uint32_t PT_ASTC_12x12= 218;

// ─── TextureFlags bits ────────────────────────────────────────────────────────
static const uint32_t FLAG_COMPRESSION = (1 << 0);
static const uint32_t FLAG_UNKNOWN1    = (1 << 1);
static const uint32_t FLAG_UNKNOWN2    = (1 << 2);
static const uint32_t FLAG_PADDING     = (1 << 3);

// ─── Helpers ──────────────────────────────────────────────────────────────────
bool pixel_is_astc(uint32_t type);
bool pixel_is_astc_srgb(uint32_t type);
void pixel_astc_blocks(uint32_t type, int& bw, int& bh);
size_t astc_level_size(int w, int h, int bw, int bh);
size_t raw_level_size(int w, int h, uint32_t pixel_type);
bool pixel_is_raw(uint32_t pixel_type);
std::string pixel_type_name(uint32_t pixel_type);

// ─── Structures ───────────────────────────────────────────────────────────────
struct MipLevel {
    uint16_t width;
    uint16_t height;
    uint32_t offset;
    std::vector<uint8_t> hash;
};

struct SCTXFile {
    uint32_t pixel_type   = PT_ASTC_8x8;
    uint16_t width        = 0;
    uint16_t height       = 0;
    uint8_t  levels_count = 1;
    uint32_t flags        = 0;
    uint32_t tex_len      = 0;

    bool use_compression() const { return (flags & FLAG_COMPRESSION) != 0; }
    bool use_padding()     const { return (flags & FLAG_PADDING)     != 0; }
    bool unknown_flag1()   const { return (flags & FLAG_UNKNOWN1)    != 0; }
    bool unknown_flag2()   const { return (flags & FLAG_UNKNOWN2)    != 0; }

    std::vector<MipLevel> levels;
    std::vector<uint8_t>  texture_data;
};

bool parse_sctx(const uint8_t* data, size_t size, SCTXFile& out, std::string& error);
bool build_sctx(const SCTXFile& file,
                const std::vector<uint8_t>& texture_data,
                std::vector<uint8_t>& out_file,
                std::string& error);
