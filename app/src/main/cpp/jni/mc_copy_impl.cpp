/**
 * mc_copy_impl.cpp
 * Копирование MovieClip по export name из одного SC в другой.
 * Порт sc_mc_copy.py
 */
#include "sc_core.h"
#include <sstream>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <android/log.h>

#define TAG "MCCopy"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint16_t r16(const std::vector<uint8_t>& d, size_t& p) {
    if (p+2 > d.size()) return 0;
    uint16_t v; memcpy(&v, d.data()+p, 2); p+=2; return v;
}
static uint32_t r32(const std::vector<uint8_t>& d, size_t& p) {
    if (p+4 > d.size()) return 0;
    uint32_t v; memcpy(&v, d.data()+p, 4); p+=4; return v;
}
static uint8_t r8(const std::vector<uint8_t>& d, size_t& p) {
    return (p < d.size()) ? d[p++] : 0;
}
static std::string read_str(const std::vector<uint8_t>& d, size_t& p) {
    uint8_t len = r8(d, p);
    uint16_t slen = len;
    if (len == 0xFF) slen = r16(d, p);
    if (p+slen > d.size()) return "";
    std::string s((char*)d.data()+p, slen); p+=slen; return s;
}
static void w16(std::vector<uint8_t>& d, uint16_t v) {
    d.push_back(v&0xFF); d.push_back(v>>8);
}
static void w32(std::vector<uint8_t>& d, uint32_t v) {
    d.push_back(v&0xFF); d.push_back((v>>8)&0xFF);
    d.push_back((v>>16)&0xFF); d.push_back(v>>24);
}
static void write_str(std::vector<uint8_t>& d, const std::string& s) {
    if (s.size() < 0xFF) d.push_back((uint8_t)s.size());
    else { d.push_back(0xFF); w16(d, (uint16_t)s.size()); }
    d.insert(d.end(), s.begin(), s.end());
}
static void write_tag(std::vector<uint8_t>& out, uint8_t tag,
                      const std::vector<uint8_t>& payload) {
    out.push_back(tag);
    w32(out, (uint32_t)payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

// ── SC Header ─────────────────────────────────────────────────────────────────
struct SCHeader {
    uint16_t shape_c, mc_c, tex_c, tf_c, mat_c, ct_c;
    uint32_t unk_u32; uint8_t unk_u8;
    std::vector<uint16_t> export_ids;
    std::vector<std::string> export_names;
    size_t tag_stream_pos;
};

static SCHeader parse_header(const std::vector<uint8_t>& d) {
    SCHeader h{}; size_t p = 0;
    h.shape_c = r16(d,p); h.mc_c   = r16(d,p);
    h.tex_c   = r16(d,p); h.tf_c   = r16(d,p);
    h.mat_c   = r16(d,p); h.ct_c   = r16(d,p);
    h.unk_u32 = r32(d,p); h.unk_u8 = r8(d,p);
    uint16_t ec = r16(d,p);
    for (int i=0;i<ec;i++) h.export_ids.push_back(r16(d,p));
    for (int i=0;i<ec;i++) h.export_names.push_back(read_str(d,p));
    h.tag_stream_pos = p;
    return h;
}

static std::vector<uint8_t> build_header(const SCHeader& h) {
    std::vector<uint8_t> d;
    w16(d,h.shape_c); w16(d,h.mc_c); w16(d,h.tex_c);
    w16(d,h.tf_c);    w16(d,h.mat_c);w16(d,h.ct_c);
    w32(d,h.unk_u32); d.push_back(h.unk_u8);
    w16(d,(uint16_t)h.export_ids.size());
    for (auto id : h.export_ids)   w16(d,id);
    for (auto& n : h.export_names) write_str(d,n);
    return d;
}

// ── Tag stream ────────────────────────────────────────────────────────────────
struct Tag { uint8_t id; std::vector<uint8_t> payload; };

static std::vector<Tag> walk_tags(const std::vector<uint8_t>& d, size_t start) {
    std::vector<Tag> tags; size_t p = start;
    while (p+5 <= d.size()) {
        Tag t; t.id = d[p++];
        uint32_t len; memcpy(&len, d.data()+p, 4); p+=4;
        if (p+len > d.size()) break;
        t.payload.assign(d.begin()+p, d.begin()+p+len); p+=len;
        tags.push_back(t);
        if (t.id == 0) break;
    }
    return tags;
}

// ── MC tag 49 ────────────────────────────────────────────────────────────────
struct MCBind { uint16_t child_id, mat_idx, ct_idx; };
struct MCFrame { uint16_t bind_count; std::string label; };
struct MC49 {
    uint16_t mc_id, frame_count, bind_count;
    std::vector<MCBind> binds;
    std::vector<MCFrame> frames;
    std::vector<uint8_t> tail;
};

static MC49 parse_mc49(const std::vector<uint8_t>& p_raw) {
    MC49 mc{}; size_t p = 0;
    mc.mc_id       = r16(p_raw,p);
    mc.frame_count = r16(p_raw,p);
    mc.bind_count  = r16(p_raw,p);
    for (int i=0;i<mc.bind_count;i++) {
        MCBind b; b.child_id=r16(p_raw,p); b.mat_idx=r16(p_raw,p); b.ct_idx=r16(p_raw,p);
        mc.binds.push_back(b);
    }
    for (int i=0;i<mc.frame_count && p+3<=p_raw.size();i++) {
        MCFrame f; f.bind_count=r16(p_raw,p);
        uint8_t llen=r8(p_raw,p);
        if (p+llen<=p_raw.size()) { f.label=std::string((char*)p_raw.data()+p,llen); p+=llen; }
        mc.frames.push_back(f);
    }
    mc.tail.assign(p_raw.begin()+p, p_raw.end());
    return mc;
}

static std::vector<uint8_t> build_mc49(const MC49& mc) {
    std::vector<uint8_t> d;
    w16(d,mc.mc_id); w16(d,mc.frame_count); w16(d,mc.bind_count);
    for (auto& b : mc.binds) { w16(d,b.child_id); w16(d,b.mat_idx); w16(d,b.ct_idx); }
    for (auto& f : mc.frames) {
        w16(d,f.bind_count);
        d.push_back((uint8_t)f.label.size());
        d.insert(d.end(), f.label.begin(), f.label.end());
    }
    d.insert(d.end(), mc.tail.begin(), mc.tail.end());
    return d;
}

// ── Dependency collector ──────────────────────────────────────────────────────
static void collect_deps(uint16_t mc_id,
    const std::map<uint16_t,std::vector<uint8_t>>& mc_map,
    const std::map<uint16_t,std::vector<uint8_t>>& shape_map,
    std::set<uint16_t>& visited_mc,
    std::set<uint16_t>& visited_shape)
{
    if (visited_mc.count(mc_id)) return;
    visited_mc.insert(mc_id);
    auto it = mc_map.find(mc_id);
    if (it == mc_map.end()) return;
    MC49 mc = parse_mc49(it->second);
    for (auto& b : mc.binds) {
        if (b.child_id == 0xFFFF) continue;
        if (shape_map.count(b.child_id)) visited_shape.insert(b.child_id);
        else collect_deps(b.child_id, mc_map, shape_map, visited_mc, visited_shape);
    }
}

// ── Main mc_copy ──────────────────────────────────────────────────────────────
// names: список экспортов через запятую (пусто = все)
std::string mc_copy(const std::string& src_path, const std::string& dst_path,
                    const std::string& names, const std::string& out_dir) {
    std::vector<uint8_t> src_data, dst_data;
    if (!sc_read_file(src_path, src_data)) return "Error: cannot read src: " + src_path;
    if (!sc_read_file(dst_path, dst_data)) return "Error: cannot read dst: " + dst_path;

    ScHeader src_hdr_sc = sc_parse_header(src_data);
    ScHeader dst_hdr_sc = sc_parse_header(dst_data);
    if (!src_hdr_sc.valid) return "Error: src not SC";
    if (!dst_hdr_sc.valid) return "Error: dst not SC";

    std::vector<uint8_t> src_raw, dst_raw;
    try {
        src_raw = sc_decompress(src_data, src_hdr_sc.comp);
        dst_raw = sc_decompress(dst_data, dst_hdr_sc.comp);
    } catch (std::exception& e) { return std::string("Error: decompress: ")+e.what(); }

    SCHeader src_hdr = parse_header(src_raw);
    SCHeader dst_hdr = parse_header(dst_raw);

    auto src_tags = walk_tags(src_raw, src_hdr.tag_stream_pos);
    auto dst_tags = walk_tags(dst_raw, dst_hdr.tag_stream_pos);

    // Index source
    std::map<uint16_t,std::vector<uint8_t>> src_mc_map, src_shape_map;
    std::vector<std::vector<uint8_t>> src_mat_list, src_ct_list;
    for (auto& t : src_tags) {
        if (t.id==49 && t.payload.size()>=2) {
            uint16_t id; memcpy(&id,t.payload.data(),2);
            src_mc_map[id]=t.payload;
        } else if (t.id==18 && t.payload.size()>=2) {
            uint16_t id; memcpy(&id,t.payload.data(),2);
            src_shape_map[id]=t.payload;
        } else if (t.id==8) src_mat_list.push_back(t.payload);
        else if (t.id==9)   src_ct_list.push_back(t.payload);
    }

    // Map export names → ids
    std::map<std::string,uint16_t> src_export_map;
    for (size_t i=0;i<src_hdr.export_names.size();i++)
        src_export_map[src_hdr.export_names[i]]=src_hdr.export_ids[i];

    // Parse names filter (comma-separated)
    std::set<std::string> name_filter;
    if (!names.empty()) {
        std::istringstream ss(names);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.front()==' ') tok.erase(tok.begin());
            while (!tok.empty() && tok.back()==' ') tok.pop_back();
            if (!tok.empty()) name_filter.insert(tok);
        }
    }

    // Use all exports from src (or filtered subset)
    std::vector<std::pair<std::string,uint16_t>> targets;
    for (auto& kv : src_export_map) {
        if (name_filter.empty() || name_filter.count(kv.first))
            targets.push_back(kv);
    }

    // Collect deps
    std::set<uint16_t> needed_mc, needed_shape;
    for (auto& [name,mc_id] : targets)
        collect_deps(mc_id, src_mc_map, src_shape_map, needed_mc, needed_shape);

    // Compute offsets
    uint16_t dst_max_id = 0;
    for (auto& t : dst_tags)
        if ((t.id==18||t.id==49) && t.payload.size()>=2) {
            uint16_t id; memcpy(&id,t.payload.data(),2);
            if (id!=0xFFFF) dst_max_id=std::max(dst_max_id,id);
        }
    uint16_t id_off  = dst_max_id+1;
    uint16_t mat_off = dst_hdr.mat_c;
    uint16_t ct_off  = dst_hdr.ct_c;

    // Remap shapes
    std::vector<Tag> new_shape_tags;
    std::map<uint16_t,uint16_t> shape_id_map;
    for (auto id : needed_shape) {
        auto& payload = src_shape_map[id];
        uint16_t new_id = id+id_off;
        shape_id_map[id]=new_id;
        std::vector<uint8_t> np=payload;
        memcpy(np.data(),&new_id,2);
        new_shape_tags.push_back({18,np});
    }

    // Remap MCs
    std::map<uint16_t,uint16_t> mc_id_map;
    for (auto id : needed_mc) mc_id_map[id]=id+id_off;
    std::vector<Tag> new_mc_tags;
    for (auto id : needed_mc) {
        MC49 mc = parse_mc49(src_mc_map[id]);
        mc.mc_id = mc_id_map[id];
        for (auto& b : mc.binds) {
            if (b.child_id!=0xFFFF) {
                if (mc_id_map.count(b.child_id)) b.child_id=mc_id_map[b.child_id];
                else if (shape_id_map.count(b.child_id)) b.child_id=shape_id_map[b.child_id];
                else b.child_id+=id_off;
            }
            if (b.mat_idx!=0xFFFF) b.mat_idx+=mat_off;
            if (b.ct_idx !=0xFFFF) b.ct_idx +=ct_off;
        }
        new_mc_tags.push_back({49,build_mc49(mc)});
    }

    // New export table
    SCHeader new_hdr = dst_hdr;
    new_hdr.shape_c += (uint16_t)new_shape_tags.size();
    new_hdr.mc_c    += (uint16_t)new_mc_tags.size();
    new_hdr.mat_c   += (uint16_t)src_mat_list.size();
    new_hdr.ct_c    += (uint16_t)src_ct_list.size();
    for (auto& [name,old_id] : targets) {
        new_hdr.export_ids.push_back(mc_id_map.count(old_id)?mc_id_map[old_id]:old_id+id_off);
        new_hdr.export_names.push_back(name);
    }

    // Assemble
    std::vector<Tag> dst_no_end;
    for (auto& t : dst_tags) if (t.id!=0) dst_no_end.push_back(t);

    std::vector<uint8_t> new_raw = build_header(new_hdr);
    for (auto& t : dst_no_end)    write_tag(new_raw, t.id, t.payload);
    for (auto& p : src_mat_list)  write_tag(new_raw, 8, p);
    for (auto& p : src_ct_list)   write_tag(new_raw, 9, p);
    for (auto& t : new_shape_tags) write_tag(new_raw, t.id, t.payload);
    for (auto& t : new_mc_tags)    write_tag(new_raw, t.id, t.payload);
    // End tag
    new_raw.push_back(0); w32(new_raw,0);

    // Compress + prepend SC file header
    std::vector<uint8_t> sc_file_hdr(dst_data.begin(), dst_data.begin()+dst_hdr_sc.comp.offset);
    std::vector<uint8_t> comp;
    try { comp = sc_compress(new_raw, dst_hdr_sc.comp.kind); }
    catch (std::exception& e) { return std::string("Error: compress: ")+e.what(); }

    sc_file_hdr.insert(sc_file_hdr.end(), comp.begin(), comp.end());

    std::string out_path = out_dir + "/" + sc_basename_no_ext(dst_path) + "_mc.sc";
    if (!sc_write_file(out_path, sc_file_hdr)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: MC Copy complete\n";
    r << "  Copied: " << targets.size() << " exports\n";
    r << "  MCs: " << needed_mc.size() << "  Shapes: " << needed_shape.size() << "\n";
    r << "  Output: " << sc_basename_no_ext(dst_path) << "_mc.sc\n";
    r << "  Dir: " << out_dir;
    return r.str();
}
