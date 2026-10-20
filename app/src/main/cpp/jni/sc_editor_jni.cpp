/**
 * sc_editor_jni.cpp
 * JNI для редактора SC:
 * - nativeGetScInfo: извлекает PNG атлас + список спрайтов
 */
#include <jni.h>
#include <string>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <android/log.h>
#include "sc_core.h"
#include "sc2_reader.h"
#include "stb/stb_image_write.h"
#include "astc_decoder.h"

#define TAG "ScEditor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef SCTOOL_JNI_CLASS
#define SCTOOL_JNI_CLASS Java_com_compose_sctool_ScProcessor
#endif

#define _CAT(a,b) a##b
#define CAT(a,b) _CAT(a,b)
#define FN(name) CAT(SCTOOL_JNI_CLASS, name)

static const uint8_t KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

struct ASTCFmt {
    uint32_t gl;
    int bw, bh;
};

static const ASTCFmt ASTC_FMTS[] = {
    {0x93B0,4,4},{0x93B1,5,4},{0x93B2,5,5},{0x93B3,6,5},
    {0x93B4,6,6},{0x93B5,8,5},{0x93B6,8,6},{0x93B7,8,8},
    {0x93B8,10,5},{0x93B9,10,6},{0x93BA,10,8},{0x93BB,10,10},
    {0x93BC,12,10},{0x93BD,12,12},
    {0x93D0,4,4},{0x93D1,5,4},{0x93D2,5,5},{0x93D3,6,5},
    {0x93D4,6,6},{0x93D5,8,5},{0x93D6,8,6},{0x93D7,8,8},
    {0x93D8,10,5},{0x93D9,10,6},{0x93DA,10,8},{0x93DB,10,10},
    {0x93DC,12,10},{0x93DD,12,12},{0,0,0}
};

/*
 * Пишет атлас как raw RGBA (8-байтный заголовок
 * width/height + пиксели), вместо PNG.
 *
 * PNG-кодирование (DEFLATE) на native стороне и
 * повторное PNG-декодирование на Kotlin стороне —
 * чистая избыточность, когда картинка уже готова
 * как RGBA в памяти. Раньше это дублировалось в
 * трёх разных местах — вынесено в одну функцию,
 * чтобы формат не рассинхронизировался между путями.
 *
 * Имя файла остаётся "*_atlas.raw" — единый контракт
 * для loadScAtlas на Kotlin-стороне.
 */
static bool write_raw_atlas(
        const std::string& out_path,
        int w,
        int h,
        const std::vector<uint8_t>& rgba) {

    FILE* fp =
        fopen(out_path.c_str(), "wb");

    if (!fp)
        return false;

    uint32_t hdr[2] = {
        (uint32_t)w,
        (uint32_t)h
    };

    bool ok =
        fwrite(hdr, sizeof(hdr), 1, fp) == 1 &&
        fwrite(
            rgba.data(),
            1,
            rgba.size(),
            fp) == rgba.size();

    fclose(fp);

    return ok;
}

// Декодировать KTX блок в RGBA
static bool decode_ktx(
        const uint8_t* d,
        size_t sz,
        int& w,
        int& h,
        std::vector<uint8_t>& rgba) {

    if (sz < 64 || memcmp(d, KTX_MAGIC, 12) != 0)
        return false;

    size_t p = 12;

    auto rd32 = [&]() -> uint32_t {
        if (p + 4 > sz)
            return 0;

        uint32_t v;
        memcpy(&v, d + p, 4);
        p += 4;
        return v;
    };

    rd32(); // endianness

    uint32_t glType = rd32();
    rd32(); // glTypeSize
    uint32_t glFmt = rd32();
    uint32_t glInt = rd32();

    rd32(); // baseInternalFormat

    w = (int)rd32();
    h = (int)rd32();

    rd32(); // pixelDepth
    rd32(); // numberOfArrayElements
    rd32(); // numberOfFaces
    rd32(); // numberOfMipmapLevels

    uint32_t kvd = rd32();

    if (p + kvd > sz)
        return false;

    p += kvd;

    uint32_t imgSize = rd32();

    if (p + imgSize > sz)
        imgSize = (uint32_t)(sz - p);

    if (glType == 0 && glFmt == 0) {
        int bw = 8;
        int bh = 8;

        for (auto* f = ASTC_FMTS; f->gl; ++f) {
            if (f->gl == glInt) {
                bw = f->bw;
                bh = f->bh;
                break;
            }
        }

        bool srgb = (glInt >= 0x93D0);

        return astc_decode(
            d + p,
            imgSize,
            w,
            h,
            bw,
            bh,
            rgba,
            srgb
        );
    }

    size_t needed = (size_t)w * (size_t)h * 4;

    if (p + needed > sz)
        return false;

    rgba.assign(d + p, d + p + needed);

    return true;
}

