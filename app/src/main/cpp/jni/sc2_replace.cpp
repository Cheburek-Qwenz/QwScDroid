// ── SC2 (v5/v6) Replace ──────────────────────────────────────────────────────
//
// Стратегия: НЕ пересобираем FlatBuffers-структуры с нуля (Header,
// DataStorage, ExportNames, Shapes, MovieClips, Textures остаются
// byte-for-byte как в оригинале).
//
// Вместо этого:
//   1. Находим Shape по ID через sc2_parse (уже даёт export_name,
//      commands с texture_index и UV bbox).
//   2. Декодируем нужную KTX-текстуру (highres) тем же ASTC-декодером,
//      что уже используется в png2sc_op.cpp для SC1.
//   3. Патчим прямоугольник новым PNG (тот же billinear resize).
//   4. Кодируем обратно тем же ASTC block size — размер в байтах
//      гарантированно совпадает со старым (ASTC size зависит только
//      от width/height/block_size, не от контента), поэтому можно
//      заменить байты "на месте" внутри body, не трогая ничего
//      вокруг (Header/DataStorage/ExportNames/Shapes/MovieClips
//      остаются нетронутыми).
//   5. Пересжимаем весь body через zstd и записываем: несжатый
//      Header-дескриптор (без изменений) + новый сжатый body.
//
// Это специально НЕ универсальный FlatBuffers writer — только patch
// уже существующего KTX-blob того же размера. Замена самой текстуры
// на другое разрешение или добавление/удаление спрайтов этим путём
// не поддерживается.

#include "sc_core.h"
#include "sc2_reader.h"
#include "stb/stb_image.h"
#include "astc_decoder.h"
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <sys/stat.h>

/*
 * Polygon geometry helpers — те же алгоритмы, что
 * уже реализованы и протестированы на Kotlin-стороне
 * (ScEditorScreen.kt: sortPointsByAngle,
 * pointInTriangle, polygonContainsPoint).
 *
 * Дублирование намеренное: Replace выполняется
 * полностью на native-стороне и не имеет доступа
 * к уже отсортированным Kotlin ScPolygon.
 *
 * ВАЖНО: порядок вершин в ShapeDrawBitmapCommand/
 * Sc2Command НЕ является порядком обхода контура
 * (см. диагностику на реальных файлах — 0 из 459
 * полигонов были простыми при исходном порядке).
 * Поэтому перед использованием полигона для
 * растеризации вершины обязательно сортируются
 * по углу вокруг центроида.
 */

struct PolyPoint { float x, y; };

static std::vector<PolyPoint> sort_points_by_angle(
        std::vector<PolyPoint> points) {

    if (points.size() < 3)
        return points;

    float cx = 0, cy = 0;

    for (auto& p : points) {
        cx += p.x;
        cy += p.y;
    }

    cx /= (float)points.size();
    cy /= (float)points.size();

    std::sort(
        points.begin(),
        points.end(),
        [&](const PolyPoint& a, const PolyPoint& b) {

            float aa =
                std::atan2(a.y - cy, a.x - cx);

            float ab =
                std::atan2(b.y - cy, b.x - cx);

            return aa < ab;
        }
    );

    return points;
}

static bool point_in_triangle(
        float px, float py,
        float ax, float ay,
        float bx, float by,
        float cx, float cy) {

    /*
     * Тот же фикс, что и в Kotlin-версии: явная
     * проверка площади треугольника, иначе
     * вырожденные (нулевого размера) треугольники
     * дают ложное "внутри" для любой точки плоскости.
     */
    float area =
        (bx - ax) * (cy - ay) -
        (cx - ax) * (by - ay);

    if (std::fabs(area) < 1e-6f)
        return false;

    float d1 =
        (px - bx) * (ay - by) -
        (ax - bx) * (py - by);

    float d2 =
        (px - cx) * (by - cy) -
        (bx - cx) * (py - cy);

    float d3 =
        (px - ax) * (cy - ay) -
        (cx - ax) * (py - ay);

    bool has_neg =
        (d1 < 0) || (d2 < 0) || (d3 < 0);

    bool has_pos =
        (d1 > 0) || (d2 > 0) || (d3 > 0);

    return !(has_neg && has_pos);
}

static bool polygon_contains_point(
        const std::vector<PolyPoint>& points,
        float x,
        float y) {

    if (points.size() < 3)
        return false;

    for (size_t i = 1; i + 1 < points.size(); ++i) {

        if (point_in_triangle(
                x, y,
                points[0].x, points[0].y,
                points[i].x, points[i].y,
                points[i+1].x, points[i+1].y))
            return true;
    }

    return false;
}


static const uint8_t SC2_KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

struct Sc2GLBlock { uint32_t gl; int bw, bh; };
static const Sc2GLBlock SC2_GL_BLOCKS[] = {
    {0x93B0,4,4},{0x93B1,5,4},{0x93B2,5,5},{0x93B3,6,5},
    {0x93B4,6,6},{0x93B5,8,5},{0x93B6,8,6},{0x93B7,8,8},
    {0x93B8,10,5},{0x93B9,10,6},{0x93BA,10,8},{0x93BB,10,10},
    {0x93BC,12,10},{0x93BD,12,12},
    {0x93D0,4,4},{0x93D1,5,4},{0x93D2,5,5},{0x93D3,6,5},
    {0x93D4,6,6},{0x93D5,8,5},{0x93D6,8,6},{0x93D7,8,8},
    {0x93D8,10,5},{0x93D9,10,6},{0x93DA,10,8},{0x93DB,10,10},
    {0x93DC,12,10},{0x93DD,12,12},{0,0,0}
};

static bool sc2_get_block(uint32_t gl, int& bw, int& bh) {
    for (const auto* b = SC2_GL_BLOCKS; b->gl; b++)
        if (b->gl == gl) { bw = b->bw; bh = b->bh; return true; }
    bw = 8; bh = 8;
    return false;
}

