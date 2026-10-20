// ASTC decoder/encoder using ARM astcenc library (v5.3.0)
#include "astc_decoder.h"
#include "sctx_parser.h"   // для pixel_is_astc_srgb
#include <cstring>
#include <vector>
#include <thread>
#include <algorithm>
#include <android/log.h>
#include <astcenc.h>

#define TAG "QwSCTX_ASTC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/*
 * Число потоков для astcenc.
 *
 * astcenc — это SPMD-библиотека: вызывающий код сам
 * должен создать N потоков и каждый вызывает
 * astcenc_compress_image/decompress_image со своим
 * thread_index (см. astcenc.h, раздел про
 * "worker thread pool"). Раньше здесь всегда был
 * жёстко захардкожен один поток — на многоядерных
 * Android-устройствах это оставляло на столе большую
 * часть доступной производительности при декодировании
 * атласа на каждое открытие SC-файла.
 *
 * Ограничиваем сверху 8 — astcenc's internal work
 * scheduling не даёт линейного прироста после
 * определённого числа потоков, а на некоторых
 * устройствах слишком много потоков только добавляет
 * overhead на переключение контекста.
 */
static unsigned int astc_thread_count() {

    unsigned int hw =
        std::thread::hardware_concurrency();

    if (hw == 0)
        hw = 4; // разумный дефолт, если API не смог определить

    return std::min(hw, 8u);
}

bool astc_decode(const uint8_t* data, size_t data_size,
                 int width, int height,
                 int block_w, int block_h,
                 std::vector<uint8_t>& output,
                 bool srgb)
{
    output.assign((size_t)width * height * 4, 0);

    astcenc_config config;
    astcenc_profile profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;

    astcenc_error status = astcenc_config_init(
        profile,
        (unsigned int)block_w, (unsigned int)block_h, 1,
        ASTCENC_PRE_MEDIUM,
        ASTCENC_FLG_DECOMPRESS_ONLY,
        &config
    );
    if(status != ASTCENC_SUCCESS) {
        LOGE("astcenc_config_init failed: %s", astcenc_get_error_string(status));
        return false;
    }

    unsigned int threads = astc_thread_count();

    astcenc_context* ctx = nullptr;
    status = astcenc_context_alloc(&config, threads, &ctx);
    if(status != ASTCENC_SUCCESS) {
        LOGE("astcenc_context_alloc failed: %s", astcenc_get_error_string(status));
        return false;
    }

    astcenc_swizzle swizzle = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

    uint8_t* out_ptr = output.data();
    astcenc_image image;
    image.dim_x     = (unsigned int)width;
    image.dim_y     = (unsigned int)height;
    image.dim_z     = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data      = reinterpret_cast<void**>(&out_ptr);

    /*
     * Запускаем worker-потоки согласно паттерну из
     * документации astcenc.h: каждый поток вызывает
     * astcenc_decompress_image с уникальным thread_index,
     * работа распределяется библиотекой динамически.
     */
    std::vector<astcenc_error> results(threads, ASTCENC_SUCCESS);

    if (threads <= 1) {

        results[0] =
            astcenc_decompress_image(
                ctx, data, data_size, &image, &swizzle, 0
            );

    } else {

        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (unsigned int t = 0; t < threads; t++) {

            workers.emplace_back([&, t]() {

                results[t] =
                    astcenc_decompress_image(
                        ctx, data, data_size, &image, &swizzle, t
                    );
            });
        }

        for (auto& w : workers)
            w.join();
    }

    status = ASTCENC_SUCCESS;
    for (auto r : results) {
        if (r != ASTCENC_SUCCESS) { status = r; break; }
    }

    if(status != ASTCENC_SUCCESS) {
        LOGE("astcenc_decompress_image failed: %s (block=%dx%d img=%dx%d size=%zu)",
             astcenc_get_error_string(status), block_w, block_h, width, height, data_size);
    }

    astcenc_context_free(ctx);
    return status == ASTCENC_SUCCESS;
}

// Перегрузка без srgb (для совместимости)
bool astc_decode(const uint8_t* data, size_t data_size,
                 int width, int height,
                 int block_w, int block_h,
                 std::vector<uint8_t>& output)
{
    return astc_decode(data, data_size, width, height, block_w, block_h, output, false);
}

static bool astc_encode_impl(const uint8_t* rgba, int width, int height,
                             int block_w, int block_h,
                             std::vector<uint8_t>& output, float preset);

bool astc_encode(const uint8_t* rgba, int width, int height,
                 int block_w, int block_h,
                 std::vector<uint8_t>& output)
{
    return astc_encode_impl(rgba, width, height, block_w, block_h, output, ASTCENC_PRE_MEDIUM);
}

// Быстрый пресет: для больших текстур gif2sc.
bool astc_encode_fast(const uint8_t* rgba, int width, int height,
                      int block_w, int block_h,
                      std::vector<uint8_t>& output)
{
    return astc_encode_impl(rgba, width, height, block_w, block_h, output, ASTCENC_PRE_FAST);
}

static bool astc_encode_impl(const uint8_t* rgba, int width, int height,
                             int block_w, int block_h,
                             std::vector<uint8_t>& output, float preset)
{
    int bx = (width  + block_w - 1) / block_w;
    int by = (height + block_h - 1) / block_h;
    output.resize((size_t)bx * by * 16, 0);

    astcenc_config config;
    astcenc_error status = astcenc_config_init(
        ASTCENC_PRF_LDR,
        (unsigned int)block_w, (unsigned int)block_h, 1,
        preset, 0, &config
    );
    if(status != ASTCENC_SUCCESS) {
        LOGE("astcenc_config_init (encode) failed: %s", astcenc_get_error_string(status));
        return false;
    }

    unsigned int threads = astc_thread_count();

    astcenc_context* ctx = nullptr;
    status = astcenc_context_alloc(&config, threads, &ctx);
    if(status != ASTCENC_SUCCESS) {
        LOGE("astcenc_context_alloc (encode) failed: %s", astcenc_get_error_string(status));
        return false;
    }

    astcenc_swizzle swizzle = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };

    const uint8_t* in_ptr = rgba;
    astcenc_image image;
    image.dim_x     = (unsigned int)width;
    image.dim_y     = (unsigned int)height;
    image.dim_z     = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data      = reinterpret_cast<void**>(const_cast<uint8_t**>(&in_ptr));

    std::vector<astcenc_error> results(threads, ASTCENC_SUCCESS);

    if (threads <= 1) {

        results[0] =
            astcenc_compress_image(
                ctx, &image, &swizzle,
                output.data(), output.size(), 0
            );

    } else {

        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (unsigned int t = 0; t < threads; t++) {

            workers.emplace_back([&, t]() {

                results[t] =
                    astcenc_compress_image(
                        ctx, &image, &swizzle,
                        output.data(), output.size(), t
                    );
            });
        }

        for (auto& w : workers)
            w.join();
    }

    status = ASTCENC_SUCCESS;
    for (auto r : results) {
        if (r != ASTCENC_SUCCESS) { status = r; break; }
    }

    if(status != ASTCENC_SUCCESS) {
        LOGE("astcenc_compress_image failed: %s", astcenc_get_error_string(status));
    }

    astcenc_context_free(ctx);
    return status == ASTCENC_SUCCESS;
}
