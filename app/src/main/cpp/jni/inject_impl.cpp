/**
 * inject_impl.cpp
 * Inject Preview + List для SC v5
 * (inject_put требует полного FlatBuffer парсера — пока заглушка с пояснением)
 */
#include "sc_core.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "astc_decoder.h"
#include <sstream>
#include <cstring>
#include <android/log.h>

#define TAG "Inject"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const uint8_t KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

// Декодировать KTX блок → RGBA
static bool decode_ktx(const uint8_t* data, size_t size,
                        int& w, int& h, std::vector<uint8_t>& rgba) {
    if (size < 64 || memcmp(data, KTX_MAGIC, 12) != 0) return false;
    size_t p = 16;
    auto rd32 = [&]() -> uint32_t {
        if (p+4>size) return 0; uint32_t v; memcpy(&v,data+p,4); p+=4; return v;
    };
    uint32_t glType=rd32(); rd32(); uint32_t glFormat=rd32();
    uint32_t glInt=rd32(); rd32();
    w=(int)rd32(); h=(int)rd32();
    rd32();rd32();rd32();rd32();
    uint32_t kvd=rd32(); p+=kvd;
    uint32_t imgSize=rd32();
    if (p+imgSize>size) imgSize=(uint32_t)(size-p);

    if (glType==0 && glFormat==0) {
        struct F{uint32_t gl;int bw,bh;};
        static const F fmts[]={
            {0x93B7,8,8},{0x93D7,8,8},{0x93B0,4,4},{0x93D0,4,4},
            {0x93B4,6,6},{0x93D4,6,6},{0x93BB,10,10},{0x93BD,12,12},{0,0,0}
        };
        int bw=8,bh=8;
        for(auto* f=fmts;f->gl;f++) if(f->gl==glInt){bw=f->bw;bh=f->bh;break;}
        bool srgb=(glInt>=0x93D0);
        return astc_decode(data+p, imgSize, w, h, bw, bh, rgba, srgb);
    }
    size_t needed=(size_t)w*h*4;
    if (p+needed>size) return false;
    rgba.assign(data+p, data+p+needed);
    return true;
}

// Список экспортов SC (inject_list == sc_exports по сути)
std::string inject_list(const std::string& sc_path) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "Error: cannot read " + sc_path;
    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "Error: not an SC file";

    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: ")+e.what(); }

    if (raw.size() < 15) return "Error: SC too small";
    size_t pos = 0;
    auto r16 = [&]() -> uint16_t {
        if (pos+2>raw.size()) return 0;
        uint16_t v; memcpy(&v,raw.data()+pos,2); pos+=2; return v;
    };
    auto r8  = [&]() -> uint8_t { return pos<raw.size()?raw[pos++]:0; };
    auto r32 = [&]() -> uint32_t {
        if (pos+4>raw.size()) return 0;
        uint32_t v; memcpy(&v,raw.data()+pos,4); pos+=4; return v;
    };
    auto read_str = [&]() -> std::string {
        uint8_t len=r8(); uint16_t slen=len;
        if (len==0xFF) slen=r16();
        if (pos+slen>raw.size()) return "";
        std::string s((char*)raw.data()+pos,slen); pos+=slen; return s;
    };

    uint16_t shape_c=r16(),mc_c=r16(),tex_c=r16(),tf_c=r16(),mat_c=r16(),ct_c=r16();
    r32(); r8();
    uint16_t export_count=r16();
    if (export_count>10000) return "Error: invalid export count";

    std::vector<uint16_t> ids(export_count);
    for (auto& id : ids) id=r16();
    std::vector<std::string> names(export_count);
    for (auto& n : names) n=read_str();

    std::ostringstream r;
    r << "OK: SC Exports\n";
    r << "  Shapes: " << shape_c << "  MC: " << mc_c << "  Tex: " << tex_c << "\n";
    r << "  Total exports: " << export_count << "\n\n";
    for (int i=0;i<(int)export_count;i++)
        r << "  [" << ids[i] << "] " << names[i] << "\n";
    return r.str();
}

// Inject Preview — рисует атлас со всеми KTX текстурами объединёнными
std::string inject_preview(const std::string& sc_path, const std::string& out_dir) {
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "Error: cannot read " + sc_path;
    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "Error: not an SC file";

    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: ")+e.what(); }

    // Найти все KTX блоки
    std::vector<size_t> ktx_offs;
    for (size_t i=0;i+12<=raw.size();i++)
        if (memcmp(raw.data()+i, KTX_MAGIC, 12)==0) ktx_offs.push_back(i);

    if (ktx_offs.empty()) return "Error: no KTX textures found";

    std::string stem = sc_basename_no_ext(sc_path);
    std::ostringstream result;
    int saved=0;

    for (int i=0;i<(int)ktx_offs.size();i++) {
        size_t start=ktx_offs[i];
        size_t end=(i+1<(int)ktx_offs.size())?ktx_offs[i+1]:raw.size();
        int w=0,h=0; std::vector<uint8_t> rgba;
        if (!decode_ktx(raw.data()+start, end-start, w, h, rgba)) {
            result << "Warning: KTX[" << i << "] decode failed\n";
            continue;
        }
        std::string suffix = ktx_offs.size()>1 ? "_tex"+std::to_string(i) : "_preview";
        std::string out_path = out_dir+"/"+stem+suffix+".png";
        if (!stbi_write_png(out_path.c_str(), w, h, 4, rgba.data(), w*4)) {
            result << "Error: write failed " << out_path << "\n";
            continue;
        }
        result << stem << suffix << ".png  " << w << "x" << h << "\n";
        saved++;
    }

    if (saved==0) return "Error: no textures decoded";
    std::ostringstream r;
    r << "OK: Inject Preview\n";
    r << "  Textures: " << saved << " of " << ktx_offs.size() << "\n";
    r << result.str();
    r << "  Dir: " << out_dir;
    return r.str();
}