// Тот же KTX-декодер, что в png2sc_op.cpp, но самодостаточный здесь
// (не переиспользуем static-функцию из другого .cpp напрямую).
static bool sc2_decode_ktx_rgba(const uint8_t* d, size_t sz, int& w, int& h,
                                 uint32_t& gl, int& bw, int& bh,
                                 size_t& image_start, size_t& image_size,
                                 std::vector<uint8_t>& rgba) {
    if (sz < 64 || memcmp(d, SC2_KTX_MAGIC, 12) != 0) return false;
    size_t p = 12;
    auto rd32 = [&]() -> uint32_t {
        if (p + 4 > sz) return 0;
        uint32_t v; memcpy(&v, d + p, 4); p += 4; return v;
    };
    rd32(); rd32(); rd32(); rd32(); gl = rd32(); rd32();
    w = (int)rd32(); h = (int)rd32();
    rd32(); rd32(); rd32(); rd32();
    uint32_t kvd = rd32();
    if (p + (size_t)kvd + 4 > sz) return false;
    p += kvd;
    uint32_t img = rd32();
    if (p + img > sz) return false;
    image_start = p;
    image_size = img;
    if (!sc2_get_block(gl, bw, bh)) return false;
    bool srgb = (gl >= 0x93D0 && gl <= 0x93DD);
    return astc_decode(d + p, img, w, h, bw, bh, rgba, srgb);
}

// Заново проходит по тому же контейнеру/chunk-layout, что sc2_parse,
// но возвращает "сырые" данные, нужные для patch: полный decompressed
// body, абсолютный offset Textures-chunk payload внутри body, и байты
// несжатого Header-дескриптора (чтобы записать его обратно как есть).
struct Sc2RawLayout {
    std::vector<uint8_t> header_bytes;   // "SC" + version + descriptor_size + descriptor, как есть
    std::vector<uint8_t> body;           // весь decompressed body
    size_t textures_payload_off = 0;     // offset payload Textures chunk внутри body
    size_t textures_payload_len = 0;
    uint32_t external_matrix_bank_size = 0;
    std::vector<uint8_t> external_matrix_bank; // хвост после compressed-блока, если есть
    bool valid = false;
};

static bool sc2_load_raw_layout(const std::vector<uint8_t>& file_data,
                                 Sc2RawLayout& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };

    if (file_data.size() < 10 || file_data[0] != 'S' || file_data[1] != 'C')
        return fail("not an SC file");

    uint8_t version = file_data[2];
    if (version < 5 || version > 6) return fail("not SC2 v5/v6");

    size_t pos = 6;
    if (version == 6) pos += 2;

    if (pos + 4 > file_data.size()) return fail("truncated: no descriptor size");

    auto rd_u32 = [&](size_t o) -> uint32_t {
        uint32_t v; memcpy(&v, file_data.data() + o, 4); return v;
    };

    uint32_t descriptor_size = rd_u32(pos);
    size_t descriptor_at = pos + 4;
    pos += 4;
    if (pos + descriptor_size > file_data.size()) return fail("truncated: descriptor");
    pos += descriptor_size;

    // Header — читаем нужные поля напрямую (та же vtable-логика,
    // co-located с sc2_reader.cpp, но здесь достаточно нескольких
    // полей: resources_offset, compressed_size, external_matrix_bank_size).
    {
        // Минимальный inline-reader вместо подключения fbs:: namespace
        // из sc2_reader.cpp (он static/anonymous там).
        const uint8_t* hb = file_data.data() + descriptor_at;
        size_t hlen = descriptor_size;
        if (hlen < 4) return fail("invalid Header flatbuffer");

        uint32_t root_rel; memcpy(&root_rel, hb, 4);
        size_t table_pos = root_rel;
        if (table_pos + 4 > hlen) return fail("invalid Header root");

        int32_t vt_off; memcpy(&vt_off, hb + table_pos, 4);
        size_t vtable = (size_t)((int64_t)table_pos - vt_off);
        if (vtable + 2 > hlen) return fail("invalid Header vtable");

        uint16_t vtable_size; memcpy(&vtable_size, hb + vtable, 2);

        auto field_u32 = [&](int index, uint32_t def) -> uint32_t {
            size_t entry = 4 + (size_t)index * 2;
            if (entry + 2 > vtable_size) return def;
            uint16_t rel; memcpy(&rel, hb + vtable + entry, 2);
            if (rel == 0) return def;
            size_t field_pos = table_pos + rel;
            if (field_pos + 4 > hlen) return def;
            uint32_t v; memcpy(&v, hb + field_pos, 4);
            return v;
        };

        uint32_t resources_offset          = field_u32(8, 0);
        uint32_t compressed_size           = field_u32(11, 0);
        uint32_t external_matrix_bank_size = field_u32(12, 0);
        (void)resources_offset;

        out.external_matrix_bank_size = external_matrix_bank_size;

        size_t comp_len = compressed_size
            ? compressed_size
            : (file_data.size() - pos - external_matrix_bank_size);
        if (pos + comp_len > file_data.size()) return fail("truncated: compressed body");

        std::vector<uint8_t> comp(file_data.begin() + pos, file_data.begin() + pos + comp_len);

        static const uint8_t ZSTD_MAGIC[4] = {0x28, 0xb5, 0x2f, 0xfd};
        bool is_zstd = comp.size() >= 4 && memcmp(comp.data(), ZSTD_MAGIC, 4) == 0;

        CompInfo ci{0, CompKind::ZSTD};
        try {
            out.body = is_zstd ? sc_decompress(comp, ci) : comp;
        } catch (...) {
            return fail("zstd decompress failed");
        }

        size_t tail_off = pos + comp_len;
        if (external_matrix_bank_size > 0) {
            if (tail_off + external_matrix_bank_size > file_data.size())
                return fail("truncated: external matrix bank");
            out.external_matrix_bank.assign(
                file_data.begin() + tail_off,
                file_data.begin() + tail_off + external_matrix_bank_size
            );
        }

        // Header-дескриптор целиком (magic+version+descriptor_size+descriptor)
        // остаётся байт-в-байт как в оригинале — сохраняем его как есть.
        out.header_bytes.assign(file_data.begin(), file_data.begin() + pos);

        // -- Повторяем chunk-layout body: DataStorage, затем 6 chunk'ов --
        if (out.body.size() < 4) return fail("truncated body: no DataStorage size");

        size_t bpos = 0;
        uint32_t ds_size; memcpy(&ds_size, out.body.data(), 4);
        bpos += 4;
        if (bpos + ds_size > out.body.size()) return fail("truncated: DataStorage");
        bpos += ds_size;

        if (resources_offset < bpos || resources_offset > out.body.size())
            return fail("resources_offset out of range");
        bpos = resources_offset;

        auto skip_chunk = [&](const char* name) -> bool {
            if (bpos + 4 > out.body.size()) return false;
            uint32_t sz; memcpy(&sz, out.body.data() + bpos, 4);
            if (bpos + 4 + sz > out.body.size()) return false;
            bpos += 4 + sz;
            (void)name;
            return true;
        };

        if (!skip_chunk("ExportNames"))        return fail("truncated: ExportNames chunk");
        if (!skip_chunk("TextFields"))         return fail("truncated: TextFields chunk");
        if (!skip_chunk("Shapes"))             return fail("truncated: Shapes chunk");
        if (!skip_chunk("MovieClips"))         return fail("truncated: MovieClips chunk");
        if (!skip_chunk("MovieClipModifiers")) return fail("truncated: MovieClipModifiers chunk");

        // Textures — этот chunk патчим, запоминаем его точные границы.
        if (bpos + 4 > out.body.size()) return fail("truncated: Textures chunk");
        uint32_t tex_sz; memcpy(&tex_sz, out.body.data() + bpos, 4);
        if (bpos + 4 + tex_sz > out.body.size()) return fail("truncated: Textures chunk");

        out.textures_payload_off = bpos + 4;
        out.textures_payload_len = tex_sz;
    }

    out.valid = true;
    return true;
}

