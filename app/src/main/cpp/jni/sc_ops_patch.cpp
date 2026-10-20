/**
 * sc_ops_patch_v2.cpp — sc_extract_ktx с автоиндексом и показом доступных
 * Заменяет sc_ops_patch.cpp
 */
#include "sc_core.h"
#include <sstream>
#include <cstring>
#include <android/log.h>

#define TAG "SCOps2"

static const uint8_t KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

std::string sc_extract_ktx(const std::string& sc_path, int idx, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "Error: cannot read " + sc_path;

    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "Error: not an SC file";

    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: decompress: ") + e.what(); }

    // Находим все KTX блоки + их размеры
    struct KTXInfo { size_t offset; int w, h; };
    std::vector<KTXInfo> ktxs;

    for (size_t i = 0; i+12 <= raw.size(); i++) {
        if (memcmp(raw.data()+i, KTX_MAGIC, 12) == 0) {
            int w=0, h=0;
            if (i+44 <= raw.size()) {
                memcpy(&w, raw.data()+i+36, 4);
                memcpy(&h, raw.data()+i+40, 4);
            }
            ktxs.push_back({i, w, h});
        }
    }

    if (ktxs.empty()) return "Error: no KTX blocks found in SC";

    // Показываем список если индекс не задан (-1) или вне диапазона
    if (idx < 0 || idx >= (int)ktxs.size()) {
        std::ostringstream r;
        r << "Info: found " << ktxs.size() << " KTX block(s):\n\n";
        for (int i = 0; i < (int)ktxs.size(); i++)
            r << "  [" << i << "] " << ktxs[i].w << "x" << ktxs[i].h << "\n";
        r << "\nEnter index (0-" << ktxs.size()-1 << ") and run again";
        return r.str();
    }

    size_t start = ktxs[idx].offset;
    size_t end   = (idx+1 < (int)ktxs.size()) ? ktxs[idx+1].offset : raw.size();

    std::string stem     = sc_basename_no_ext(sc_path);
    std::string out_path = out_dir + "/" + stem + "_" + std::to_string(idx) + ".ktx";
    std::vector<uint8_t> ktx_data(raw.begin()+start, raw.begin()+end);
    if (!sc_write_file(out_path, ktx_data)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: KTX extracted\n";
    r << "  File: " << stem << "_" << idx << ".ktx\n";
    r << "  Size: " << ktxs[idx].w << "x" << ktxs[idx].h << "\n";
    r << "  Bytes: " << ktx_data.size() << "\n";
    r << "  Dir: " << out_dir;
    return r.str();
}
