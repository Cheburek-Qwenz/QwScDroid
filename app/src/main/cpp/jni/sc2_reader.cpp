#include "sc2_reader.h"
#include <cstring>
#include <functional>
#include <unordered_map>

// ── Minimal FlatBuffers wire-format reader ──────────────────────────────────
// No codegen, no google/flatbuffers dependency: we know the exact field order
// of every table from sc2_schemas/*.fbs, so we walk vtables by hand. All
// buffers here are little-endian (matches ARM/x86 Android + the file format).
namespace fbs {

static inline uint16_t rd_u16(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v, b + o, 2); return v; }
static inline uint32_t rd_u32(const uint8_t* b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static inline int32_t  rd_i32(const uint8_t* b, size_t o) { int32_t  v; memcpy(&v, b + o, 4); return v; }
static inline float    rd_f32(const uint8_t* b, size_t o) { float    v; memcpy(&v, b + o, 4); return v; }

// A view into a FlatBuffers table, backed by a caller-owned byte buffer.
struct Table {
    const uint8_t* buf = nullptr;
    size_t buf_len = 0;
    size_t pos = 0;          // table start offset within buf
    size_t vtable = 0;
    uint16_t vtable_size = 0;
    bool ok = false;

    Table() = default;
    Table(const uint8_t* b, size_t len, size_t p) : buf(b), buf_len(len), pos(p) {
        if (!buf || pos + 4 > buf_len) return;
        int32_t vt_off = rd_i32(buf, pos);
        // vtable is at pos - vt_off; guard against garbage offsets
        if (vt_off == 0 || (size_t)llabs((long long)vt_off) > buf_len) return;
        size_t vt = (size_t)((int64_t)pos - vt_off);
        if (vt + 4 > buf_len) return;
        vtable = vt;
        vtable_size = rd_u16(buf, vtable);
        if (vtable + vtable_size > buf_len) return;
        ok = true;
    }

    uint16_t field_vt_offset(int index) const {
        size_t entry = 4 + (size_t)index * 2;
        if (!ok || entry + 2 > vtable_size) return 0;
        return rd_u16(buf, vtable + entry);
    }
    bool has(int index) const { return field_vt_offset(index) != 0; }

    uint8_t  u8 (int index, uint8_t  def = 0) const { auto o = field_vt_offset(index); return o ? buf[pos + o] : def; }
    uint16_t u16(int index, uint16_t def = 0) const { auto o = field_vt_offset(index); return o ? rd_u16(buf, pos + o) : def; }
    uint32_t u32(int index, uint32_t def = 0) const { auto o = field_vt_offset(index); return o ? rd_u32(buf, pos + o) : def; }

    // absolute offset (within buf) that an offset-typed field (string/vector/table) points to
    bool offset_ptr(int index, size_t& out) const {
        auto o = field_vt_offset(index);
        if (!o) return false;
        size_t field_pos = pos + o;
        if (field_pos + 4 > buf_len) return false;
        uint32_t rel = rd_u32(buf, field_pos);
        size_t target = field_pos + rel;
        if (target > buf_len) return false;
        out = target;
        return true;
    }

    Table sub_table(int index) const {
        size_t p;
        if (!offset_ptr(index, p)) return Table();
        return Table(buf, buf_len, p);
    }

    bool string_field(int index, std::string& out) const {
        size_t p;
        if (!offset_ptr(index, p)) return false;
        if (p + 4 > buf_len) return false;
        uint32_t len = rd_u32(buf, p);
        if (p + 4 + len > buf_len) return false;
        out.assign((const char*)(buf + p + 4), len);
        return true;
    }

    // vector: returns (elements_start_abs_offset, count); elements_start is
    // where inline scalars/structs live, or where uoffset_t[] lives for
    // vectors of strings/tables.
    bool vector_raw(int index, size_t& elements_start, uint32_t& count) const {
        size_t p;
        if (!offset_ptr(index, p)) { elements_start = 0; count = 0; return false; }
        if (p + 4 > buf_len) { elements_start = 0; count = 0; return false; }
        count = rd_u32(buf, p);
        elements_start = p + 4;
        return true;
    }

    // vector of tables/strings: resolve each element's uoffset relative to its own slot
    std::vector<size_t> vector_of_offsets(int index) const {
        std::vector<size_t> out;
        size_t start; uint32_t cnt;
        if (!vector_raw(index, start, cnt)) return out;
        out.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            size_t ep = start + (size_t)i * 4;
            if (ep + 4 > buf_len) break;
            uint32_t rel = rd_u32(buf, ep);
            out.push_back(ep + rel);
        }
        return out;
    }
};