// ── Главная функция замены ──────────────────────────────────────────────────

std::string sc2_png2sc(const std::string& png_path, const std::string& orig_sc,
                        int shape_id, const std::string& out_dir) {

    int pw, ph, pch;
    uint8_t* loaded = stbi_load(png_path.c_str(), &pw, &ph, &pch, 4);
    if (!loaded) return "Error: cannot load PNG: " + png_path;
    std::vector<uint8_t> input(loaded, loaded + (size_t)pw * ph * 4);
    stbi_image_free(loaded);

    std::vector<uint8_t> file_data;
    if (!sc_read_file(orig_sc, file_data))
        return "Error: cannot read SC: " + orig_sc;

    // 1. Найти Shape (для UV bbox и texture_index) через уже
    //    протестированный sc2_parse.
    Sc2File sc;
    std::string sc2_err;
    if (!sc2_parse(file_data, sc, &sc2_err))
        return "Error: SC2 parse failed: " + sc2_err;

    const Sc2Shape* target_shape = nullptr;
    for (auto& s : sc.shapes) {
        if (s.id == (uint16_t)shape_id) { target_shape = &s; break; }
    }
    if (!target_shape) return "Error: shape id " + std::to_string(shape_id) + " not found";

    float min_u, min_v, max_u, max_v;
    if (!sc2_shape_uv_bbox(*target_shape, min_u, min_v, max_u, max_v))
        return "Error: shape has no vertices";

    uint32_t texture_index = 0;
    if (!target_shape->commands.empty())
        texture_index = target_shape->commands[0].texture_index;

    if (texture_index >= sc.textures.size())
        return "Error: texture index " + std::to_string(texture_index) + " out of range";

    auto& tex_meta = sc.textures[texture_index].highres;
    if (!tex_meta.present) return "Error: target texture has no highres data";

    // 2. Заново прочитать "сырой" layout — нужен полный body для patch
    //    и точные границы Textures-chunk внутри него.
    Sc2RawLayout layout;
    std::string layout_err;
    if (!sc2_load_raw_layout(file_data, layout, &layout_err))
        return "Error: layout: " + layout_err;

    if (layout.textures_payload_off + tex_meta.data_off + tex_meta.data_len >
        layout.body.size()) {
        return "Error: texture data out of range in body";
    }

    const uint8_t* ktx_ptr =
        layout.body.data() + layout.textures_payload_off + tex_meta.data_off;

    int kw = 0, kh = 0, bw = 8, bh = 8;
    uint32_t gl = 0;
    size_t image_start = 0, image_size = 0;
    std::vector<uint8_t> atlas_rgba;

    if (!sc2_decode_ktx_rgba(ktx_ptr, tex_meta.data_len, kw, kh, gl, bw, bh,
                              image_start, image_size, atlas_rgba)) {
        return "Error: KTX decode failed";
    }

    if (kw != tex_meta.width || kh != tex_meta.height) {
        // Не фатально (некоторые файлы хранят чуть другие значения),
        // но используем реальные декодированные размеры дальше.
    }

    // 3. Перевести нормализованный UV bbox в пиксели этой конкретной
    //    текстуры и патчить прямоугольник новым PNG (billinear resize),
    //    той же логикой, что и в png2sc_op.cpp для SC1.
    int x0 = std::max(0, (int)std::floor(min_u * kw + 0.001f));
    int y0 = std::max(0, (int)std::floor(min_v * kh + 0.001f));
    int x1 = std::min(kw, (int)std::ceil(max_u * kw - 0.001f));
    int y1 = std::min(kh, (int)std::ceil(max_v * kh - 0.001f));
    int tw = x1 - x0, th = y1 - y0;
    if (tw <= 0 || th <= 0) return "Error: empty target rectangle";

    std::vector<uint8_t> repl((size_t)tw * th * 4);
    float sx = (float)pw / tw, sy = (float)ph / th;
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            float fx = (x + 0.5f) * sx - 0.5f, fy = (y + 0.5f) * sy - 0.5f;
            int x0s = std::max(0, std::min(pw - 1, (int)std::floor(fx)));
            int y0s = std::max(0, std::min(ph - 1, (int)std::floor(fy)));
            int x1s = std::min(pw - 1, x0s + 1), y1s = std::min(ph - 1, y0s + 1);
            float dx = std::max(0.f, std::min(1.f, fx - x0s));
            float dy = std::max(0.f, std::min(1.f, fy - y0s));
            for (int c = 0; c < 4; c++) {
                float v = input[((size_t)y0s * pw + x0s) * 4 + c] * (1 - dx) * (1 - dy)
                        + input[((size_t)y0s * pw + x1s) * 4 + c] * dx * (1 - dy)
                        + input[((size_t)y1s * pw + x0s) * 4 + c] * (1 - dx) * dy
                        + input[((size_t)y1s * pw + x1s) * 4 + c] * dx * dy;
                repl[((size_t)y * tw + x) * 4 + c] = (uint8_t)std::lround(v);
            }
        }
    }
    /*
     * Строим полигоны Shape'а для текущей текстуры —
     * те же команды, что дали texture_index/bbox выше.
     *
     * Пиксели вне этих полигонов НЕ патчатся вообще —
     * остаются оригинальным содержимым атласа. Это и
     * устраняет риск затирания соседних спрайтов,
     * когда реальная геометрия не прямоугольная
     * (bounding box шире фактического контура).
     */
    std::vector<std::vector<PolyPoint>> shape_polygons;

    for (auto& cmd : target_shape->commands) {

        if (cmd.texture_index != texture_index)
            continue;

        if (cmd.vertices.size() < 3)
            continue;

        std::vector<PolyPoint> pts;
        pts.reserve(cmd.vertices.size());

        for (auto& v : cmd.vertices) {

            pts.push_back({
                v.u * (float)kw,
                v.v * (float)kh
            });
        }

        shape_polygons.push_back(
            sort_points_by_angle(pts)
        );
    }

    for (int y = 0; y < th; y++) {

        for (int x = 0; x < tw; x++) {

            /*
             * Пиксель патчится только если он попадает
             * хотя бы в один из полигонов Shape'а. Если
             * у Shape'а вообще нет полигонов (формат/
             * версия файла не дала вершин), fallback —
             * патчим весь прямоугольник как раньше,
             * чтобы не потерять функциональность на
             * тех файлах, где geometry недоступна.
             */
            bool inside =
                shape_polygons.empty();

            if (!inside) {

                float px =
                    (float)(x + x0) + 0.5f;

                float py =
                    (float)(y + y0) + 0.5f;

                for (auto& poly : shape_polygons) {

                    if (polygon_contains_point(
                            poly, px, py)) {

                        inside = true;
                        break;
                    }
                }
            }

            if (!inside)
                continue;

            uint8_t* dst =
                atlas_rgba.data() +
                ((size_t)(y + y0) * kw + (x + x0)) * 4;

            const uint8_t* src =
                repl.data() +
                ((size_t)y * tw + x) * 4;

            memcpy(dst, src, 4);
        }
    }

    // 4. Кодируем обратно тем же ASTC block size. Размер в байтах
    //    зависит только от (kw,kh,bw,bh), не от контента — совпадает
    //    с исходным image_size гарантированно.
    std::vector<uint8_t> encoded;
    if (!astc_encode(atlas_rgba.data(), kw, kh, bw, bh, encoded))
        return "Error: ASTC encode failed";
    if (encoded.size() != image_size)
        return "Error: encoded KTX size changed unexpectedly ("
             + std::to_string(encoded.size()) + " vs " + std::to_string(image_size) + ")";

    // 5. Патчим ровно эти байты внутри копии body, ничего больше
    //    не трогаем (Header/DataStorage/ExportNames/Shapes/MovieClips
    //    остаются byte-for-byte как в оригинале).
    std::vector<uint8_t> new_body = layout.body;
    size_t patch_at =
        layout.textures_payload_off + tex_meta.data_off + image_start;

    if (patch_at + encoded.size() > new_body.size())
        return "Error: patch offset out of range";

    memcpy(new_body.data() + patch_at, encoded.data(), encoded.size());

    // 6. Пересжимаем body и записываем: header_bytes (без изменений)
    //    + новый compressed body + external_matrix_bank (без изменений).
    std::vector<uint8_t> new_comp;
    try {
        new_comp = sc_compress(new_body, CompKind::ZSTD);
    } catch (std::exception& e) {
        return std::string("Error: compress: ") + e.what();
    }

    std::vector<uint8_t> out_file = layout.header_bytes;
    out_file.insert(out_file.end(), new_comp.begin(), new_comp.end());
    if (!layout.external_matrix_bank.empty()) {
        out_file.insert(out_file.end(),
                         layout.external_matrix_bank.begin(),
                         layout.external_matrix_bank.end());
    }

    /*
     * ВАЖНО: Header.compressed_size (поле 11 в дескрипторе) хранит
     * точный размер сжатого блока. Если он записан не как 0
     * ("до конца файла"), после пересжатия он может не совпадать
     * с новым new_comp.size() (zstd — недетерминированный по
     * итоговому размеру между разными реализациями/уровнями).
     *
     * header_bytes копируется как есть, поэтому если файл использует
     * explicit compressed_size, старое значение может разойтись
     * с реальным новым размером блока и чтение сломается.
     * Патчим это поле, если оно присутствует и ненулевое.
     */
    {
        // Ищем то же поле[11] в скопированном дескрипторе и патчим,
        // если оно было явно записано (не 0 "до конца файла").
        uint8_t version = out_file[2];
        size_t pos = 6;
        if (version == 6) pos += 2;
        uint32_t descriptor_size;
        memcpy(&descriptor_size, out_file.data() + pos, 4);
        size_t descriptor_at = pos + 4;

        uint8_t* hb = out_file.data() + descriptor_at;
        size_t hlen = descriptor_size;

        uint32_t root_rel; memcpy(&root_rel, hb, 4);
        size_t table_pos = root_rel;
        int32_t vt_off; memcpy(&vt_off, hb + table_pos, 4);
        size_t vtable = (size_t)((int64_t)table_pos - vt_off);
        uint16_t vtable_size; memcpy(&vtable_size, hb + vtable, 2);

        int index = 11; // compressed_size field
        size_t entry = 4 + (size_t)index * 2;
        if (entry + 2 <= vtable_size) {
            uint16_t rel; memcpy(&rel, hb + vtable + entry, 2);
            if (rel != 0) {
                size_t field_pos = table_pos + rel;
                if (field_pos + 4 <= hlen) {
                    uint32_t old_val;
                    memcpy(&old_val, hb + field_pos, 4);
                    if (old_val != 0) {
                        uint32_t new_val = (uint32_t)new_comp.size();
                        memcpy(hb + field_pos, &new_val, 4);
                    }
                }
            }
        }
    }

    std::string stem = sc_basename_no_ext(orig_sc);
    std::string out_path = out_dir + "/" + stem + "_patched.sc";
    if (!sc_write_file(out_path, out_file))
        return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: PNG to SC2 (v" << (int)sc.version << ")\n";
    r << "  Shape[" << shape_id << "] \"" << target_shape->export_name << "\""
      << " -> texture[" << texture_index << "]\n";
    r << "  Atlas: " << kw << "x" << kh << "  ASTC " << bw << "x" << bh << "\n";
    r << "  Replaced: " << tw << "x" << th << " at (" << x0 << "," << y0 << ")\n";
    r << "  PNG: " << pw << "x" << ph << " -> " << tw << "x" << th << "\n";
    r << "  Output: " << stem << "_patched.sc\n";
    r << "  Dir: " << out_dir;
    return r.str();
}