// Inject Put — вставка PNG в нужный KTX блок SC
// name: имя спрайта (используется как суффикс выходного файла, если не пустой)
std::string inject_put(const std::string& sc_path, const std::string& sprite_path,
                       const std::string& name, const std::string& out_dir) {
    // Читаем оригинальный SC
    std::vector<uint8_t> data;
    if (!sc_read_file(sc_path, data)) return "Error: cannot read " + sc_path;
    ScHeader hdr = sc_parse_header(data);
    if (!hdr.valid) return "Error: not an SC file";

    std::vector<uint8_t> raw;
    try { raw = sc_decompress(data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: ")+e.what(); }

    // Находим первый KTX блок
    size_t ktx_start = 0;
    bool found = false;
    for (size_t i=0;i+12<=raw.size();i++) {
        if (memcmp(raw.data()+i, KTX_MAGIC, 12)==0) { ktx_start=i; found=true; break; }
    }
    if (!found) return "Error: no KTX block found in SC";

    // Декодируем существующую текстуру
    size_t ktx_end = raw.size();
    for (size_t i=ktx_start+12;i+12<=raw.size();i++)
        if (memcmp(raw.data()+i, KTX_MAGIC, 12)==0) { ktx_end=i; break; }

    int orig_w=0, orig_h=0; std::vector<uint8_t> orig_rgba;
    if (!decode_ktx(raw.data()+ktx_start, ktx_end-ktx_start, orig_w, orig_h, orig_rgba))
        return "Error: cannot decode original KTX";

    // Загружаем спрайт
    int sw=0,sh=0,sc2=0;
    uint8_t* sprite = stbi_load(sprite_path.c_str(), &sw, &sh, &sc2, 4);
    if (!sprite) return "Error: cannot load sprite PNG: " + sprite_path;

    // Масштабируем спрайт до размера оригинала если нужно
    std::vector<uint8_t> sprite_rgba(orig_w*orig_h*4, 0);
    if (sw==orig_w && sh==orig_h) {
        memcpy(sprite_rgba.data(), sprite, orig_w*orig_h*4);
    } else {
        // Простой nearest-neighbor
        for (int y=0;y<orig_h;y++) for (int x=0;x<orig_w;x++) {
            int sx=x*sw/orig_w, sy=y*sh/orig_h;
            memcpy(sprite_rgba.data()+(y*orig_w+x)*4, sprite+(sy*sw+sx)*4, 4);
        }
    }
    stbi_image_free(sprite);

    // Перекодируем в ASTC 8x8
    std::vector<uint8_t> new_astc;
    if (!astc_encode(sprite_rgba.data(), orig_w, orig_h, 8, 8, new_astc))
        return "Error: ASTC encode failed";

    // Собираем новый KTX заголовок (копируем оригинальный + меняем данные)
    std::vector<uint8_t> orig_ktx(raw.begin()+ktx_start, raw.begin()+ktx_end);
    // Находим offset данных в KTX (после заголовка)
    size_t ktx_hdr_size = 64; // минимальный KTX header
    if (orig_ktx.size() > 60) {
        uint32_t kvd; memcpy(&kvd, orig_ktx.data()+60, 4);
        ktx_hdr_size = 64 + kvd + 4; // + imageSize field
    }

    // Новый KTX = старый заголовок + новые данные ASTC
    std::vector<uint8_t> new_ktx(orig_ktx.begin(), orig_ktx.begin()+std::min(ktx_hdr_size,orig_ktx.size()));
    // Обновляем imageSize
    uint32_t new_img_size = (uint32_t)new_astc.size();
    if (new_ktx.size() >= ktx_hdr_size)
        memcpy(new_ktx.data()+ktx_hdr_size-4, &new_img_size, 4);
    new_ktx.insert(new_ktx.end(), new_astc.begin(), new_astc.end());

    // Собираем новый raw
    std::vector<uint8_t> new_raw;
    new_raw.insert(new_raw.end(), raw.begin(), raw.begin()+ktx_start);
    new_raw.insert(new_raw.end(), new_ktx.begin(), new_ktx.end());
    new_raw.insert(new_raw.end(), raw.begin()+ktx_end, raw.end());

    // Перепаковываем SC
    std::vector<uint8_t> sc_hdr(data.begin(), data.begin()+hdr.comp.offset);
    std::vector<uint8_t> comp;
    try { comp = sc_compress(new_raw, hdr.comp.kind); }
    catch (std::exception& e) { return std::string("Error: compress: ")+e.what(); }
    sc_hdr.insert(sc_hdr.end(), comp.begin(), comp.end());

    std::string suffix = name.empty() ? "_injected" : ("_" + name);
    std::string out_path = out_dir+"/"+sc_basename_no_ext(sc_path)+suffix+".sc";
    if (!sc_write_file(out_path, sc_hdr)) return "Error: write failed: "+out_path;

    std::ostringstream r;
    r << "OK: Inject complete\n";
    r << "  Texture: " << orig_w << "x" << orig_h << "\n";
    r << "  Output: " << sc_basename_no_ext(sc_path) << "_injected.sc\n";
    r << "  Dir: " << out_dir;
    return r.str();
}