static Table root(const uint8_t* buf, size_t len) {
    if (!buf || len < 4) return Table();
    uint32_t off = rd_u32(buf, 0);
    return Table(buf, len, off);
}

} // namespace fbs

using fbs::Table;

// ── SC2 container / body parsing ────────────────────────────────────────────

static bool read_chunk(const std::vector<uint8_t>& body, size_t& bpos, const uint8_t*& payload, size_t& payload_len) {
    if (bpos + 4 > body.size()) return false;
    uint32_t size = fbs::rd_u32(body.data(), bpos);
    if (bpos + 4 + size > body.size()) return false;
    payload = body.data() + bpos + 4;
    payload_len = size;
    bpos += 4 + size;
    return true;
}

bool sc2_parse(const std::vector<uint8_t>& file_data, Sc2File& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };

    if (file_data.size() < 10 || file_data[0] != 'S' || file_data[1] != 'C')
        return fail("not an SC file");

    uint8_t version = file_data[2];
    if (version < 5) return fail("not SC2 (version < 5)");
    if (version > 6) return fail("unsupported SC2 version (>6)");

    size_t pos = 6;
    if (version == 6) pos += 2; // unknown u16, always 0

    if (pos + 4 > file_data.size()) return fail("truncated: no descriptor size");
    uint32_t descriptor_size = fbs::rd_u32(file_data.data(), pos);
    pos += 4;
    if (pos + descriptor_size > file_data.size()) return fail("truncated: descriptor");

    Table hdr = fbs::root(file_data.data() + pos, descriptor_size);
    if (!hdr.ok) return fail("invalid Header flatbuffer");
    pos += descriptor_size;

    out.version = version;
    out.translation_precision = hdr.u32(0, 0);
    out.scale_precision       = hdr.u32(1, 0);
    out.shape_count      = hdr.u32(2, 0);
    out.movieclip_count  = hdr.u32(3, 0);
    out.texture_count    = hdr.u32(4, 0);
    out.textfield_count  = hdr.u32(5, 0);
    uint32_t resources_offset          = hdr.u32(8, 0);
    uint32_t compressed_size           = hdr.u32(11, 0);
    uint32_t external_matrix_bank_size = hdr.u32(12, 0);

    size_t comp_len = compressed_size ? compressed_size
                                       : (file_data.size() - pos - external_matrix_bank_size);
    if (pos + comp_len > file_data.size()) return fail("truncated: compressed body");

    std::vector<uint8_t> comp(file_data.begin() + pos, file_data.begin() + pos + comp_len);
    CompInfo ci{0, CompKind::ZSTD};
    // Body may legitimately be uncompressed (rare); sniff zstd magic to decide.
    static const uint8_t ZSTD_MAGIC[4] = {0x28, 0xb5, 0x2f, 0xfd};
    bool is_zstd = comp.size() >= 4 && memcmp(comp.data(), ZSTD_MAGIC, 4) == 0;
    std::vector<uint8_t> body;
    try {
        body = is_zstd ? sc_decompress(comp, ci) : comp;
    } catch (...) {
        return fail("zstd decompress failed");
    }

    // -- DataStorage (fixed at body offset 0) --
    if (body.size() < 4) return fail("truncated body: no DataStorage size");
    size_t bpos = 0;
    uint32_t ds_size = fbs::rd_u32(body.data(), bpos);
    bpos += 4;
    if (bpos + ds_size > body.size()) return fail("truncated: DataStorage");

    out.data_storage.assign(body.begin() + bpos, body.begin() + bpos + ds_size);
    Table ds = fbs::root(out.data_storage.data(), out.data_storage.size());
    if (!ds.ok) return fail("invalid DataStorage flatbuffer");

    // strings: field 0, vector<string offset>
    {
        auto str_offsets = ds.vector_of_offsets(0);
        out.strings.reserve(str_offsets.size());
        for (size_t off : str_offsets) {
            std::string s;
            if (off + 4 <= out.data_storage.size()) {
                uint32_t len = fbs::rd_u32(out.data_storage.data(), off);
                if (off + 4 + len <= out.data_storage.size())
                    s.assign((const char*)out.data_storage.data() + off + 4, len);
            }
            out.strings.push_back(std::move(s));
        }
    }
    // shapes_bitmap_poins: field 5, vector<ubyte> — 12 bytes per vertex (f32 x, f32 y, u16 u, u16 v)
    {
        size_t start; uint32_t cnt;
        if (ds.vector_raw(5, start, cnt)) {
            out.vertex_buf_off = (uint32_t)start;
            out.vertex_buf_len = cnt;
        }
    }

    // frame_elements: field 4, vector<ushort> (по 3 на элемент)
    {
        size_t start; uint32_t cnt;
        if (ds.vector_raw(4, start, cnt) && start + (size_t)cnt * 2 <= out.data_storage.size()) {
            out.frame_elements.reserve(cnt);
            for (uint32_t i = 0; i < cnt; i++)
                out.frame_elements.push_back(fbs::rd_u16(out.data_storage.data(), start + (size_t)i * 2));
        }
    }
    // банк матриц: field 6 -> bank[0].matrices (поле 0), структуры по 24 байта (6 float)
    {
        auto banks = ds.vector_of_offsets(6);
        if (!banks.empty()) {
            Table bank(out.data_storage.data(), out.data_storage.size(), banks[0]);
            size_t start; uint32_t cnt;
            if (bank.ok && bank.vector_raw(0, start, cnt) && start + (size_t)cnt * 24 <= out.data_storage.size()) {
                out.bank_matrices.reserve((size_t)cnt * 6);
                for (uint32_t i = 0; i < cnt * 6; i++)
                    out.bank_matrices.push_back(fbs::rd_f32(out.data_storage.data(), start + (size_t)i * 4));
            }
            size_t cstart; uint32_t ccnt;
            if (bank.ok && bank.vector_raw(1, cstart, ccnt) && cstart + (size_t)ccnt * 7 <= out.data_storage.size())
                out.bank_colors.assign(out.data_storage.begin() + cstart, out.data_storage.begin() + cstart + (size_t)ccnt * 7);
        }
    }

    bpos += ds_size;
    if (resources_offset < bpos || resources_offset > body.size())
        return fail("resources_offset out of range");
    bpos = resources_offset;

    // -- Fixed chunk order: ExportNames, TextFields, Shapes, MovieClips, MovieClipModifiers, Textures --
    const uint8_t* payload; size_t plen;

    // ExportNames
    if (!read_chunk(body, bpos, payload, plen)) return fail("truncated: ExportNames chunk");
    {
        Table exp = fbs::root(payload, plen);
        if (exp.ok) {
            size_t ostart, nstart; uint32_t ocnt, ncnt;
            bool hasO = exp.vector_raw(0, ostart, ocnt);
            bool hasN = exp.vector_raw(1, nstart, ncnt);
            if (hasO && hasN && ocnt == ncnt) {
                out.exports.reserve(ocnt);
                for (uint32_t i = 0; i < ocnt; i++) {
                    if (ostart + (size_t)i * 2 + 2 > plen) break;
                    if (nstart + (size_t)i * 4 + 4 > plen) break;
                    uint16_t oid  = fbs::rd_u16(payload, ostart + (size_t)i * 2);
                    uint32_t nref = fbs::rd_u32(payload, nstart + (size_t)i * 4);
                    Sc2Export e;
                    e.object_id = oid;
                    e.name = (nref < out.strings.size()) ? out.strings[nref] : std::string();
                    out.exports.push_back(std::move(e));
                }
            }
        }
    }

    // TextFields — not parsed, just skip
    if (!read_chunk(body, bpos, payload, plen)) return fail("truncated: TextFields chunk");

    // Shapes
    if (!read_chunk(body, bpos, payload, plen)) return fail("truncated: Shapes chunk");
    {
        Table shp = fbs::root(payload, plen);
        if (shp.ok) {
            auto shape_offsets = shp.vector_of_offsets(0);
            out.shapes.reserve(shape_offsets.size());
            for (size_t soff : shape_offsets) {
                Table st(payload, plen, soff);
                if (!st.ok) continue;
                Sc2Shape shape;
                shape.id = st.u16(0, 0);

                // commands: field 1, vector<struct ShapeDrawBitmapCommand> — 16 bytes each
                // struct layout: u32 unk1, u32 texture_index, u32 points_count, u32 points_offset
                size_t cstart; uint32_t ccnt;
                if (st.vector_raw(1, cstart, ccnt)) {
                    shape.commands.reserve(ccnt);
                    for (uint32_t c = 0; c < ccnt; c++) {
                        size_t co = cstart + (size_t)c * 16;
                        if (co + 16 > plen) break;
                        uint32_t tex_idx = fbs::rd_u32(payload, co + 4);
                        uint32_t pcount  = fbs::rd_u32(payload, co + 8);
                        uint32_t poffset = fbs::rd_u32(payload, co + 12);

                        Sc2Command cmd;
                        cmd.texture_index = tex_idx;
                        cmd.vertices.reserve(pcount);
                        for (uint32_t v = 0; v < pcount; v++) {
                            size_t vo = out.vertex_buf_off + (size_t)(poffset + v) * 12;
                            if (vo + 12 > out.data_storage.size()) break;
                            Sc2Vertex vert;
                            vert.x = fbs::rd_f32(out.data_storage.data(), vo);
                            vert.y = fbs::rd_f32(out.data_storage.data(), vo + 4);
                            vert.u = fbs::rd_u16(out.data_storage.data(), vo + 8)  / 65535.0f;
                            vert.v = fbs::rd_u16(out.data_storage.data(), vo + 10) / 65535.0f;
                            cmd.vertices.push_back(vert);
                        }
                        shape.commands.push_back(std::move(cmd));
                    }
                }
                out.shapes.push_back(std::move(shape));
            }
        }
    }

    // MovieClips
    if (!read_chunk(body, bpos, payload, plen)) return fail("truncated: MovieClips chunk");
    {
        Table mcs = fbs::root(payload, plen);
        if (mcs.ok) {
            auto mc_offsets = mcs.vector_of_offsets(0);
            out.movieclips.reserve(mc_offsets.size());
            for (size_t moff : mc_offsets) {
                Table mt(payload, plen, moff);
                if (!mt.ok) continue;
                Sc2MovieClip mc;
                mc.id = mt.u16(0, 0);
                if (mt.has(1)) { mc.has_export_name_ref = true; mc.export_name_ref_id = mt.u32(1, 0); }

                // children_ids: field 5, vector<ushort>
                size_t cstart; uint32_t ccnt;
                if (mt.vector_raw(5, cstart, ccnt)) {
                    mc.children_ids.reserve(ccnt);
                    for (uint32_t c = 0; c < ccnt; c++) {
                        size_t co = cstart + (size_t)c * 2;
                        if (co + 2 > plen) break;
                        mc.children_ids.push_back(fbs::rd_u16(payload, co));
                    }
                }
                {
                    size_t bstart; uint32_t bcnt;
                    if (mt.vector_raw(7, bstart, bcnt) && bstart + bcnt <= plen)
                        mc.children_blending.assign(payload + bstart, payload + bstart + bcnt);
                }
                mc.framerate = mt.u8(2, 0);
                mc.frame_count = mt.u16(3, 0);
                {
                    size_t fstart; uint32_t fcnt;
                    if (mt.vector_raw(8, fstart, fcnt) && fstart + (size_t)fcnt * 8 <= plen) {
                        mc.frame_elem_counts.reserve(fcnt);
                        for (uint32_t f = 0; f < fcnt; f++)
                            mc.frame_elem_counts.push_back(fbs::rd_u32(payload, fstart + (size_t)f * 8));
                    }
                }
                out.movieclips.push_back(std::move(mc));
            }
        }
    }
    // MovieClipModifiers: вектор структур по 4 байта {u16 id, u8 type, pad}
    if (!read_chunk(body, bpos, payload, plen)) return fail("truncated: MovieClipModifiers chunk");
    {
        Table mods = fbs::root(payload, plen);
        size_t mstart; uint32_t mcnt;
        if (mods.ok && mods.vector_raw(0, mstart, mcnt) && mstart + (size_t)mcnt * 4 <= plen) {
            for (uint32_t i = 0; i < mcnt; i++) {
                out.modifier_ids.push_back(fbs::rd_u16(payload, mstart + (size_t)i * 4));
                out.modifier_types.push_back(payload[mstart + (size_t)i * 4 + 2]);
            }
        }
    }

    // Textures
    if (!read_chunk(body, bpos, payload, plen)) return fail("truncated: Textures chunk");
    {
        out.textures_chunk.assign(payload, payload + plen);
        Table tex = fbs::root(out.textures_chunk.data(), out.textures_chunk.size());
        if (tex.ok) {
            auto set_offsets = tex.vector_of_offsets(0);
            out.textures.reserve(set_offsets.size());

            auto parse_tex_data = [&](Table t) -> Sc2TextureData {
                Sc2TextureData d;
                if (!t.ok) return d;
                d.present    = true;
                d.format     = t.u8(0, 0);
                d.pixel_type = t.u8(1, 0);
                d.width      = t.u16(2, 0);
                d.height     = t.u16(3, 0);
                size_t dstart; uint32_t dcnt;
                if (t.vector_raw(4, dstart, dcnt)) { d.data_off = (uint32_t)dstart; d.data_len = dcnt; }
                std::string ext;
                if (t.string_field(5, ext)) d.external_path = ext;
                return d;
            };

            for (size_t so : set_offsets) {
                Table ts(out.textures_chunk.data(), out.textures_chunk.size(), so);
                if (!ts.ok) continue;
                Sc2TextureSet set;
                set.lowres  = parse_tex_data(ts.sub_table(0));
                set.highres = parse_tex_data(ts.sub_table(1));
                out.textures.push_back(std::move(set));
            }
        }
    }

    // -- Resolve export names onto shapes: export(object_id) is usually a MovieClip id;
    //    walk children_ids (recursively through nested MovieClips) to find the Shape id(s)
    //    it ultimately displays, and tag those shapes with the export name. --
    {
        std::unordered_map<uint16_t, size_t> shape_by_id;
        std::unordered_map<uint16_t, size_t> mc_by_id;
        for (size_t i = 0; i < out.shapes.size(); i++) shape_by_id[out.shapes[i].id] = i;
        for (size_t i = 0; i < out.movieclips.size(); i++) mc_by_id[out.movieclips[i].id] = i;

        std::function<void(uint16_t, const std::string&, std::vector<bool>&)> visit =
            [&](uint16_t id, const std::string& name, std::vector<bool>& visited_mc) {
                auto sit = shape_by_id.find(id);
                if (sit != shape_by_id.end()) {
                    Sc2Shape& sh = out.shapes[sit->second];
                    if (sh.export_name.empty()) sh.export_name = name;
                    return;
                }
                auto mit = mc_by_id.find(id);
                if (mit == mc_by_id.end()) return;
                if (visited_mc[mit->second]) return; // cycle guard
                visited_mc[mit->second] = true;
                for (uint16_t child : out.movieclips[mit->second].children_ids)
                    visit(child, name, visited_mc);
            };

        for (auto& e : out.exports) {
            std::vector<bool> visited_mc(out.movieclips.size(), false);
            visit(e.object_id, e.name, visited_mc);
        }
    }

    out.valid = true;
    return true;
}

bool sc2_shape_uv_bbox(const Sc2Shape& shape, float& min_u, float& min_v, float& max_u, float& max_v) {
    bool any = false;
    min_u = min_v = 1e18f;
    max_u = max_v = -1e18f;
    for (auto& cmd : shape.commands) {
        for (auto& v : cmd.vertices) {
            any = true;
            if (v.u < min_u) min_u = v.u;
            if (v.u > max_u) max_u = v.u;
            if (v.v < min_v) min_v = v.v;
            if (v.v > max_v) max_v = v.v;
        }
    }
    return any;
}