// ═══════════════════════════════════════════════════════════════════════════
//  BATCH: много PNG за один проход (ASTC кодируется один раз на текстуру)
// ═══════════════════════════════════════════════════════════════════════════

static std::string unique_sc_path(const std::string& dir, const std::string& base_no_ext) {
    std::string p = dir + "/" + base_no_ext + ".sc";
    struct stat st;
    int i = 2;
    while (stat(p.c_str(), &st) == 0)
        p = dir + "/" + base_no_ext + "_" + std::to_string(i++) + ".sc";
    return p;
}

static std::string path_basename(const std::string& p) {
    size_t k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}

/* Патч одного шейпа в уже декодированный атлас текстуры (билинейный ресайз + маска по полигонам). */
static bool blit_shape_polygons(std::vector<uint8_t>& atlas, int kw, int kh,
                                const std::vector<uint8_t>& input, int pw, int ph,
                                const Sc2Shape& shape, uint32_t texture_index) {
    float min_u, min_v, max_u, max_v;
    if (!sc2_shape_uv_bbox(shape, min_u, min_v, max_u, max_v)) return false;
    int x0 = std::max(0, (int)std::floor(min_u * kw + 0.001f));
    int y0 = std::max(0, (int)std::floor(min_v * kh + 0.001f));
    int x1 = std::min(kw, (int)std::ceil(max_u * kw - 0.001f));
    int y1 = std::min(kh, (int)std::ceil(max_v * kh - 0.001f));
    int tw = x1 - x0, th = y1 - y0;
    if (tw <= 0 || th <= 0) return false;

    std::vector<std::vector<PolyPoint>> polys;
    for (auto& cmd : shape.commands) {
        if (cmd.texture_index != texture_index || cmd.vertices.size() < 3) continue;
        std::vector<PolyPoint> pts;
        for (auto& v : cmd.vertices) pts.push_back({ v.u * (float)kw, v.v * (float)kh });
        polys.push_back(sort_points_by_angle(pts));
    }

    float sx = (float)pw / tw, sy = (float)ph / th;
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            bool inside = polys.empty();
            if (!inside) {
                float px = (float)(x + x0) + 0.5f, py = (float)(y + y0) + 0.5f;
                for (auto& poly : polys)
                    if (polygon_contains_point(poly, px, py)) { inside = true; break; }
            }
            if (!inside) continue;

            float fx = (x + 0.5f) * sx - 0.5f, fy = (y + 0.5f) * sy - 0.5f;
            int x0s = std::max(0, std::min(pw - 1, (int)std::floor(fx)));
            int y0s = std::max(0, std::min(ph - 1, (int)std::floor(fy)));
            int x1s = std::min(pw - 1, x0s + 1), y1s = std::min(ph - 1, y0s + 1);
            float dx = std::max(0.f, std::min(1.f, fx - x0s));
            float dy = std::max(0.f, std::min(1.f, fy - y0s));
            uint8_t* dst = atlas.data() + ((size_t)(y + y0) * kw + (x + x0)) * 4;
            for (int c = 0; c < 4; c++) {
                float v = input[((size_t)y0s * pw + x0s) * 4 + c] * (1 - dx) * (1 - dy)
                        + input[((size_t)y0s * pw + x1s) * 4 + c] * dx * (1 - dy)
                        + input[((size_t)y1s * pw + x0s) * 4 + c] * (1 - dx) * dy
                        + input[((size_t)y1s * pw + x1s) * 4 + c] * dx * dy;
                dst[c] = (uint8_t)std::lround(v);
            }
        }
    }
    return true;
}