// Информация о спрайте.
/*
 * Реальная геометрия одного draw-command Shape'а —
 * не bounding box, а фактический контур вершин
 * из ShapeDrawBitmapCommand.
 *
 * У одного Shape может быть несколько draw-команд
 * (несколько кусков текстуры), поэтому у спрайта
 * может быть несколько полигонов.
 */
struct SpritePolygon {
    int texture_index;

    // UV-точки в пикселях атласа, в порядке вершин
    // (не min/max — реальный контур).
    std::vector<std::pair<float,float>> uv;
};

struct SpriteInfo {
    int id;
    std::string name;

    // UV в пикселях атласа.
    float u0;
    float v0;
    float u1;
    float v1;

    // Размер в игровых координатах.
    float w;
    float h;

    // Реальная геометрия (может быть пустой, если
    // формат/версия файла не даёт вершин).
    std::vector<SpritePolygon> polygons;
};

/*
 * Форматирует одну строку вывода для спрайта:
 *
 *   id|name|u0|v0|u1|v1|w|h|geometry
 *
 * geometry (может отсутствовать, если у спрайта
 * нет полигонов — тогда строка заканчивается на
 * восьмом поле, как раньше, полностью обратно
 * совместимо со старым форматом):
 *
 *   textureIndex:x,y;x,y;...~textureIndex:x,y;...
 *
 * Несколько полигонов одного спрайта разделяются
 * символом '~'.
 */
static std::string serialize_sprite_line(
        const SpriteInfo& s) {

    std::string nm =
        s.name.empty()
        ? (
            "sprite_" +
            std::to_string(s.id)
          )
        : s.name;

    std::ostringstream out;

    out
        << s.id << "|"
        << nm << "|"
        << s.u0 << "|"
        << s.v0 << "|"
        << s.u1 << "|"
        << s.v1 << "|"
        << s.w  << "|"
        << s.h;

    if (!s.polygons.empty()) {

        out << "|";

        for (size_t pi = 0;
             pi < s.polygons.size();
             ++pi) {

            if (pi > 0)
                out << "~";

            const auto& poly =
                s.polygons[pi];

            out
                << poly.texture_index
                << ":";

            for (size_t vi = 0;
                 vi < poly.uv.size();
                 ++vi) {

                if (vi > 0)
                    out << ";";

                out
                    << poly.uv[vi].first
                    << ","
                    << poly.uv[vi].second;
            }
        }
    }

    out << "\n";

    return out.str();
}

/*
 * Декодирует ОДНУ текстуру SC2-файла по индексу и
 * возвращает текстовый ответ для Kotlin-стороны.
 *
 * Формат вывода:
 *
 *   TEXTURES:0=4096x4096,1=4096x4096,...,8=1404x1496
 *   id|name|u0|v0|u1|v1|w|h
 *   id|name|u0|v0|u1|v1|w|h
 *   ...
 *
 * Первая строка — метаданные ВСЕХ текстур файла (для
 * UI multi-texture selector), без их декодирования —
 * дёшево по памяти, чтобы Kotlin мог сразу показать
 * список текстур не дожидаясь декодирования каждой.
 *
 * Остальные строки — только те спрайты, чей
 * texture_index совпадает с запрошенным — раньше
 * (до multi-texture) в вывод попадали ВСЕ спрайты
 * файла с координатами, посчитанными от размеров
 * texture[0], что было в принципе неверно для
 * спрайтов на других текстурах.
 *
 * Декодируется в память и пишется на диск только
 * текстура texture_index — остальные (потенциально
 * десятки мегабайт каждая на файлах вроде ui.sc)
 * не трогаются вообще, что и устраняет риск
 * OutOfMemoryError на больших многотекстурных SC.
 */
