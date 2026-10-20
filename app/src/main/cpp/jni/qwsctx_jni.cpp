#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#include "sctx_parser.h"
#include "astc_decoder.h"
#include "zstd/lib/zstd.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#define TAG "QwSCTX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── JNI helpers ─────────────────────────────────────────────────────────────
static std::string jstr(JNIEnv* env, jstring s) {
    if(!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}
static jstring toj(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}
static bool read_file(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if(!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
static bool write_file(const std::string& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    if(!f) return false;
    f.write((const char*)data, size);
    return f.good();
}
static std::string strip_ext(const std::string& path) {
    auto dot = path.rfind('.');
    return (dot != std::string::npos) ? path.substr(0, dot) : path;
}

// ─── Decode pixel data → RGBA8 ────────────────────────────────────────────────
static std::string decode_level(const SCTXFile& sctx, int lvl,
                                std::vector<uint8_t>& rgba_out)
{
    int w = std::max(1, (int)sctx.width  >> lvl);
    int h = std::max(1, (int)sctx.height >> lvl);

    // Get level data pointer
    uint32_t offset = 0;
    if(lvl < (int)sctx.levels.size()) {
        offset = sctx.levels[lvl].offset;
    }
    if(offset >= sctx.texture_data.size()) {
        return "Offset mip-уровня за пределами данных текстуры";
    }
    const uint8_t* mip_data = sctx.texture_data.data() + offset;
    size_t mip_avail = sctx.texture_data.size() - offset;

    if(pixel_is_astc(sctx.pixel_type)) {
        int bw, bh;
        pixel_astc_blocks(sctx.pixel_type, bw, bh);
        size_t expected = astc_level_size(w, h, bw, bh);
        if(expected > mip_avail) expected = mip_avail;
        LOGI("  ASTC %dx%d block=%dx%d size=%zu", w, h, bw, bh, expected);
        if(!astc_decode(mip_data, expected, w, h, bw, bh, rgba_out, pixel_is_astc_srgb(sctx.pixel_type)))
            return "Ошибка ASTC декодирования";
    } else if(pixel_is_raw(sctx.pixel_type)) {
        // RGBA8 / BGRA8 — copy as-is (4 bytes per pixel)
        size_t needed = (size_t)w * h * 4;
        if(needed > mip_avail) return "Недостаточно данных для RAW текстуры";
        rgba_out.assign(mip_data, mip_data + needed);
        // Swap B↔R for BGRA
        if(sctx.pixel_type == PT_BGRA8Unorm || sctx.pixel_type == 81) {
            for(size_t i = 0; i < rgba_out.size(); i += 4)
                std::swap(rgba_out[i], rgba_out[i+2]);
        }
    } else {
        return "Неподдерживаемый pixel_type: " + pixel_type_name(sctx.pixel_type);
    }
    return "";
}

// ─── Build JSON metadata ──────────────────────────────────────────────────────
static std::string build_json(const SCTXFile& f, const std::string& png_name) {
    std::string j = "{\n";
    j += "  \"texture\": \"" + png_name + "\",\n";
    j += "  \"type\": \"" + pixel_type_name(f.pixel_type) + "\",\n";
    j += "  \"generate_mip_maps\": " + std::string(f.levels_count > 1 ? "true" : "false") + ",\n";
    j += "  \"compressed\": " + std::string(f.use_compression() ? "true" : "false") + ",\n";
    j += "  \"unknown_flag\": " + std::string(f.unknown_flag1() ? "true" : "false") + ",\n";
    j += "  \"unknown_flag_1\": " + std::string(f.unknown_flag2() ? "true" : "false") + ",\n";
    j += "  \"streaming\": { \"ids\": null, \"textures\": null }\n";
    j += "}";
    return j;
}

// ─── Parse JSON field (very simple, single-level) ────────────────────────────
static std::string json_str(const std::string& js, const std::string& key) {
    auto kf = js.find("\"" + key + "\"");
    if(kf == std::string::npos) return "";
    auto col = js.find(':', kf);
    if(col == std::string::npos) return "";
    auto q1 = js.find('"', col+1);
    if(q1 == std::string::npos) return "";
    auto q2 = js.find('"', q1+1);
    if(q2 == std::string::npos) return "";
    return js.substr(q1+1, q2-q1-1);
}
static bool json_bool(const std::string& js, const std::string& key, bool def=false) {
    auto kf = js.find("\"" + key + "\"");
    if(kf == std::string::npos) return def;
    auto col = js.find(':', kf);
    if(col == std::string::npos) return def;
    size_t v = js.find_first_not_of(" \t\r\n", col+1);
    if(v == std::string::npos) return def;
    return js.substr(v, 4) == "true";
}

// Конвертирует строку типа в pixel_type uint32
static uint32_t pixel_type_from_str(const std::string& s) {
    if(s == "RGBA8Unorm")      return PT_RGBA8Unorm;
    if(s == "RGBA8Unorm_sRGB") return PT_RGBA8Unorm_sRGB;
    if(s == "BGRA8Unorm")      return PT_BGRA8Unorm;
    if(s == "ASTC_RGBA8_4x4")  return PT_ASTC_4x4;
    if(s == "ASTC_RGBA8_5x4")  return PT_ASTC_5x4;
    if(s == "ASTC_RGBA8_5x5")  return PT_ASTC_5x5;
    if(s == "ASTC_RGBA8_6x5")  return PT_ASTC_6x5;
    if(s == "ASTC_RGBA8_6x6")  return PT_ASTC_6x6;
    if(s == "ASTC_RGBA8_8x5")  return PT_ASTC_8x5;
    if(s == "ASTC_RGBA8_8x6")  return PT_ASTC_8x6;
    if(s == "ASTC_RGBA8_8x8")  return PT_ASTC_8x8;
    if(s == "ASTC_RGBA8_10x5") return PT_ASTC_10x5;
    if(s == "ASTC_RGBA8_10x8") return PT_ASTC_10x8;
    if(s == "ASTC_RGBA8_10x10")return PT_ASTC_10x10;
    if(s == "ASTC_RGBA8_12x10")return PT_ASTC_12x10;
    if(s == "ASTC_RGBA8_12x12")return PT_ASTC_12x12;
    return PT_RGBA8Unorm; // default
}

extern "C" {

// ─── decode ──────────────────────────────────────────────────────────────────
// Java: public static native String decode(String inputPath, String outputPath, boolean textureOnly);
JNIEXPORT jstring JNICALL
Java_com_qwsctx_app_SctxConverter_decode(
    JNIEnv* env, jclass,
    jstring inputPath, jstring outputPath, jboolean textureOnly)
{
    std::string in  = jstr(env, inputPath);
    std::string out = jstr(env, outputPath);
    std::string base = out.empty() ? strip_ext(in) : strip_ext(out);
    std::string png_path  = base + ".png";
    std::string json_path = base + ".json";

    LOGI("decode: %s → %s (textureOnly=%d)", in.c_str(), base.c_str(), (int)textureOnly);

    std::vector<uint8_t> file_data;
    if(!read_file(in, file_data))
        return toj(env, "Не удалось открыть файл: " + in);

    SCTXFile sctx;
    std::string err;
    if(!parse_sctx(file_data.data(), file_data.size(), sctx, err))
        return toj(env, "Ошибка парсинга: " + err);

    LOGI("  %dx%d type=%s levels=%d compressed=%d",
         sctx.width, sctx.height,
         pixel_type_name(sctx.pixel_type).c_str(),
         sctx.levels_count, (int)sctx.use_compression());

    // Декодируем mip 0
    std::vector<uint8_t> rgba;
    std::string dec_err = decode_level(sctx, 0, rgba);
    if(!dec_err.empty()) return toj(env, dec_err);

    int w = sctx.width, h = sctx.height;
    int ok = stbi_write_png(png_path.c_str(), w, h, 4, rgba.data(), w*4);
    if(!ok) return toj(env, "Не удалось сохранить PNG: " + png_path);
    LOGI("  PNG: %s (%dx%d)", png_path.c_str(), w, h);

    if(!textureOnly) {
        // PNG name относительно директории (только имя файла)
        std::string png_name = png_path;
        auto slash = png_name.rfind('/');
        if(slash == std::string::npos) slash = png_name.rfind('\\');
        if(slash != std::string::npos) png_name = png_name.substr(slash+1);

        std::string json = build_json(sctx, png_name);
        if(!write_file(json_path, json.data(), json.size()))
            return toj(env, "Не удалось сохранить JSON: " + json_path);
        LOGI("  JSON: %s", json_path.c_str());
    }

    return toj(env, "");
}

// ─── encode ──────────────────────────────────────────────────────────────────
// Java: public static native String encode(String inputPath, String outputPath, boolean compress, boolean usePadding);
JNIEXPORT jstring JNICALL
Java_com_qwsctx_app_SctxConverter_encode(
    JNIEnv* env, jclass,
    jstring inputPath, jstring outputPath,
    jboolean compress, jboolean usePadding)
{
    std::string in  = jstr(env, inputPath);
    std::string out = jstr(env, outputPath);
    if(out.empty()) out = strip_ext(in) + ".sctx";
    LOGI("encode: %s → %s", in.c_str(), out.c_str());

    std::string png_path;
    uint32_t pixel_type   = PT_ASTC_8x8; // default как в оригинале
    bool gen_mips         = true;
    bool unk1             = false;
    bool unk2             = true;  // unknown_flag2 = true в оригинале для PNG input

    auto lower_in = in;
    for(auto& c : lower_in) c = (char)tolower((unsigned char)c);
    bool from_json = (lower_in.size() >= 5 &&
                      lower_in.substr(lower_in.size()-5) == ".json");

    if(from_json) {
        std::vector<uint8_t> jdata;
        if(!read_file(in, jdata)) return toj(env, "Не удалось открыть JSON: " + in);
        std::string js(jdata.begin(), jdata.end());

        std::string tex_name = json_str(js, "texture");
        if(tex_name.empty()) return toj(env, "JSON: нет поля \"texture\"");

        // Путь к PNG: если не абсолютный, то рядом с JSON
        if(tex_name[0] != '/' && tex_name[0] != '\\' &&
           !(tex_name.size() > 1 && tex_name[1] == ':')) {
            auto dir = in.rfind('/');
            if(dir == std::string::npos) dir = in.rfind('\\');
            if(dir != std::string::npos)
                png_path = in.substr(0, dir+1) + tex_name;
            else
                png_path = tex_name;
        } else {
            png_path = tex_name;
        }

        std::string type_str = json_str(js, "type");
        if(!type_str.empty()) pixel_type = pixel_type_from_str(type_str);
        gen_mips = json_bool(js, "generate_mip_maps", true);
        unk1     = json_bool(js, "unknown_flag",   false);
        unk2     = json_bool(js, "unknown_flag_1", false);
    } else {
        png_path   = in;
        pixel_type = PT_ASTC_8x8;
        gen_mips   = true;
        unk2       = true; // как в load_default_image
    }

    LOGI("  PNG: %s  pixel_type=%s gen_mips=%d",
         png_path.c_str(), pixel_type_name(pixel_type).c_str(), (int)gen_mips);

    int w, h, ch;
    uint8_t* img = stbi_load(png_path.c_str(), &w, &h, &ch, 4);
    if(!img) return toj(env, "Не удалось загрузить PNG: " + png_path);

    // Вычисляем mip уровни
    int levels = 1;
    if(gen_mips) {
        int mw = w, mh = h;
        while(mw > 1 || mh > 1) { mw = std::max(1,mw/2); mh = std::max(1,mh/2); levels++; }
        levels = std::min(levels, 11);
    }

    // Строим SCTXFile
    SCTXFile sctx;
    sctx.pixel_type   = pixel_type;
    sctx.width        = (uint16_t)w;
    sctx.height       = (uint16_t)h;
    sctx.levels_count = (uint8_t)levels;
    sctx.flags        = 0;
    if(compress)   sctx.flags |= FLAG_COMPRESSION;
    if(unk1)       sctx.flags |= FLAG_UNKNOWN1;
    if(unk2)       sctx.flags |= FLAG_UNKNOWN2;
    if(usePadding) sctx.flags |= FLAG_PADDING;

    // Кодируем все mip уровни
    std::vector<uint8_t> tex_data;
    bool is_astc = pixel_is_astc(pixel_type);
    int bw=8, bh=8;
    if(is_astc) pixel_astc_blocks(pixel_type, bw, bh);

    for(int lvl = 0; lvl < levels; lvl++) {
        int lw = std::max(1, w >> lvl);
        int lh = std::max(1, h >> lvl);

        // Масштабируем
        std::vector<uint8_t> scaled(lw * lh * 4);
        if(lvl == 0) {
            scaled.assign(img, img + (size_t)w*h*4);
        } else {
            // Nearest-neighbor downscale (достаточно для mip)
            for(int y = 0; y < lh; y++) {
                for(int x = 0; x < lw; x++) {
                    int sx = (int)((float)x * w / lw);
                    int sy = (int)((float)y * h / lh);
                    sx = std::min(sx, w-1); sy = std::min(sy, h-1);
                    int src = (sy*w + sx)*4;
                    int dst = (y*lw + x)*4;
                    scaled[dst+0] = img[src+0];
                    scaled[dst+1] = img[src+1];
                    scaled[dst+2] = img[src+2];
                    scaled[dst+3] = img[src+3];
                }
            }
        }

        MipLevel ml;
        ml.width  = (uint16_t)lw;
        ml.height = (uint16_t)lh;
        ml.offset = (uint32_t)tex_data.size();

        std::vector<uint8_t> level_data;
        if(is_astc) {
            if(!astc_encode(scaled.data(), lw, lh, bw, bh, level_data)) {
                stbi_image_free(img);
                return toj(env, "Ошибка ASTC кодирования на уровне " + std::to_string(lvl));
            }
        } else {
            // RAW: для BGRA меняем порядок каналов
            level_data = scaled;
            if(pixel_type == PT_BGRA8Unorm || pixel_type == 81) {
                for(size_t i = 0; i < level_data.size(); i += 4)
                    std::swap(level_data[i], level_data[i+2]);
            }
        }

        tex_data.insert(tex_data.end(), level_data.begin(), level_data.end());
        sctx.levels.push_back(ml);
    }
    stbi_image_free(img);

    LOGI("  levels=%d tex_data=%zu bytes", levels, tex_data.size());

    // Строим файл
    std::vector<uint8_t> out_data;
    std::string build_err;
    if(!build_sctx(sctx, tex_data, out_data, build_err))
        return toj(env, "Ошибка сборки SCTX: " + build_err);

    if(!write_file(out, out_data.data(), out_data.size()))
        return toj(env, "Не удалось сохранить файл: " + out);

    LOGI("  Saved: %s (%zu bytes)", out.c_str(), out_data.size());
    return toj(env, "");
}

// ─── getInfo ─────────────────────────────────────────────────────────────────
// Java: public static native String getInfo(String inputPath);
JNIEXPORT jstring JNICALL
Java_com_qwsctx_app_SctxConverter_getInfo(JNIEnv* env, jclass, jstring inputPath)
{
    std::string in = jstr(env, inputPath);
    std::vector<uint8_t> data;
    if(!read_file(in, data)) return toj(env, "{\"error\":\"File not found\"}");

    SCTXFile sctx;
    std::string err;
    if(!parse_sctx(data.data(), data.size(), sctx, err))
        return toj(env, "{\"error\":\"" + err + "\"}");

    std::string j = "{";
    j += "\"type\":\"" + pixel_type_name(sctx.pixel_type) + "\",";
    j += "\"width\":"  + std::to_string(sctx.width)  + ",";
    j += "\"height\":" + std::to_string(sctx.height) + ",";
    j += "\"levels\":" + std::to_string(sctx.levels_count) + ",";
    j += "\"generate_mip_maps\":" + std::string(sctx.levels_count > 1 ? "true" : "false") + ",";
    j += "\"compressed\":" + std::string(sctx.use_compression() ? "true" : "false") + ",";
    j += "\"padding\":"    + std::string(sctx.use_padding()     ? "true" : "false") + ",";
    j += "\"streaming\":false,";
    j += "\"file_size\":" + std::to_string(data.size());
    j += "}";
    return toj(env, j);
}

// ─── getVersion ──────────────────────────────────────────────────────────────
// Java: public static native String getVersion();
JNIEXPORT jstring JNICALL
Java_com_qwsctx_app_SctxConverter_getVersion(JNIEnv* env, jclass) {
    return toj(env, "QwSCTX Native v2.0");
}

} // extern "C"