static void patch_header_compressed_size(std::vector<uint8_t>& out_file, size_t comp_size) {
    uint8_t version = out_file[2];
    size_t pos = 6;
    if (version == 6) pos += 2;
    uint32_t descriptor_size;
    memcpy(&descriptor_size, out_file.data() + pos, 4);
    uint8_t* hb = out_file.data() + pos + 4;
    size_t hlen = descriptor_size;

    uint32_t root_rel; memcpy(&root_rel, hb, 4);
    size_t table_pos = root_rel;
    int32_t vt_off; memcpy(&vt_off, hb + table_pos, 4);
    size_t vtable = (size_t)((int64_t)table_pos - vt_off);
    uint16_t vtable_size; memcpy(&vtable_size, hb + vtable, 2);

    size_t entry = 4 + 11 * 2;   // поле compressed_size
    if (entry + 2 <= vtable_size) {
        uint16_t rel; memcpy(&rel, hb + vtable + entry, 2);
        if (rel != 0) {
            size_t field_pos = table_pos + rel;
            if (field_pos + 4 <= hlen) {
                uint32_t old_val; memcpy(&old_val, hb + field_pos, 4);
                if (old_val != 0) { uint32_t nv = (uint32_t)comp_size; memcpy(hb + field_pos, &nv, 4); }
            }
        }
    }
}