static std::string sc2_render_texture(
        JNIEnv* env,
        const Sc2File& sc2,
        const std::string& sc_path,
        const std::string& out_dir,
        int texture_index) {

    (void)env;

    if (texture_index < 0 ||
        (size_t)texture_index >= sc2.textures.size()) {

        return "Error: texture index out of range";
    }

    auto& tex =
        sc2.textures[texture_index].highres;

    if (!tex.present) {

        return "Error: requested texture has no highres data";
    }

    if (tex.data_off + tex.data_len >
        sc2.textures_chunk.size()) {

        return "Error: SC2 texture data out of range";
    }

    const uint8_t* tex_data =
        sc2.textures_chunk.data() +
        tex.data_off;

    int aw = 0;
    int ah = 0;

    std::vector<uint8_t> rgba;

    bool decoded = false;

    /*
     * format==8 (KhronosTexture) -> данные это
     * готовый KTX-блок, декодируем как обычно.
     *
     * Иначе (raw RGBA/подобные форматы) —
     * читаем напрямую по known width/height.
     */
    if (tex.format == 8 &&
        tex.data_len >= 12 &&
        memcmp(tex_data, KTX_MAGIC, 12) == 0) {

        decoded =
            decode_ktx(
                tex_data,
                tex.data_len,
                aw,
                ah,
                rgba
            );

    } else {

        aw = tex.width;
        ah = tex.height;

        size_t needed =
            (size_t)aw * (size_t)ah * 4;

        if (tex.data_len >= needed) {

            rgba.assign(
                tex_data,
                tex_data + needed
            );

            decoded = true;
        }
    }

    if (!decoded || aw <= 0 || ah <= 0) {
        return "Error: SC2 texture decode failed";
    }

    std::string stem =
        sc_basename_no_ext(sc_path);

    std::string png_path =
        out_dir + "/" +
        stem +
        "_atlas.raw";

    if (!write_raw_atlas(
            png_path,
            aw,
            ah,
            rgba)) {

        return "Error: raw atlas write failed";
    }

    std::ostringstream out;

    // Первая строка: метаданные всех текстур файла.
    out << "TEXTURES:";

    for (size_t i = 0;
         i < sc2.textures.size();
         ++i) {

        if (i > 0)
            out << ",";

        auto& t =
            sc2.textures[i].highres;

        out
            << i << "="
            << (t.present ? t.width : 0)
            << "x"
            << (t.present ? t.height : 0);
    }

    out << "\n";

    /*
     * id|name|u0|v0|u1|v1|w|h — тот же текстовый
     * контракт, что и у старого (v0/SC1) пути,
     * чтобы Kotlin-сторона не менялась.
     *
     * Только спрайты этой конкретной текстуры —
     * определяем texture_index по первой
     * draw-команде Shape (как и раньше делали
     * для sc2_png2sc Replace).
     */
    for (auto& s : sc2.shapes) {

        uint32_t shape_tex = 0;

        if (!s.commands.empty())
            shape_tex =
                s.commands[0].texture_index;

        if ((int)shape_tex != texture_index)
            continue;

        float min_u, min_v, max_u, max_v;

        if (!sc2_shape_uv_bbox(
                s, min_u, min_v, max_u, max_v))
            continue;

        std::string nm =
            s.export_name.empty()
            ? (
                "sprite_" +
                std::to_string(s.id)
              )
            : s.export_name;

        float u0 = min_u * aw;
        float v0 = min_v * ah;
        float u1 = max_u * aw;
        float v1 = max_v * ah;

        out
            << s.id << "|"
            << nm << "|"
            << u0 << "|"
            << v0 << "|"
            << u1 << "|"
            << v1 << "|"
            << (u1 - u0) << "|"
            << (v1 - v0);

        /*
         * Geometry: те же draw-команды, что дали
         * bbox выше, только теперь сохраняем полный
         * контур вершин, а не только min/max.
         *
         * commands у Sc2Shape уже фильтруются по
         * их собственному texture_index — сюда
         * попадают только вершины, реально лежащие
         * на текущей запрошенной текстуре.
         */
        bool any_poly = false;

        for (size_t ci = 0;
             ci < s.commands.size();
             ++ci) {

            const auto& cmd =
                s.commands[ci];

            if (cmd.texture_index !=
                (uint32_t)texture_index)
                continue;

            if (cmd.vertices.size() < 3)
                continue;

            out
                << (any_poly ? "~" : "|")
                << cmd.texture_index
                << ":";

            for (size_t vi = 0;
                 vi < cmd.vertices.size();
                 ++vi) {

                if (vi > 0)
                    out << ";";

                out
                    << (cmd.vertices[vi].u * aw)
                    << ","
                    << (cmd.vertices[vi].v * ah);
            }

            any_poly = true;
        }

        out << "\n";
    }

    return out.str();
}

