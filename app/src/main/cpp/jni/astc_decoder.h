#pragma once
#include <cstdint>
#include <vector>

// Декодирует ASTC данные в RGBA8
bool astc_decode(const uint8_t* data, size_t data_size,
                 int width, int height,
                 int block_w, int block_h,
                 std::vector<uint8_t>& output);

// С явным указанием sRGB
bool astc_decode(const uint8_t* data, size_t data_size,
                 int width, int height,
                 int block_w, int block_h,
                 std::vector<uint8_t>& output,
                 bool srgb);

// Кодирует RGBA8 → ASTC
bool astc_encode(const uint8_t* rgba, int width, int height,
                 int block_w, int block_h,
                 std::vector<uint8_t>& output);