std::string sc2_png2sc_batch(const std::vector<std::pair<int, std::string>>& items,
                             const std::string& orig_sc, const std::string& out_dir) {
    std::vector<uint8_t> file_data;
    if (!sc_read_file(orig_sc, file_data)) return "Error: cannot read SC: " + orig_sc;

    Sc2File sc; std::string err;
    if (!sc2_parse(file_data, sc, &err)) return "Error: SC2 parse failed: " + err;
    Sc2RawLayout layout;
    if (!sc2_load_raw_layout(file_data, layout, &err)) return "Error: layout: " + err;

    std::unordered_map<int, const Sc2Shape*> by_id;
    for (auto& sh : sc.shapes) by_id[sh.id] = &sh;

    struct Job { const Sc2Shape* shape; int id; const std::string* png; };
    std::map<uint32_t, std::vector<Job>> by_tex;
    std::vector<std::string> failures;

    for (auto& it : items) {
        auto f = by_id.find(it.first);
        if (f == by_id.end()) { failures.push_back("shape " + std::to_string(it.first) + ": не найден"); continue; }
        const Sc2Shape* sh = f->second;
        uint32_t ti = sh->commands.empty() ? 0 : sh->commands[0].texture_index;
        if (ti >= sc.textures.size() || !sc.textures[ti].highres.present) {
            failures.push_back("shape " + std::to_string(it.first) + ": нет данных текстуры"); continue;
        }
        by_tex[ti].push_back({ sh, it.first, &it.second });
    }

    std::vector<uint8_t> new_body = layout.body;
    int applied = 0;
    std::ostringstream detail;

    for (auto& kv : by_tex) {
        uint32_t ti = kv.first;
        auto& meta = sc.textures[ti].highres;
        if (layout.textures_payload_off + meta.data_off + meta.data_len > layout.body.size()) {
            for (auto& j : kv.second) failures.push_back("shape " + std::to_string(j.id) + ": данные текстуры за границей");
            continue;
        }
        const uint8_t* ktx_ptr = layout.body.data() + layout.textures_payload_off + meta.data_off;
        int kw = 0, kh = 0, bw = 8, bh = 8; uint32_t gl = 0;
        size_t image_start = 0, image_size = 0;
        std::vector<uint8_t> atlas;
        if (!sc2_decode_ktx_rgba(ktx_ptr, meta.data_len, kw, kh, gl, bw, bh, image_start, image_size, atlas)) {
            for (auto& j : kv.second) failures.push_back("shape " + std::to_string(j.id) + ": KTX не декодируется");
            continue;
        }

        int ok_here = 0;
        for (auto& j : kv.second) {
            int pw, ph, pch;
            uint8_t* ld = stbi_load(j.png->c_str(), &pw, &ph, &pch, 4);
            if (!ld) { failures.push_back("shape " + std::to_string(j.id) + ": не открывается PNG"); continue; }
            std::vector<uint8_t> input(ld, ld + (size_t)pw * ph * 4);
            stbi_image_free(ld);
            if (blit_shape_polygons(atlas, kw, kh, input, pw, ph, *j.shape, ti)) { ok_here++; applied++; }
            else failures.push_back("shape " + std::to_string(j.id) + ": пустая область");
        }
        if (ok_here == 0) continue;

        std::vector<uint8_t> enc;
        if (!astc_encode(atlas.data(), kw, kh, bw, bh, enc) || enc.size() != image_size) {
            return "Error: ASTC encode failed (texture " + std::to_string(ti) + ")";
        }
        size_t patch_at = layout.textures_payload_off + meta.data_off + image_start;
        if (patch_at + enc.size() > new_body.size()) return "Error: patch offset out of range";
        memcpy(new_body.data() + patch_at, enc.data(), enc.size());
        detail << "  texture[" << ti << "] " << kw << "x" << kh << ": " << ok_here << " шт.\n";
    }

    if (applied == 0) {
        std::string r = "Error: ничего не заменено";
        for (auto& f : failures) r += "\n  " + f;
        return r;
    }

    std::vector<uint8_t> new_comp;
    try { new_comp = sc_compress(new_body, CompKind::ZSTD); }
    catch (std::exception& e) { return std::string("Error: compress: ") + e.what(); }

    std::vector<uint8_t> out_file = layout.header_bytes;
    out_file.insert(out_file.end(), new_comp.begin(), new_comp.end());
    if (!layout.external_matrix_bank.empty())
        out_file.insert(out_file.end(), layout.external_matrix_bank.begin(), layout.external_matrix_bank.end());
    patch_header_compressed_size(out_file, new_comp.size());

    std::string out_path = unique_sc_path(out_dir, sc_basename_no_ext(orig_sc) + "_patched");
    if (!sc_write_file(out_path, out_file)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: batch SC2 (v" << (int)sc.version << ")\n";
    r << "  Applied: " << applied << " of " << items.size() << "\n";
    r << detail.str();
    for (size_t i = 0; i < failures.size() && i < 5; i++) r << "  ! " << failures[i] << "\n";
    r << "  Output: " << path_basename(out_path) << "\n";
    r << "  Dir: " << out_dir;
    return r.str();
}


// ═══════════════════════════════════════════════════════════════════════════
//  ЭКСПОРТНЕЙМЫ (SC2): список и правка (переименование / удаление / добавление)
// ═══════════════════════════════════════════════════════════════════════════
//
// ExportNames = FlatBuffers-таблица { object_ids:[ushort], name_ref_ids:[uint] }.
// Имена лежат в DataStorage.strings и адресуются по индексу.
//
// Смещения FlatBuffers только «вперёд», поэтому вектор строк нельзя расширить
// на месте: в конец DataStorage дописывается НОВЫЙ вектор строк (старые + новые)
// со свежими копиями строк, а поле strings таблицы DataStorage переключается на
// него. Индексы строк не меняются — остальные чанки трогать не нужно.
// Старый вектор остаётся в буфере мёртвым грузом.
// Сдвиг DataStorage учитывается в Header.resources_offset и compressed_size.

static inline uint16_t e_rd16(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v, b + o, 2); return v; }
static inline uint32_t e_rd32(const uint8_t* b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static inline void e_wr32(std::vector<uint8_t>& b, size_t o, uint32_t v) { memcpy(b.data() + o, &v, 4); }
static inline void e_put32(std::vector<uint8_t>& b, uint32_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
static inline void e_put16(std::vector<uint8_t>& b, uint16_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 2); }
static inline void e_align4(std::vector<uint8_t>& b) { while (b.size() % 4) b.push_back(0); }

/* Позиция (абс. в buf) поля table-слота `slot`, 0 если поля нет. */
static size_t e_field_pos(const uint8_t* buf, size_t len, size_t table_pos, int slot) {
    if (table_pos + 4 > len) return 0;
    int32_t vt_off; memcpy(&vt_off, buf + table_pos, 4);
    int64_t vt = (int64_t)table_pos - vt_off;
    if (vt < 0 || (size_t)vt + 4 > len) return 0;
    uint16_t vt_size = e_rd16(buf, (size_t)vt);
    size_t entry = 4 + (size_t)slot * 2;
    if (entry + 2 > vt_size) return 0;
    uint16_t rel = e_rd16(buf, (size_t)vt + entry);
    if (rel == 0) return 0;
    size_t fp = table_pos + rel;
    return fp + 4 <= len ? fp : 0;
}

/* Вектор в поле slot: elements_start (абс.), count. */
static bool e_vector(const uint8_t* buf, size_t len, size_t table_pos, int slot,
                     size_t& start, uint32_t& count, size_t* field_pos_out = nullptr) {
    size_t fp = e_field_pos(buf, len, table_pos, slot);
    if (!fp) return false;
    size_t vec = fp + e_rd32(buf, fp);
    if (vec + 4 > len) return false;
    count = e_rd32(buf, vec);
    start = vec + 4;
    if (field_pos_out) *field_pos_out = fp;
    return true;
}

/* Позиция поля u32 Header-дескриптора внутри всего файла (0 если поля нет). */
static size_t e_header_u32_pos(const std::vector<uint8_t>& file, int slot) {
    uint8_t version = file[2];
    size_t pos = 6; if (version == 6) pos += 2;
    uint32_t dsz; memcpy(&dsz, file.data() + pos, 4);
    size_t at = pos + 4;
    uint32_t root_rel; memcpy(&root_rel, file.data() + at, 4);
    size_t rel = e_field_pos(file.data() + at, dsz, root_rel, slot);
    return rel ? at + rel : 0;
}

struct E2Entry { uint16_t id; uint32_t ref; bool del; };

struct E2State {
    Sc2RawLayout layout;
    uint32_t resources_offset = 0;
    size_t ds_size = 0;
    std::vector<uint8_t> ds;                 // DataStorage буфер
    size_t strings_field_pos = 0;            // позиция поля strings в таблице DataStorage
    std::vector<size_t> str_pos;             // абс. позиции строк в ds
    std::vector<std::string> strings;
    size_t exp_chunk_off = 0;                // позиция [u32 size] ExportNames чанка в body
    size_t exp_chunk_len = 0;                // payload
    std::vector<E2Entry> entries;
};

static bool e2_load(const std::vector<uint8_t>& file, E2State& st, std::string& err) {
    if (!sc2_load_raw_layout(file, st.layout, &err)) return false;
    size_t hp = e_header_u32_pos(file, 8);
    if (!hp) { err = "Header: нет resources_offset"; return false; }
    st.resources_offset = e_rd32(file.data(), hp);

    const auto& body = st.layout.body;
    if (body.size() < 4) { err = "body слишком короткий"; return false; }
    st.ds_size = e_rd32(body.data(), 0);
    if (4 + st.ds_size > body.size()) { err = "DataStorage за границей"; return false; }
    st.ds.assign(body.begin() + 4, body.begin() + 4 + st.ds_size);

    // strings
    {
        if (st.ds.size() < 8) { err = "DataStorage пуст"; return false; }
        size_t table = e_rd32(st.ds.data(), 0);
        size_t start; uint32_t cnt;
        if (!e_vector(st.ds.data(), st.ds.size(), table, 0, start, cnt, &st.strings_field_pos)) {
            err = "DataStorage: нет strings"; return false;
        }
        for (uint32_t i = 0; i < cnt; i++) {
            size_t ep = start + (size_t)i * 4;
            if (ep + 4 > st.ds.size()) { err = "strings за границей"; return false; }
            size_t sp = ep + e_rd32(st.ds.data(), ep);
            std::string t;
            if (sp + 4 <= st.ds.size()) {
                uint32_t l = e_rd32(st.ds.data(), sp);
                if (sp + 4 + l <= st.ds.size()) t.assign((const char*)st.ds.data() + sp + 4, l);
            }
            st.str_pos.push_back(sp);
            st.strings.push_back(t);
        }
    }

    // ExportNames chunk
    size_t bpos = st.resources_offset;
    if (bpos < 4 + st.ds_size || bpos + 4 > body.size()) { err = "resources_offset вне диапазона"; return false; }
    st.exp_chunk_off = bpos;
    st.exp_chunk_len = e_rd32(body.data(), bpos);
    if (bpos + 4 + st.exp_chunk_len > body.size()) { err = "ExportNames за границей"; return false; }
    const uint8_t* pl = body.data() + bpos + 4;
    size_t plen = st.exp_chunk_len;
    if (plen >= 4) {
        size_t table = e_rd32(pl, 0);
        size_t os, ns; uint32_t oc, nc;
        if (e_vector(pl, plen, table, 0, os, oc) && e_vector(pl, plen, table, 1, ns, nc) && oc == nc) {
            for (uint32_t i = 0; i < oc; i++) {
                if (os + (size_t)i * 2 + 2 > plen || ns + (size_t)i * 4 + 4 > plen) break;
                st.entries.push_back({ e_rd16(pl, os + (size_t)i * 2), e_rd32(pl, ns + (size_t)i * 4), false });
            }
        }
    }
    return true;
}

std::string sc2_exports_list(const std::string& path) {
    std::vector<uint8_t> file;
    if (!sc_read_file(path, file)) return "Error: cannot read SC: " + path;
    E2State st; std::string err;
    if (!e2_load(file, st, err)) return "Error: " + err;
    std::ostringstream r;
    for (auto& e : st.entries) {
        std::string nm = e.ref < st.strings.size() ? st.strings[e.ref] : std::string();
        r << e.id << '\t' << nm << '\n';
    }
    return r.str();
}

/* Список MovieClip'ов SC2: строки "id\tэкспортнейм\tкадры". */
std::string sc2_movieclips_list(const std::string& path) {
    std::vector<uint8_t> file;
    if (!sc_read_file(path, file)) return "Error: cannot read SC: " + path;
    Sc2File sc; std::string err;
    if (!sc2_parse(file, sc, &err)) return "Error: " + err;
    std::map<uint16_t, std::string> names;
    for (auto& e : sc.exports) names[e.object_id] = e.name;
    std::ostringstream r;
    for (auto& m : sc.movieclips) {
        auto it = names.find(m.id);
        r << m.id << '\t' << (it != names.end() ? it->second : std::string()) << '\t' << m.frame_count << '\n';
    }
    return r.str();
}

/* ops: (type 'R'|'D'|'A', index, object_id, name) */
std::string sc2_exports_edit(const std::vector<std::tuple<char, int, int, std::string>>& ops,
                             const std::string& orig_sc, const std::string& out_dir) {
    std::vector<uint8_t> file;
    if (!sc_read_file(orig_sc, file)) return "Error: cannot read SC: " + orig_sc;
    E2State st; std::string err;
    if (!e2_load(file, st, err)) return "Error: " + err;

    // допустимые id MovieClip'ов (для добавления)
    std::set<uint16_t> mc_ids;
    {
        Sc2File sc; std::string e2;
        if (sc2_parse(file, sc, &e2)) for (auto& m : sc.movieclips) mc_ids.insert(m.id);
    }

    std::vector<std::string> new_strings;     // индексы: strings.size() + k
    std::vector<E2Entry> entries = st.entries;
    int renamed = 0, deleted = 0, added = 0;
    std::vector<std::string> notes;

    auto name_of = [&](const E2Entry& e) {
        return e.ref < st.strings.size() ? st.strings[e.ref]
                                         : new_strings[e.ref - st.strings.size()];
    };

    for (auto& op : ops) {
        char t = std::get<0>(op);
        int idx = std::get<1>(op);
        int oid = std::get<2>(op);
        const std::string& nm = std::get<3>(op);
        if (t == 'R') {
            if (idx < 0 || idx >= (int)st.entries.size()) return "Error: неверный индекс экспортнейма " + std::to_string(idx);
            if (nm.empty()) return "Error: пустое имя";
            if (entries[idx].del) continue;
            if (name_of(entries[idx]) == nm) continue;
            entries[idx].ref = (uint32_t)(st.strings.size() + new_strings.size());
            new_strings.push_back(nm);
            renamed++;
        } else if (t == 'D') {
            if (idx < 0 || idx >= (int)st.entries.size()) return "Error: неверный индекс экспортнейма " + std::to_string(idx);
            if (!entries[idx].del) { entries[idx].del = true; deleted++; }
        } else if (t == 'A') {
            if (nm.empty()) return "Error: пустое имя";
            if (oid < 0 || oid > 65535) return "Error: неверный ID объекта " + std::to_string(oid);
            if (!mc_ids.empty() && !mc_ids.count((uint16_t)oid))
                return "Error: MovieClip с ID " + std::to_string(oid) + " не найден";
            E2Entry ne{ (uint16_t)oid, (uint32_t)(st.strings.size() + new_strings.size()), false };
            new_strings.push_back(nm);
            entries.push_back(ne);
            added++;
        }
    }
    if (renamed + deleted + added == 0) return "Error: нет изменений";

    // уникальность имён
    {
        std::set<std::string> seen;
        for (auto& e : entries) {
            if (e.del) continue;
            std::string n = name_of(e);
            if (!seen.insert(n).second) return "Error: имя \"" + n + "\" уже используется";
        }
    }

    // ── новый DataStorage ───────────────────────────────────────────────
    std::vector<uint8_t> nds = st.ds;
    size_t n_old = st.strings.size();
    size_t total = n_old + new_strings.size();
    e_align4(nds);
    size_t vec_pos = nds.size();
    e_put32(nds, (uint32_t)total);
    nds.resize(nds.size() + total * 4, 0);

    std::vector<size_t> new_pos(total);
    for (size_t i = 0; i < total; i++) {
        const std::string& str = i < n_old ? st.strings[i] : new_strings[i - n_old];
        e_align4(nds);
        new_pos[i] = nds.size();
        e_put32(nds, (uint32_t)str.size());
        nds.insert(nds.end(), str.begin(), str.end());
        nds.push_back(0);
    }
    e_align4(nds);
    for (size_t i = 0; i < total; i++) {
        size_t elem = vec_pos + 4 + i * 4;
        e_wr32(nds, elem, (uint32_t)(new_pos[i] - elem));
    }
    e_wr32(nds, st.strings_field_pos, (uint32_t)(vec_pos - st.strings_field_pos));

    // ── новый ExportNames ───────────────────────────────────────────────
    std::vector<E2Entry> fin;
    for (auto& e : entries) if (!e.del) fin.push_back(e);

    std::vector<uint8_t> nx;
    e_put32(nx, 12);                    // root -> table @12
    e_put16(nx, 8);                     // vtable size
    e_put16(nx, 12);                    // table size
    e_put16(nx, 4);                     // object_ids @+4
    e_put16(nx, 8);                     // name_ref_ids @+8
    e_put32(nx, 8);                     // table: soffset = 12 - 4
    e_put32(nx, 0);                     // placeholder object_ids
    e_put32(nx, 0);                     // placeholder name_ref_ids
    size_t ids_vec = nx.size();
    e_put32(nx, (uint32_t)fin.size());
    for (auto& e : fin) e_put16(nx, e.id);
    e_align4(nx);
    size_t refs_vec = nx.size();
    e_put32(nx, (uint32_t)fin.size());
    for (auto& e : fin) e_put32(nx, e.ref);
    e_wr32(nx, 16, (uint32_t)(ids_vec - 16));
    e_wr32(nx, 20, (uint32_t)(refs_vec - 20));

    // ── новый body ──────────────────────────────────────────────────────
    const auto& ob = st.layout.body;
    std::vector<uint8_t> nb;
    e_put32(nb, (uint32_t)nds.size());
    nb.insert(nb.end(), nds.begin(), nds.end());
    nb.insert(nb.end(), ob.begin() + 4 + st.ds_size, ob.begin() + st.exp_chunk_off);
    e_put32(nb, (uint32_t)nx.size());
    nb.insert(nb.end(), nx.begin(), nx.end());
    nb.insert(nb.end(), ob.begin() + st.exp_chunk_off + 4 + st.exp_chunk_len, ob.end());

    int64_t delta = (int64_t)nds.size() - (int64_t)st.ds_size;
    uint32_t new_res_off = (uint32_t)((int64_t)st.resources_offset + delta);

    std::vector<uint8_t> comp;
    try { comp = sc_compress(nb, CompKind::ZSTD); }
    catch (std::exception& e) { return std::string("Error: compress: ") + e.what(); }

    std::vector<uint8_t> out = st.layout.header_bytes;
    out.insert(out.end(), comp.begin(), comp.end());
    if (!st.layout.external_matrix_bank.empty())
        out.insert(out.end(), st.layout.external_matrix_bank.begin(), st.layout.external_matrix_bank.end());

    size_t rp = e_header_u32_pos(out, 8);
    if (!rp) return "Error: Header: нет resources_offset";
    memcpy(out.data() + rp, &new_res_off, 4);
    patch_header_compressed_size(out, comp.size());

    std::string out_path = unique_sc_path(out_dir, sc_basename_no_ext(orig_sc) + "_exp");
    if (!sc_write_file(out_path, out)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: export names (SC2 v" << (int)file[2] << ")\n";
    if (renamed) r << "  Renamed: " << renamed << "\n";
    if (deleted) r << "  Deleted: " << deleted << "\n";
    if (added)   r << "  Added: " << added << "\n";
    r << "  Total: " << fin.size() << "\n";
    r << "  Output: " << path_basename(out_path) << "\n";
    r << "  Dir: " << out_dir;
    return r.str();
}

// ===================== Текстура SC2 → RGBA (для sc2gif) =====================
// KTX (ASTC) декодируется как обычно; несжатая текстура (формат из gif2sc)
// хранится как RGBA8 и копируется как есть.
bool sc2_decode_texture_rgba(const Sc2File& sc, size_t tex_index, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (tex_index >= sc.textures.size()) return false;
    const Sc2TextureData& t = sc.textures[tex_index].highres.present ? sc.textures[tex_index].highres
                                                                     : sc.textures[tex_index].lowres;
    if (!t.present || t.data_len == 0 || (size_t)t.data_off + t.data_len > sc.textures_chunk.size()) return false;
    const uint8_t* d = sc.textures_chunk.data() + t.data_off;
    if (t.data_len >= 12 && memcmp(d, SC2_KTX_MAGIC, 12) == 0) {
        uint32_t gl; int bw, bh; size_t is, isz;
        return sc2_decode_ktx_rgba(d, t.data_len, w, h, gl, bw, bh, is, isz, rgba);
    }
    w = t.width; h = t.height;
    if (w <= 0 || h <= 0 || (size_t)w * h * 4 > t.data_len) return false;
    rgba.assign(d, d + (size_t)w * h * 4);
    return true;
}