static std::vector<SpriteInfo> parse_shapes(
        const std::vector<uint8_t>& raw,
        int atlas_w,
        int atlas_h) {

    std::vector<SpriteInfo> result;

    size_t p = 0;

    auto r8 = [&]() -> uint8_t {
        if (p >= raw.size())
            return 0;

        return raw[p++];
    };

    auto r16 = [&]() -> uint16_t {
        if (p + 2 > raw.size()) {
            p = raw.size();
            return 0;
        }

        uint16_t v;
        memcpy(&v, raw.data() + p, 2);
        p += 2;

        return v;
    };

    // ВАЖНО:
    // X/Y в ShapeDrawBitmapCommand хранятся как 32-bit twips.
    auto r32 = [&]() -> uint32_t {
        if (p + 4 > raw.size()) {
            p = raw.size();
            return 0;
        }

        uint32_t v;
        memcpy(&v, raw.data() + p, 4);
        p += 4;

        return v;
    };

    auto rstr = [&]() -> std::string {
        uint8_t n = r8();

        size_t len = n;

        if (n == 0xFF)
            len = r16();

        if (p + len > raw.size()) {
            p = raw.size();
            return {};
        }

        std::string v(
            (const char*)raw.data() + p,
            len
        );

        p += len;

        return v;
    };

    /*
     * SC header:
     *
     * six uint16 values
     * 5 bytes matrix-bank header
     * export count
     * export IDs
     * export names
     */
    if (raw.size() < 19)
        return result;

    uint16_t shape_c = r16();
    uint16_t mc_c    = r16();
    uint16_t tex_c   = r16();
    uint16_t tf_c    = r16();
    uint16_t mat_c   = r16();
    uint16_t ct_c    = r16();

    (void)shape_c;
    (void)mc_c;
    (void)tex_c;
    (void)tf_c;
    (void)mat_c;
    (void)ct_c;

    // sc-viewer: после шести short идёт 5 байт ScMatrixBank header.
    if (p + 5 > raw.size())
        return result;

    p += 5;

    uint16_t ec = r16();

    if (ec > 10000)
        return result;

    std::vector<uint16_t> exp_ids(ec);

    for (auto& id : exp_ids)
        id = r16();

    std::vector<std::string> exp_names(ec);

    for (auto& name : exp_names)
        name = rstr();

    /*
     * Export ID -> name.
     */
    std::map<uint16_t, std::string> mc_to_name;

    for (int i = 0; i < (int)ec; ++i)
        mc_to_name[exp_ids[i]] = exp_names[i];

    /*
     * MovieClip ID -> все children (не только первый).
     *
     * Нужно, чтобы отличить содержательный Shape
     * (уникальный портрет/иконку) от служебных
     * переиспользуемых Shape (общая рамка/фон,
     * одинаковые у разных экспортов).
     */
    std::map<uint16_t, std::vector<uint16_t>> mc_all_children;

    size_t tags_start = p;

    while (p + 5 <= raw.size()) {

        uint8_t tag = r8();

        int32_t len = (int32_t)r32();

        if (tag == 0)
            break;

        if (len < 0 ||
            p + (size_t)len > raw.size())
            break;

        size_t start = p;
        size_t end = p + (size_t)len;

        /*
         * MovieClip tags:
         * 3, 12, 13, 14, 35, 49
         */
        bool is_mc =
            tag == 3  ||
            tag == 12 ||
            tag == 13 ||
            tag == 14 ||
            tag == 35 ||
            tag == 49;

        if (is_mc && end - start >= 5) {

            uint16_t mc_id = r16();

            r8(); // FPS

            uint16_t frame_c = r16();
            (void)frame_c;

            bool custom = (tag == 49);
            bool flattened =
                (tag != 3 &&
                 tag != 14);

            if (custom && p < end) {

                uint8_t prop_c = r8();

                for (uint8_t i = 0;
                     i < prop_c && p < end;
                     ++i) {

                    r8();
                    r8();
                }
            }

            if (flattened && p + 4 <= end) {

                uint32_t element_c = r32();

                size_t bytes =
                    (size_t)element_c * 6;

                if (bytes <= end - p)
                    p += bytes;
                else
                    p = end;
            }

            if (p + 2 <= end) {

                uint16_t child_c = r16();

                std::vector<uint16_t> children;

                for (uint16_t k = 0;
                     k < child_c &&
                     p + 2 <= end;
                     ++k) {

                    children.push_back(r16());
                }

                mc_all_children[mc_id] =
                    children;
            }
        }

        p = end;
    }

    /*
     * Export -> Shape.
     *
     * Для каждого экспорта собираем ВСЕ leaf-Shape
     * в его поддереве MovieClip (рекурсивно, не только
     * первого ребёнка).
     *
     * Один и тот же служебный Shape (общая рамка/фон)
     * часто переиспользуется у многих разных экспортов —
     * такие исключаем по частоте встречаемости.
     *
     * Экспорту присваивается тот leaf-Shape, который
     * уникален (или наименее переиспользуем) в его
     * поддереве — это и есть содержательная картинка,
     * а не общий декоративный элемент.
     */

    // Собираем leaf-потомков для каждого экспорта.
    std::vector<std::vector<uint16_t>> export_leaves(ec);

    for (int i = 0; i < (int)ec; ++i) {

        std::set<uint16_t> visited;
        std::vector<uint16_t> frontier = {
            exp_ids[i]
        };

        std::vector<uint16_t> leaves;

        int depth = 0;

        while (!frontier.empty() &&
               depth < 16) {

            std::vector<uint16_t> next;

            for (uint16_t id : frontier) {

                auto it =
                    mc_all_children.find(id);

                if (it == mc_all_children.end()) {

                    // Не MovieClip -> это leaf Shape.
                    leaves.push_back(id);
                    continue;
                }

                for (uint16_t child : it->second) {

                    if (child == 0xFFFF)
                        continue;

                    if (visited.insert(child).second)
                        next.push_back(child);
                }
            }

            frontier = next;
            depth++;
        }

        export_leaves[i] = leaves;
    }

    // Частота: сколько разных экспортов используют
    // данный leaf Shape в своём поддереве.
    std::map<uint16_t, int> leaf_usage_count;

    for (int i = 0; i < (int)ec; ++i) {

        std::set<uint16_t> unique_leaves(
            export_leaves[i].begin(),
            export_leaves[i].end()
        );

        for (uint16_t leaf : unique_leaves)
            leaf_usage_count[leaf]++;
    }

    std::map<uint16_t, std::string> shape_to_name;

    for (int i = 0; i < (int)ec; ++i) {

        if (export_leaves[i].empty())
            continue;

        /*
         * Кандидаты, отсортированные по частоте
         * переиспользования (по возрастанию) —
         * сначала самые уникальные.
         *
         * Если лучший кандидат уже занят другим
         * экспортом (например, общий child между
         * "_big" и "_small" вариантами одного героя),
         * пробуем следующего по редкости, а не
         * отказываемся от имени вовсе.
         */
        std::vector<uint16_t> candidates =
            export_leaves[i];

        std::sort(
            candidates.begin(),
            candidates.end(),
            [&](uint16_t a, uint16_t b) {

                int ca =
                    leaf_usage_count[a];

                int cb =
                    leaf_usage_count[b];

                if (ca != cb)
                    return ca < cb;

                return a < b;
            }
        );

        for (uint16_t candidate : candidates) {

            if (shape_to_name.find(candidate) ==
                shape_to_name.end()) {

                shape_to_name[candidate] =
                    exp_names[i];

                break;
            }
        }
    }

    /*
     * Парсим Shape / Shape_2.
     *
     * Shape_2 = tag 18.
     */
    p = tags_start;

    while (p + 5 <= raw.size()) {

        uint8_t tag = r8();

        int32_t len = (int32_t)r32();

        if (tag == 0)
            break;

        if (len < 0 ||
            p + (size_t)len > raw.size())
            break;

        size_t start = p;
        size_t end = p + (size_t)len;

        if ((tag == 2 || tag == 18) &&
            end - start >= 4) {

            uint16_t sh_id = r16();

            uint16_t draw_c = r16();

            uint16_t point_c =
                (tag == 18 && p + 2 <= end)
                ? r16()
                : (uint16_t)(draw_c * 4);

            (void)point_c;

            float min_u = 1e30f;
            float min_v = 1e30f;

            float max_u = -1e30f;
            float max_v = -1e30f;

            float min_x = 1e30f;
            float min_y = 1e30f;

            float max_x = -1e30f;
            float max_y = -1e30f;

            bool got = false;

            /*
             * Полигоны этого Shape — по одному на
             * каждую bitmap draw-команду с вершинами.
             */
            std::vector<SpritePolygon> shape_polygons;

            size_t q = p;

            for (uint16_t d = 0;
                 d < draw_c &&
                 q + 5 <= end;
                 ++d) {

                uint8_t ctag = raw[q++];

                int32_t clen;

                memcpy(
                    &clen,
                    raw.data() + q,
                    4
                );

                q += 4;

                if (clen < 0 ||
                    q + (size_t)clen > end)
                    break;

                size_t ce =
                    q + (size_t)clen;

                /*
                 * Bitmap commands:
                 *
                 * 4  = ShapeDrawBitmapCommand
                 * 17 = ShapeDrawBitmapCommand_2
                 * 22 = ShapeDrawBitmapCommand_3
                 */
                bool bitmap =
                    ctag == 4 ||
                    ctag == 17 ||
                    ctag == 22;

                if (bitmap && q < ce) {

                    // Texture index.
                    uint8_t texture_index =
                        raw[q++];

                    uint16_t vc = 4;

                    if (ctag != 4 && q < ce)
                        vc = raw[q++];

                    /*
                     * Один vertex:
                     *
                     * X int32
                     * Y int32
                     * U uint16
                     * V uint16
                     *
                     * = 12 bytes
                     *
                     * Причём формат хранит:
                     *
                     * X всех вершин
                     * Y всех вершин
                     * U/V всех вершин
                     *
                     * Поэтому читаем X/Y отдельно.
                     */
                    if (vc == 0 ||
                        q + (size_t)vc * 12 > ce) {

                        q = ce;
                        continue;
                    }

                    std::vector<int32_t> xs(vc);
                    std::vector<int32_t> ys(vc);

                    // Все X/Y идут последовательно.
                    for (uint16_t i = 0;
                         i < vc;
                         ++i) {

                        memcpy(
                            &xs[i],
                            raw.data() + q,
                            4
                        );

                        q += 4;

                        memcpy(
                            &ys[i],
                            raw.data() + q,
                            4
                        );

                        q += 4;
                    }

                    // UV.

                    SpritePolygon poly;
                    poly.texture_index =
                        (int)texture_index;
                    poly.uv.reserve(vc);

                    for (uint16_t i = 0;
                         i < vc;
                         ++i) {

                        uint16_t u;
                        uint16_t v;

                        memcpy(
                            &u,
                            raw.data() + q,
                            2
                        );

                        q += 2;

                        memcpy(
                            &v,
                            raw.data() + q,
                            2
                        );

                        q += 2;

                        /*
                         * UV в SC:
                         *
                         * uint16 / 65535
                         */
                        float uf =
                            (float)u /
                            65535.0f *
                            (float)atlas_w;

                        float vf =
                            (float)v /
                            65535.0f *
                            (float)atlas_h;

                        poly.uv.push_back(
                            { uf, vf }
                        );

                        min_u =
                            std::min(min_u, uf);

                        max_u =
                            std::max(max_u, uf);

                        min_v =
                            std::min(min_v, vf);

                        max_v =
                            std::max(max_v, vf);

                        /*
                         * Twips:
                         *
                         * int32 / 20
                         */
                        float xf =
                            (float)xs[i] /
                            20.0f;

                        float yf =
                            (float)ys[i] /
                            20.0f;

                        min_x =
                            std::min(min_x, xf);

                        max_x =
                            std::max(max_x, xf);

                        min_y =
                            std::min(min_y, yf);

                        max_y =
                            std::max(max_y, yf);

                        got = true;
                    }

                    if (poly.uv.size() >= 3) {

                        shape_polygons.push_back(
                            std::move(poly)
                        );
                    }
                }

                q = ce;
            }

            if (got) {

                SpriteInfo si{};

                si.id = sh_id;

                si.u0 = min_u;
                si.v0 = min_v;
                si.u1 = max_u;
                si.v1 = max_v;

                /*
                 * Для редактора прямоугольник спрайта
                 * определяется именно UV в атласе.
                 */
                si.w =
                    max_u - min_u;

                si.h =
                    max_v - min_v;

                /*
                 * Если UV повреждены/нулевые,
                 * используем реальные XY-размеры.
                 */
                if (si.w < 0.5f ||
                    si.h < 0.5f) {

                    si.w =
                        max_x - min_x;

                    si.h =
                        max_y - min_y;
                }

                auto it =
                    shape_to_name.find(sh_id);

                if (it == shape_to_name.end())
                    si.name = "";
                else
                    si.name = it->second;

                si.polygons =
                    std::move(shape_polygons);

                result.push_back(si);
            }
        }

        p = end;
    }

    return result;
}

// ── nativeGetScInfo ───────────────────────────────────────────────────────────

extern "C"
JNIEXPORT jstring JNICALL
Java_com_compose_sctool_ScProcessor_nativeGetScInfo(
        JNIEnv* env,
        jclass,
        jstring sc_path_j,
        jstring out_dir_j) {

    auto jstr =
        [&](jstring s) -> std::string {

        const char* c =
            env->GetStringUTFChars(
                s,
                nullptr
            );

        std::string r(c);

        env->ReleaseStringUTFChars(
            s,
            c
        );

        return r;
    };

    std::string sc_path =
        jstr(sc_path_j);

    std::string out_dir =
        jstr(out_dir_j);

    std::vector<uint8_t> data;

    if (!sc_read_file(sc_path, data))
        return env->NewStringUTF(
            "Error: cannot read SC"
        );

    ScHeader hdr =
        sc_parse_header(data);

    if (!hdr.valid)
        return env->NewStringUTF(
            "Error: not an SC file"
        );

    /*
     * SC2 (v5/v6): совершенно другой внутренний формат
     * (FlatBuffers), обрабатывается отдельным путём
     * через sc2_reader, не через старый tag-based парсер.
     */
    if (hdr.version >= 5) {

        Sc2File sc2;
        std::string sc2_err;

        if (!sc2_parse(data, sc2, &sc2_err)) {

            return env->NewStringUTF(
                (
                    std::string("Error: SC2 parse failed: ") +
                    sc2_err
                ).c_str()
            );
        }

        if (sc2.textures.empty()) {

            return env->NewStringUTF(
                "Error: SC2 file has no texture"
            );
        }

        /*
         * По умолчанию (для обратной совместимости
         * со старым однотекстурным вызовом) берём
         * первую текстуру, у которой есть highres.
         *
         * Явный выбор текстуры — через отдельную
         * функцию nativeGetScTexture ниже, вызываемую
         * из multi-texture selector в UI.
         */
        int texture_index = 0;

        for (size_t i = 0;
             i < sc2.textures.size();
             ++i) {

            if (sc2.textures[i].highres.present) {

                texture_index =
                    (int)i;

                break;
            }
        }

        std::string result =
            sc2_render_texture(
                env,
                sc2,
                sc_path,
                out_dir,
                texture_index
            );

        return env->NewStringUTF(
            result.c_str()
        );
    }

    std::vector<uint8_t> raw;

    try {

        raw =
            sc_decompress(
                data,
                hdr.comp
            );

    } catch (std::exception& e) {

        return env->NewStringUTF(
            (
                std::string("Error: ") +
                e.what()
            ).c_str()
        );
    }

    /*
     * Ищем KTX в основном SC.
     *
     * memchr по первому байту сигнатуры вместо
     * memcmp(12) на каждой позиции — этот поиск
     * выполняется при КАЖДОМ открытии файла на
     * полном декомпрессированном буфере (десятки
     * мегабайт для больших атласов), поэтому именно
     * здесь экономия заметнее всего.
     */
    size_t ktx_off =
        std::string::npos;

    {
        size_t search_from = 0;

        for (;;) {

            if (search_from + 12 > raw.size())
                break;

            const void* found =
                memchr(
                    raw.data() + search_from,
                    KTX_MAGIC[0],
                    raw.size() - search_from - 11
                );

            if (!found)
                break;

            size_t i =
                (const uint8_t*)found -
                raw.data();

            if (memcmp(
                    raw.data() + i,
                    KTX_MAGIC,
                    12) == 0) {

                ktx_off = i;
                break;
            }

            search_from = i + 1;
        }
    }

    /*
     * Если KTX нет, ищем соседний _tex.sc.
     */
    if (ktx_off == std::string::npos) {

        std::string stem =
            sc_basename_no_ext(sc_path);

        std::string dir =
            sc_dirname(sc_path);

        std::string tex_path =
            dir + "/" +
            stem +
            "_tex.sc";

        std::vector<uint8_t> tex_data;

        if (sc_read_file(
                tex_path,
                tex_data)) {

            ScHeader tex_hdr =
                sc_parse_header(tex_data);

            if (tex_hdr.valid) {

                try {

                    std::vector<uint8_t> tex_raw =
                        sc_decompress(
                            tex_data,
                            tex_hdr.comp
                        );

                    /*
                     * Поиск KTX-сигнатуры.
                     *
                     * memchr по первому байту сигнатуры вместо
                     * побайтового memcmp(12) на каждой позиции —
                     * на атласах в десятки мегабайт это резко
                     * сокращает число фактических сравнений
                     * (memchr использует SIMD/векторизацию
                     * платформы, а memcmp вызывался бы миллионы
                     * раз впустую на несовпадающих байтах).
                     */
                    size_t search_from = 0;

                    for (;;) {

                        if (search_from + 12 > tex_raw.size())
                            break;

                        const void* found =
                            memchr(
                                tex_raw.data() + search_from,
                                KTX_MAGIC[0],
                                tex_raw.size() - search_from - 11
                            );

                        if (!found)
                            break;

                        size_t i =
                            (const uint8_t*)found -
                            tex_raw.data();

                        if (memcmp(
                                tex_raw.data() + i,
                                KTX_MAGIC,
                                12) != 0) {

                            search_from = i + 1;
                            continue;
                        }

                        int aw = 0;
                        int ah = 0;

                        std::vector<uint8_t> rgba;

                        size_t blk_end =
                            tex_raw.size();

                        if (!decode_ktx(
                                tex_raw.data() + i,
                                blk_end - i,
                                aw,
                                ah,
                                rgba)) {

                            return env->NewStringUTF(
                                "Error: KTX decode failed (tex)"
                            );
                        }

                        std::string png_path =
                            out_dir + "/" +
                            stem +
                            "_atlas.raw";

                        write_raw_atlas(
                            png_path,
                            aw,
                            ah,
                            rgba
                        );

                        auto sprites =
                            parse_shapes(
                                raw,
                                aw,
                                ah
                            );

                        std::ostringstream out;

                        for (auto& s : sprites) {

                            out <<
                                serialize_sprite_line(s);
                        }

                        return env->NewStringUTF(
                            out.str().c_str()
                        );
                    }

                } catch (...) {
                    // Ниже будет понятная ошибка.
                }
            }
        }

        return env->NewStringUTF(
            "Error: no KTX found (load _tex.sc too)"
        );
    }

    /*
     * Ограничиваем KTX текущим блоком.
     *
     * memchr по первому байту сигнатуры вместо
     * memcmp(12) на каждой позиции — см. комментарий
     * у аналогичного цикла выше, экономия особенно
     * заметна на атласах в десятки мегабайт.
     */
    size_t blk_end =
        raw.size();

    {
        size_t search_from = ktx_off + 12;

        for (;;) {

            if (search_from + 12 > raw.size())
                break;

            const void* found =
                memchr(
                    raw.data() + search_from,
                    KTX_MAGIC[0],
                    raw.size() - search_from - 11
                );

            if (!found)
                break;

            size_t i =
                (const uint8_t*)found -
                raw.data();

            if (memcmp(
                    raw.data() + i,
                    KTX_MAGIC,
                    12) == 0) {

                blk_end = i;
                break;
            }

            search_from = i + 1;
        }
    }

    int aw = 0;
    int ah = 0;

    std::vector<uint8_t> rgba;

    if (!decode_ktx(
            raw.data() + ktx_off,
            blk_end - ktx_off,
            aw,
            ah,
            rgba)) {

        return env->NewStringUTF(
            "Error: KTX decode failed"
        );
    }

    /*
     * Сохраняем атлас как raw RGBA (не PNG).
     *
     * См. комментарий у write_raw_atlas — устраняет
     * PNG round-trip (DEFLATE encode на native +
     * PNG decode на Kotlin), заметно на больших атласах.
     */
    std::string stem =
        sc_basename_no_ext(sc_path);

    std::string png_path =
        out_dir + "/" +
        stem +
        "_atlas.raw";

    if (!write_raw_atlas(
            png_path,
            aw,
            ah,
            rgba)) {

        return env->NewStringUTF(
            "Error: raw atlas write failed"
        );
    }

    /*
     * Парсим Shape.
     */
    auto sprites =
        parse_shapes(
            raw,
            aw,
            ah
        );

    /*
     * id|name|u0|v0|u1|v1|w|h|geometry
     */
    std::ostringstream out;

    for (auto& s : sprites) {

        out <<
            serialize_sprite_line(s);
    }

    return env->NewStringUTF(
        out.str().c_str()
    );
}

// ── nativeGetScTexture ────────────────────────────────────────────────────────

/*
 * Декодирует конкретную текстуру SC2 (v5/v6) файла
 * по индексу — вызывается из multi-texture selector
 * в UI, когда пользователь переключается между
 * текстурами.
 *
 * Для SC1 (v0) файлов возвращает ошибку — у них нет
 * concept'а нескольких текстур в этом смысле, там
 * всегда используется обычный nativeGetScInfo.
 *
 * Декодируется и пишется на диск ТОЛЬКО запрошенная
 * текстура — остальные не трогаются, что критично
 * для файлов с множеством крупных текстур (например,
 * ui.sc с 4096x4096 x7), где декодирование всех сразу
 * рискует упереться в лимит памяти Android-приложения.
 */
extern "C"
JNIEXPORT jstring JNICALL
Java_com_compose_sctool_ScProcessor_nativeGetScTexture(
        JNIEnv* env,
        jclass,
        jstring sc_path_j,
        jstring out_dir_j,
        jint texture_index_j) {

    auto jstr =
        [&](jstring s) -> std::string {

        const char* c =
            env->GetStringUTFChars(
                s,
                nullptr
            );

        std::string r(c);

        env->ReleaseStringUTFChars(
            s,
            c
        );

        return r;
    };

    std::string sc_path =
        jstr(sc_path_j);

    std::string out_dir =
        jstr(out_dir_j);

    std::vector<uint8_t> data;

    if (!sc_read_file(sc_path, data))
        return env->NewStringUTF(
            "Error: cannot read SC"
        );

    ScHeader hdr =
        sc_parse_header(data);

    if (!hdr.valid)
        return env->NewStringUTF(
            "Error: not an SC file"
        );

    if (hdr.version < 5) {

        return env->NewStringUTF(
            "Error: multi-texture selector "
            "is only supported for SC2 (v5/v6) files"
        );
    }

    Sc2File sc2;
    std::string sc2_err;

    if (!sc2_parse(data, sc2, &sc2_err)) {

        return env->NewStringUTF(
            (
                std::string("Error: SC2 parse failed: ") +
                sc2_err
            ).c_str()
        );
    }

    std::string result =
        sc2_render_texture(
            env,
            sc2,
            sc_path,
            out_dir,
            (int)texture_index_j
        );

    return env->NewStringUTF(
        result.c_str()
    );
}
