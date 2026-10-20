/**
 * png2sc_op.cpp — PNG to SC (точный порт Termux sc_tool)
 * Логика 1:1 с Python: берём блок из оригинального KTX, кодируем напрямую.
 */
#include "sc_core.h"
#include "stb/stb_image.h"
#include "astc_decoder.h"
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>
#include <sys/stat.h>
#include <android/log.h>

#define TAG "Png2SC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const uint8_t KTX_MAGIC[12] = {
    0xab,'K','T','X',' ','1','1',0xbb,0x0d,0x0a,0x1a,0x0a
};

struct GLBlock { uint32_t gl; int bw, bh; };
static const GLBlock GL_BLOCKS[] = {
    {0x93B0,4,4},{0x93B1,5,4},{0x93B2,5,5},{0x93B3,6,5},
    {0x93B4,6,6},{0x93B5,8,5},{0x93B6,8,6},{0x93B7,8,8},
    {0x93B8,10,5},{0x93B9,10,6},{0x93BA,10,8},{0x93BB,10,10},
    {0x93BC,12,10},{0x93BD,12,12},
    {0x93D0,4,4},{0x93D1,5,4},{0x93D2,5,5},{0x93D3,6,5},
    {0x93D4,6,6},{0x93D5,8,5},{0x93D6,8,6},{0x93D7,8,8},
    {0x93D8,10,5},{0x93D9,10,6},{0x93DA,10,8},{0x93DB,10,10},
    {0x93DC,12,10},{0x93DD,12,12},{0,0,0}
};

static size_t mip0_size(int w, int h, int bw, int bh) {
    return (size_t)((w+bw-1)/bw) * (size_t)((h+bh-1)/bh) * 16;
}
static bool get_block(uint32_t gl, int& bw, int& bh) {
    for (const auto* b = GL_BLOCKS; b->gl; b++)
        if (b->gl == gl) { bw=b->bw; bh=b->bh; return true; }
    bw=8; bh=8; return false;
}

// MD5
static uint32_t md5_T[64];
static bool md5_init_done = false;
static void md5_init_T() {
    if (md5_init_done) return;
    for (int i=0;i<64;i++) md5_T[i]=(uint32_t)(4294967296.0*fabs(sin(i+1.0)));
    md5_init_done = true;
}
#define ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))
#define F(x,y,z) (((x)&(y))|(~(x)&(z)))
#define G(x,y,z) (((x)&(z))|((y)&~(z)))
#define H(x,y,z) ((x)^(y)^(z))
#define I(x,y,z) ((y)^((x)|~(z)))
#define STEP(f,a,b,c,d,k,s,i) a=b+ROL(a+f(b,c,d)+X[k]+md5_T[i-1],s)
static void md5_block(uint32_t s[4], const uint8_t* blk) {
    uint32_t X[16],a=s[0],b=s[1],c=s[2],d=s[3];
    for(int i=0;i<16;i++) memcpy(&X[i],blk+i*4,4);
    STEP(F,a,b,c,d,0,7,1);STEP(F,d,a,b,c,1,12,2);STEP(F,c,d,a,b,2,17,3);STEP(F,b,c,d,a,3,22,4);
    STEP(F,a,b,c,d,4,7,5);STEP(F,d,a,b,c,5,12,6);STEP(F,c,d,a,b,6,17,7);STEP(F,b,c,d,a,7,22,8);
    STEP(F,a,b,c,d,8,7,9);STEP(F,d,a,b,c,9,12,10);STEP(F,c,d,a,b,10,17,11);STEP(F,b,c,d,a,11,22,12);
    STEP(F,a,b,c,d,12,7,13);STEP(F,d,a,b,c,13,12,14);STEP(F,c,d,a,b,14,17,15);STEP(F,b,c,d,a,15,22,16);
    STEP(G,a,b,c,d,1,5,17);STEP(G,d,a,b,c,6,9,18);STEP(G,c,d,a,b,11,14,19);STEP(G,b,c,d,a,0,20,20);
    STEP(G,a,b,c,d,5,5,21);STEP(G,d,a,b,c,10,9,22);STEP(G,c,d,a,b,15,14,23);STEP(G,b,c,d,a,4,20,24);
    STEP(G,a,b,c,d,9,5,25);STEP(G,d,a,b,c,14,9,26);STEP(G,c,d,a,b,3,14,27);STEP(G,b,c,d,a,8,20,28);
    STEP(G,a,b,c,d,13,5,29);STEP(G,d,a,b,c,2,9,30);STEP(G,c,d,a,b,7,14,31);STEP(G,b,c,d,a,12,20,32);
    STEP(H,a,b,c,d,5,4,33);STEP(H,d,a,b,c,8,11,34);STEP(H,c,d,a,b,11,16,35);STEP(H,b,c,d,a,14,23,36);
    STEP(H,a,b,c,d,1,4,37);STEP(H,d,a,b,c,4,11,38);STEP(H,c,d,a,b,7,16,39);STEP(H,b,c,d,a,10,23,40);
    STEP(H,a,b,c,d,13,4,41);STEP(H,d,a,b,c,0,11,42);STEP(H,c,d,a,b,3,16,43);STEP(H,b,c,d,a,6,23,44);
    STEP(H,a,b,c,d,9,4,45);STEP(H,d,a,b,c,12,11,46);STEP(H,c,d,a,b,15,16,47);STEP(H,b,c,d,a,2,23,48);
    STEP(I,a,b,c,d,0,6,49);STEP(I,d,a,b,c,7,10,50);STEP(I,c,d,a,b,14,15,51);STEP(I,b,c,d,a,5,21,52);
    STEP(I,a,b,c,d,12,6,53);STEP(I,d,a,b,c,3,10,54);STEP(I,c,d,a,b,10,15,55);STEP(I,b,c,d,a,1,21,56);
    STEP(I,a,b,c,d,8,6,57);STEP(I,d,a,b,c,15,10,58);STEP(I,c,d,a,b,6,15,59);STEP(I,b,c,d,a,13,21,60);
    STEP(I,a,b,c,d,4,6,61);STEP(I,d,a,b,c,11,10,62);STEP(I,c,d,a,b,2,15,63);STEP(I,b,c,d,a,9,21,64);
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
}
static void compute_md5(const uint8_t* data, size_t len, uint8_t out[16]) {
    md5_init_T();
    uint32_t s[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    size_t i=0;
    for(;i+64<=len;i+=64) md5_block(s,data+i);
    uint8_t buf[128]={};
    size_t rem=len-i; memcpy(buf,data+i,rem); buf[rem]=0x80;
    uint64_t bits=(uint64_t)len*8;
    if(rem<56){memcpy(buf+56,&bits,8);md5_block(s,buf);}
    else{memcpy(buf+120,&bits,8);md5_block(s,buf);md5_block(s,buf+64);}
    memcpy(out,s,16);
}

struct ShapeRect {
    uint16_t id = 0;
    uint16_t tex_idx = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid = false;
    std::vector<std::vector<std::pair<float,float>>> polys; // в пикселях атласа, уже отсортированы по углу
};

static void sort_poly_by_angle(std::vector<std::pair<float,float>>& p) {
    if (p.size() < 3) return;
    float cx = 0, cy = 0;
    for (auto& q : p) { cx += q.first; cy += q.second; }
    cx /= p.size(); cy /= p.size();
    std::sort(p.begin(), p.end(), [&](const std::pair<float,float>& a, const std::pair<float,float>& b) {
        return std::atan2(a.second - cy, a.first - cx) < std::atan2(b.second - cy, b.first - cx);
    });
}

static bool point_in_tri(float px, float py, const std::pair<float,float>& a,
                         const std::pair<float,float>& b, const std::pair<float,float>& c) {
    float area = (b.first-a.first)*(c.second-a.second) - (b.second-a.second)*(c.first-a.first);
    if (std::fabs(area) < 1e-6f) return false;
    float w0 = ((b.first-px)*(c.second-py) - (b.second-py)*(c.first-px)) / area;
    float w1 = ((c.first-px)*(a.second-py) - (c.second-py)*(a.first-px)) / area;
    float w2 = 1.f - w0 - w1;
    const float e = -1e-3f;
    return w0 >= e && w1 >= e && w2 >= e;
}

static bool point_in_polys(const ShapeRect& r, float px, float py) {
    for (auto& poly : r.polys)
        for (size_t i = 1; i + 1 < poly.size(); i++)
            if (point_in_tri(px, py, poly[0], poly[i], poly[i+1])) return true;
    return false;
}

static bool find_shape_rect(const std::vector<uint8_t>& raw, uint16_t wanted_id,
                            int atlas_w, int atlas_h, ShapeRect& out) {
    size_t p=0;
    auto r8=[&]()->uint8_t{ return p<raw.size()?raw[p++]:0; };
    auto r16=[&]()->uint16_t{ if(p+2>raw.size()){p=raw.size();return 0;} uint16_t v; memcpy(&v,raw.data()+p,2);p+=2;return v; };
    auto r32=[&]()->uint32_t{ if(p+4>raw.size()){p=raw.size();return 0;} uint32_t v;memcpy(&v,raw.data()+p,4);p+=4;return v; };
    if(raw.size()<19) return false;
    r16();r16();r16();r16();r16();r16(); p += 5;
    uint16_t ec=r16();
    if(ec>10000) return false;
    for(uint16_t i=0;i<ec;i++) r16();
    for(uint16_t i=0;i<ec;i++) { uint8_t n=r8(); if(n==0xFF) p+=2; p+=n; if(p>raw.size()) return false; }

    while(p+5<=raw.size()) {
        uint8_t tag=r8(); int32_t len=(int32_t)r32();
        if(tag==0) break;
        if(len<0 || p+(size_t)len>raw.size()) return false;
        size_t start=p,end=p+(size_t)len;
        if((tag==2 || tag==18) && end-start>=4) {
            uint16_t id=r16(); uint16_t draw_c=r16();
            if(tag==18 && p+2<=end) r16(); // total point count
            for(uint16_t d=0; d<draw_c && p+5<=end; ++d) {
                uint8_t ctag=r8(); int32_t clen=(int32_t)r32();
                if(clen<0 || p+(size_t)clen>end) break;
                size_t ce=p+(size_t)clen;
                if(id==wanted_id && (ctag==4 || ctag==17 || ctag==22) && p<ce) {
                    uint8_t tex=(uint8_t)r8();
                    uint16_t vc=4;
                    if(ctag!=4 && p<ce) vc=r8();
                    if(vc>0 && p+(size_t)vc*8<=ce && (!out.valid || tex==out.tex_idx)) {
                        p += (size_t)vc*8; // X/Y int32 twips
                        float min_u=1e30f,min_v=1e30f,max_u=-1e30f,max_v=-1e30f;
                        std::vector<std::pair<float,float>> poly;
                        for(uint16_t i=0;i<vc;i++) {
                            uint16_t u=r16(),v=r16();
                            float uf=(float)u/65535.0f*atlas_w, vf=(float)v/65535.0f*atlas_h;
                            poly.push_back({uf,vf});
                            min_u=std::min(min_u,uf);max_u=std::max(max_u,uf);min_v=std::min(min_v,vf);max_v=std::max(max_v,vf);
                        }
                        int nx0=std::max(0,(int)std::floor(min_u+0.001f));
                        int ny0=std::max(0,(int)std::floor(min_v+0.001f));
                        int nx1=std::min(atlas_w,(int)std::ceil(max_u-0.001f));
                        int ny1=std::min(atlas_h,(int)std::ceil(max_v-0.001f));
                        if(nx1>nx0 && ny1>ny0) {
                            if(!out.valid) { out.x0=nx0; out.y0=ny0; out.x1=nx1; out.y1=ny1; }
                            else { out.x0=std::min(out.x0,nx0); out.y0=std::min(out.y0,ny0);
                                   out.x1=std::max(out.x1,nx1); out.y1=std::max(out.y1,ny1); }
                            out.id=id; out.tex_idx=tex; out.valid=true;
                            sort_poly_by_angle(poly);
                            out.polys.push_back(std::move(poly));
                        }
                    }
                }
                p=ce;
            }
            if(id==wanted_id && out.valid) return true;
        }
        p=end;
    }
    return false;
}

static bool decode_ktx_rgba(const uint8_t* d, size_t sz, int& w, int& h,
                            uint32_t& gl, int& bw, int& bh,
                            size_t& image_start, size_t& image_size,
                            std::vector<uint8_t>& rgba) {
    if(sz<64 || memcmp(d,KTX_MAGIC,12)!=0) return false;
    size_t p=12;
    auto rd32=[&]()->uint32_t{ if(p+4>sz) return 0; uint32_t v; memcpy(&v,d+p,4); p+=4; return v; };
    rd32(); rd32(); rd32(); rd32(); gl=rd32(); rd32();
    w=(int)rd32(); h=(int)rd32();
    rd32(); rd32(); rd32(); rd32();
    uint32_t kvd=rd32();
    if(p+(size_t)kvd+4>sz) return false;
    p+=kvd;
    uint32_t img=rd32();
    if(p+img>sz) return false;
    image_start=p; image_size=img;
    if(!get_block(gl,bw,bh)) return false;
    bool srgb=(gl>=0x93D0 && gl<=0x93DD);
    return astc_decode(d+p,img,w,h,bw,bh,rgba,srgb);
}

static std::vector<uint8_t> make_ktx_from_header(const uint8_t* old, size_t header_bytes,
                                                  const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> out(old, old+header_bytes);
    if(header_bytes<64) return {};
    uint32_t image_size=(uint32_t)pixels.size();
    memcpy(out.data()+header_bytes-4, &image_size, 4);
    out.insert(out.end(),pixels.begin(),pixels.end());
    return out;
}

std::string png2sc(const std::string& png_path, const std::string& orig_sc,
                   int idx, int rotate_deg, const std::string& out_dir) {
    int pw,ph,ch;
    uint8_t* loaded=stbi_load(png_path.c_str(),&pw,&ph,&ch,4);
    if(!loaded) return "Error: cannot load PNG: "+png_path;
    std::vector<uint8_t> input(loaded,loaded+(size_t)pw*ph*4);
    stbi_image_free(loaded);

    int actual_rotate=((rotate_deg%360)+360)%360;
    if(actual_rotate!=0) {
        int nw=(actual_rotate==90||actual_rotate==270)?ph:pw;
        int nh=(actual_rotate==90||actual_rotate==270)?pw:ph;
        std::vector<uint8_t> rot((size_t)nw*nh*4);
        for(int y=0;y<ph;y++) for(int x=0;x<pw;x++) {
            int nx,ny;
            if(actual_rotate==90){nx=ph-1-y;ny=x;}
            else if(actual_rotate==180){nx=pw-1-x;ny=ph-1-y;}
            else {nx=y;ny=pw-1-x;}
            memcpy(rot.data()+((size_t)ny*nw+nx)*4,input.data()+((size_t)y*pw+x)*4,4);
        }
        input.swap(rot); pw=nw; ph=nh;
    }

    std::vector<uint8_t> sc_data;
    if(!sc_read_file(orig_sc,sc_data)) return "Error: cannot read SC: "+orig_sc;
    ScHeader hdr=sc_parse_header(sc_data);
    if(!hdr.valid) return "Error: not an SC file";
    std::vector<uint8_t> raw;
    try { raw=sc_decompress(sc_data,hdr.comp); }
    catch(std::exception& e) { return std::string("Error: decompress: ")+e.what(); }

    // Locate KTX blocks and decode their atlases.
    struct KTXInfo { size_t off,end,image_start,image_size; int w,h,bw,bh; uint32_t gl; std::vector<uint8_t> rgba; };
    std::vector<KTXInfo> ktxs;
    for(size_t i=0;i+64<=raw.size();i++) {
        if(memcmp(raw.data()+i,KTX_MAGIC,12)!=0) continue;
        int w=0,h=0,bw=8,bh=8; uint32_t gl=0; size_t is=0,isz=0; std::vector<uint8_t> rgba;
        if(!decode_ktx_rgba(raw.data()+i,raw.size()-i,w,h,gl,bw,bh,is,isz,rgba)) continue;
        size_t end=i+is+isz;
        ktxs.push_back({i,end,i+is,isz,w,h,bw,bh,gl,std::move(rgba)});
        i=end>i?end-1:i;
    }
    if(ktxs.empty()) return "Error: no decodable KTX found";

    // idx is a Shape ID for the editor. If it isn't a Shape, preserve the
    // legacy behavior and interpret it as a KTX index.
    ShapeRect rect;
    bool has_shape=false;
    for(auto& k:ktxs) {
        rect = ShapeRect();
        if(find_shape_rect(raw,(uint16_t)std::max(0,idx),k.w,k.h,rect)) {
            if(rect.tex_idx<(uint16_t)ktxs.size()) {
                // tex_idx is normally the KTX order inside the SC.
                has_shape=true;
            }
            break;
        }
    }

    int target=-1;
    if(has_shape) target=(int)rect.tex_idx;
    else if(idx>=0 && idx<(int)ktxs.size()) target=idx;
    else {
        for(int i=0;i<(int)ktxs.size();i++) if(ktxs[i].w==pw&&ktxs[i].h==ph){target=i;break;}
        if(target<0) target=(int)ktxs.size()-1;
    }
    if(target<0 || target>=(int)ktxs.size()) return "Error: invalid target texture";

    KTXInfo& k=ktxs[target];
    std::vector<uint8_t> atlas=k.rgba;
    int x0=0,y0=0,x1=k.w,y1=k.h;
    if(has_shape) {
        x0=std::max(0,std::min(k.w,rect.x0)); y0=std::max(0,std::min(k.h,rect.y0));
        x1=std::max(x0,std::min(k.w,rect.x1)); y1=std::max(y0,std::min(k.h,rect.y1));
    }
    int tw=x1-x0, th=y1-y0;
    if(tw<=0||th<=0) return "Error: empty target rectangle";

    // Resize only the replacement sprite to its existing atlas rectangle.
    std::vector<uint8_t> repl((size_t)tw*th*4);
    float sx=(float)pw/tw, sy=(float)ph/th;
    for(int y=0;y<th;y++) for(int x=0;x<tw;x++) {
        float fx=(x+0.5f)*sx-0.5f, fy=(y+0.5f)*sy-0.5f;
        int x0s=std::max(0,std::min(pw-1,(int)std::floor(fx)));
        int y0s=std::max(0,std::min(ph-1,(int)std::floor(fy)));
        int x1s=std::min(pw-1,x0s+1), y1s=std::min(ph-1,y0s+1);
        float dx=std::max(0.f,std::min(1.f,fx-x0s)), dy=std::max(0.f,std::min(1.f,fy-y0s));
        for(int c=0;c<4;c++) {
            float v=input[((size_t)y0s*pw+x0s)*4+c]*(1-dx)*(1-dy)
                    +input[((size_t)y0s*pw+x1s)*4+c]*dx*(1-dy)
                    +input[((size_t)y1s*pw+x0s)*4+c]*(1-dx)*dy
                    +input[((size_t)y1s*pw+x1s)*4+c]*dx*dy;
            repl[((size_t)y*tw+x)*4+c]=(uint8_t)std::lround(v);
        }
    }
    long patched_px=0; bool used_poly=false;
    if(has_shape && !rect.polys.empty()) {
        used_poly=true;
        // Только пиксели внутри полигона(ов) шейпа — соседние спрайты не трогаем.
        for(int y=0;y<th;y++) for(int x=0;x<tw;x++) {
            if(!point_in_polys(rect,(float)(x+x0)+0.5f,(float)(y+y0)+0.5f)) continue;
            memcpy(atlas.data()+((size_t)(y+y0)*k.w+(x+x0))*4,repl.data()+((size_t)y*tw+x)*4,4);
            patched_px++;
        }
    } else {
        for(int y=0;y<th;y++) memcpy(atlas.data()+((size_t)(y+y0)*k.w+x0)*4,repl.data()+(size_t)y*tw*4,(size_t)tw*4);
    }

    std::vector<uint8_t> encoded;
    if(!astc_encode(atlas.data(),k.w,k.h,k.bw,k.bh,encoded)) return "Error: ASTC encode failed";
    if(encoded.size()!=k.image_size) return "Error: encoded KTX size changed unexpectedly";

    // Replace only the KTX image bytes. Header, dimensions, GL format and
    // surrounding SC structure remain byte-for-byte compatible.
    std::vector<uint8_t> new_raw=raw;
    memcpy(new_raw.data()+k.image_start,encoded.data(),encoded.size());

    std::vector<uint8_t> sc_hdr(sc_data.begin(),sc_data.begin()+hdr.comp.offset);
    if(sc_hdr.size()>=16) {
        uint8_t digest[16]; compute_md5(new_raw.data(),new_raw.size(),digest);
        memcpy(sc_hdr.data()+sc_hdr.size()-16,digest,16);
    }
    std::vector<uint8_t> comp;
    try { comp=sc_compress(new_raw,hdr.comp.kind); }
    catch(std::exception& e) { return std::string("Error: compress: ")+e.what(); }
    sc_hdr.insert(sc_hdr.end(),comp.begin(),comp.end());

    std::string stem=sc_basename_no_ext(orig_sc);
    if(stem.size()<4 || stem.substr(stem.size()-4)!="_tex") stem+="_tex";
    // Уникальное имя: иначе второй Replace перезаписывал бы свой же вход (и ломал Undo).
    {
        struct stat st; std::string cand=stem; int n=2;
        while(stat((out_dir+"/"+cand+".sc").c_str(),&st)==0) cand=stem+"_"+std::to_string(n++);
        stem=cand;
    }
    std::string out_path=out_dir+"/"+stem+".sc";
    if(!sc_write_file(out_path,sc_hdr)) return "Error: write failed: "+out_path;

    std::ostringstream r;
    r<<"OK: PNG to SC\n";
    if(has_shape) r<<"  Shape["<<idx<<"] -> KTX["<<target<<"]\n";
    else r<<"  KTX["<<target<<"] (legacy mode)\n";
    r<<"  Atlas: "<<k.w<<"x"<<k.h<<"  ASTC "<<k.bw<<"x"<<k.bh<<"\n";
    r<<"  Replaced: "<<tw<<"x"<<th<<" at ("<<x0<<","<<y0<<")\n";
    if(used_poly) r<<"  Polygon: "<<patched_px<<" of "<<(long)tw*th<<" px\n";
    r<<"  PNG: "<<pw<<"x"<<ph<<" -> "<<tw<<"x"<<th<<"\n";
    r<<"  Output: "<<stem<<".sc\n";
    r<<"  Dir: "<<out_dir;
    return r.str();
}


// ═══════════════════════════════════════════════════════════════════════════
//  BATCH (SC1/v0): много PNG за один проход, ASTC кодируется один раз
// ═══════════════════════════════════════════════════════════════════════════

static bool patch_rect_polys(std::vector<uint8_t>& atlas, int kw, int kh,
                             const std::vector<uint8_t>& input, int pw, int ph,
                             const ShapeRect& rect) {
    int x0=std::max(0,std::min(kw,rect.x0)), y0=std::max(0,std::min(kh,rect.y0));
    int x1=std::max(x0,std::min(kw,rect.x1)), y1=std::max(y0,std::min(kh,rect.y1));
    int tw=x1-x0, th=y1-y0;
    if(tw<=0||th<=0) return false;
    float sx=(float)pw/tw, sy=(float)ph/th;
    for(int y=0;y<th;y++) for(int x=0;x<tw;x++) {
        if(!rect.polys.empty() && !point_in_polys(rect,(float)(x+x0)+0.5f,(float)(y+y0)+0.5f)) continue;
        float fx=(x+0.5f)*sx-0.5f, fy=(y+0.5f)*sy-0.5f;
        int x0s=std::max(0,std::min(pw-1,(int)std::floor(fx)));
        int y0s=std::max(0,std::min(ph-1,(int)std::floor(fy)));
        int x1s=std::min(pw-1,x0s+1), y1s=std::min(ph-1,y0s+1);
        float dx=std::max(0.f,std::min(1.f,fx-x0s)), dy=std::max(0.f,std::min(1.f,fy-y0s));
        uint8_t* dst=atlas.data()+((size_t)(y+y0)*kw+(x+x0))*4;
        for(int c=0;c<4;c++) {
            float v=input[((size_t)y0s*pw+x0s)*4+c]*(1-dx)*(1-dy)
                   +input[((size_t)y0s*pw+x1s)*4+c]*dx*(1-dy)
                   +input[((size_t)y1s*pw+x0s)*4+c]*(1-dx)*dy
                   +input[((size_t)y1s*pw+x1s)*4+c]*dx*dy;
            dst[c]=(uint8_t)std::lround(v);
        }
    }
    return true;
}

std::string png2sc_batch(const std::vector<std::pair<int,std::string>>& items,
                         const std::string& orig_sc, const std::string& out_dir) {
    std::vector<uint8_t> sc_data;
    if(!sc_read_file(orig_sc,sc_data)) return "Error: cannot read SC: "+orig_sc;
    ScHeader hdr=sc_parse_header(sc_data);
    if(!hdr.valid) return "Error: not an SC file";
    std::vector<uint8_t> raw;
    try { raw=sc_decompress(sc_data,hdr.comp); }
    catch(std::exception& e) { return std::string("Error: decompress: ")+e.what(); }

    struct KTXInfo { size_t off,end,image_start,image_size; int w,h,bw,bh; uint32_t gl; std::vector<uint8_t> rgba; bool dirty=false; };
    std::vector<KTXInfo> ktxs;
    for(size_t i=0;i+64<=raw.size();i++) {
        if(memcmp(raw.data()+i,KTX_MAGIC,12)!=0) continue;
        int w=0,h=0,bw=8,bh=8; uint32_t gl=0; size_t is=0,isz=0; std::vector<uint8_t> rgba;
        if(!decode_ktx_rgba(raw.data()+i,raw.size()-i,w,h,gl,bw,bh,is,isz,rgba)) continue;
        size_t end=i+is+isz;
        KTXInfo k; k.off=i; k.end=end; k.image_start=i+is; k.image_size=isz;
        k.w=w; k.h=h; k.bw=bw; k.bh=bh; k.gl=gl; k.rgba=std::move(rgba);
        ktxs.push_back(std::move(k));
        i=end>i?end-1:i;
    }
    if(ktxs.empty()) return "Error: no decodable KTX found";

    int applied=0;
    std::vector<std::string> failures;
    for(auto& it:items) {
        int id=it.first;
        ShapeRect rect;
        bool found=find_shape_rect(raw,(uint16_t)std::max(0,id),ktxs[0].w,ktxs[0].h,rect);
        if(found && rect.tex_idx<(uint16_t)ktxs.size() && rect.tex_idx!=0) {
            // размеры другой текстуры — пересчитать UV
            ShapeRect r2;
            if(find_shape_rect(raw,(uint16_t)std::max(0,id),ktxs[rect.tex_idx].w,ktxs[rect.tex_idx].h,r2)) rect=r2;
        }
        if(!found || rect.tex_idx>=(uint16_t)ktxs.size()) { failures.push_back("shape "+std::to_string(id)+": не найден"); continue; }
        int pw,ph,ch;
        uint8_t* ld=stbi_load(it.second.c_str(),&pw,&ph,&ch,4);
        if(!ld) { failures.push_back("shape "+std::to_string(id)+": не открывается PNG"); continue; }
        std::vector<uint8_t> input(ld,ld+(size_t)pw*ph*4);
        stbi_image_free(ld);
        KTXInfo& k=ktxs[rect.tex_idx];
        if(patch_rect_polys(k.rgba,k.w,k.h,input,pw,ph,rect)) { k.dirty=true; applied++; }
        else failures.push_back("shape "+std::to_string(id)+": пустая область");
    }
    if(applied==0) {
        std::string r="Error: ничего не заменено";
        for(auto& f:failures) r+="\n  "+f;
        return r;
    }

    std::vector<uint8_t> new_raw=raw;
    for(auto& k:ktxs) {
        if(!k.dirty) continue;
        std::vector<uint8_t> enc;
        if(!astc_encode(k.rgba.data(),k.w,k.h,k.bw,k.bh,enc)) return "Error: ASTC encode failed";
        if(enc.size()!=k.image_size) return "Error: encoded KTX size changed unexpectedly";
        memcpy(new_raw.data()+k.image_start,enc.data(),enc.size());
    }

    std::vector<uint8_t> sc_hdr(sc_data.begin(),sc_data.begin()+hdr.comp.offset);
    if(sc_hdr.size()>=16) {
        uint8_t digest[16]; compute_md5(new_raw.data(),new_raw.size(),digest);
        memcpy(sc_hdr.data()+sc_hdr.size()-16,digest,16);
    }
    std::vector<uint8_t> comp;
    try { comp=sc_compress(new_raw,hdr.comp.kind); }
    catch(std::exception& e) { return std::string("Error: compress: ")+e.what(); }
    sc_hdr.insert(sc_hdr.end(),comp.begin(),comp.end());

    std::string stem=sc_basename_no_ext(orig_sc);
    if(stem.size()<4 || stem.substr(stem.size()-4)!="_tex") stem+="_tex";
    { struct stat st; std::string cand=stem; int n=2;
      while(stat((out_dir+"/"+cand+".sc").c_str(),&st)==0) cand=stem+"_"+std::to_string(n++);
      stem=cand; }
    std::string out_path=out_dir+"/"+stem+".sc";
    if(!sc_write_file(out_path,sc_hdr)) return "Error: write failed: "+out_path;

    std::ostringstream r;
    r<<"OK: batch SC\n";
    r<<"  Applied: "<<applied<<" of "<<items.size()<<"\n";
    for(size_t i=0;i<failures.size()&&i<5;i++) r<<"  ! "<<failures[i]<<"\n";
    r<<"  Output: "<<stem<<".sc\n";
    r<<"  Dir: "<<out_dir;
    return r.str();
}

// ===================== Экспортнеймы SC (v0) =====================
#include <tuple>
#include <set>
#include <map>

struct E1State {
    std::vector<uint8_t> raw;
    size_t sec_start = 0;   // позиция ec (u16)
    size_t sec_end = 0;     // конец таблицы имён
    std::vector<uint16_t> ids;
    std::vector<std::string> names;
};

static bool e1_parse(const std::vector<uint8_t>& raw, E1State& s, std::string& err) {
    if (raw.size() < 19) { err = "файл слишком мал"; return false; }
    size_t p = 17;
    uint16_t ec; memcpy(&ec, raw.data() + p, 2); p += 2;
    if (ec > 10000) { err = "некорректная таблица экспортнеймов"; return false; }
    s.sec_start = 17;
    s.ids.resize(ec);
    if (p + (size_t)ec * 2 > raw.size()) { err = "обрыв данных"; return false; }
    for (uint16_t i = 0; i < ec; i++) { memcpy(&s.ids[i], raw.data() + p, 2); p += 2; }
    s.names.clear();
    for (uint16_t i = 0; i < ec; i++) {
        if (p >= raw.size()) { err = "обрыв данных"; return false; }
        uint8_t n = raw[p++];
        if (n == 0xFF) { err = "неподдерживаемая длина имени"; return false; }
        if (p + n > raw.size()) { err = "обрыв данных"; return false; }
        s.names.push_back(std::string((const char*)raw.data() + p, n));
        p += n;
    }
    s.sec_end = p;
    s.raw = raw;
    return true;
}

static std::set<int> e1_movieclip_ids(const std::vector<uint8_t>& raw, size_t p) {
    std::set<int> ids;
    while (p + 5 <= raw.size()) {
        uint8_t tag = raw[p++];
        int32_t len; memcpy(&len, raw.data() + p, 4); p += 4;
        if (tag == 0) break;
        if (len < 0 || p + (size_t)len > raw.size()) break;
        if ((tag == 3 || tag == 10 || tag == 12 || tag == 14 || tag == 35 || tag == 49) && len >= 2) {
            uint16_t id; memcpy(&id, raw.data() + p, 2); ids.insert(id);
        }
        p += (size_t)len;
    }
    return ids;
}

std::string sc1_exports_list(const std::string& path) {
    std::vector<uint8_t> sc_data;
    if (!sc_read_file(path, sc_data)) return "Error: cannot read SC: " + path;
    ScHeader hdr = sc_parse_header(sc_data);
    if (!hdr.valid) return "Error: not an SC file";
    std::vector<uint8_t> raw;
    try { raw = sc_decompress(sc_data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: decompress: ") + e.what(); }
    E1State s; std::string err;
    if (!e1_parse(raw, s, err)) return "Error: " + err;
    std::string out;
    for (size_t i = 0; i < s.ids.size(); i++)
        out += std::to_string(s.ids[i]) + "\t" + s.names[i] + "\n";
    return out;
}

/* Список MovieClip'ов SC1: строки "id\tэкспортнейм\tкадры" (имя пустое, если не экспортируется). */
std::string sc1_movieclips_list(const std::string& path) {
    std::vector<uint8_t> sc_data;
    if (!sc_read_file(path, sc_data)) return "Error: cannot read SC: " + path;
    ScHeader hdr = sc_parse_header(sc_data);
    if (!hdr.valid) return "Error: not an SC file";
    std::vector<uint8_t> raw;
    try { raw = sc_decompress(sc_data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: decompress: ") + e.what(); }
    E1State s; std::string err;
    if (!e1_parse(raw, s, err)) return "Error: " + err;
    std::map<int, std::string> names;
    for (size_t i = 0; i < s.ids.size(); i++) names[s.ids[i]] = s.names[i];
    std::string out;
    size_t p = s.sec_end;
    while (p + 5 <= raw.size()) {
        uint8_t tag = raw[p++];
        int32_t len; memcpy(&len, raw.data() + p, 4); p += 4;
        if (tag == 0) break;
        if (len < 0 || p + (size_t)len > raw.size()) break;
        if ((tag == 3 || tag == 10 || tag == 12 || tag == 14 || tag == 35 || tag == 49) && len >= 2) {
            uint16_t id; memcpy(&id, raw.data() + p, 2);
            int frames = 0;
            if (len >= 5) { uint16_t fc; memcpy(&fc, raw.data() + p + 3, 2); frames = fc; }
            auto it = names.find(id);
            out += std::to_string(id) + "\t" + (it != names.end() ? it->second : std::string()) + "\t" + std::to_string(frames) + "\n";
        }
        p += (size_t)len;
    }
    return out;
}

std::string sc1_exports_edit(const std::vector<std::tuple<char,int,int,std::string>>& ops,
                             const std::string& orig_sc, const std::string& out_dir) {
    std::vector<uint8_t> sc_data;
    if (!sc_read_file(orig_sc, sc_data)) return "Error: cannot read SC: " + orig_sc;
    ScHeader hdr = sc_parse_header(sc_data);
    if (!hdr.valid) return "Error: not an SC file";
    std::vector<uint8_t> raw;
    try { raw = sc_decompress(sc_data, hdr.comp); }
    catch (std::exception& e) { return std::string("Error: decompress: ") + e.what(); }
    E1State s; std::string err;
    if (!e1_parse(raw, s, err)) return "Error: " + err;

    std::set<int> mc = e1_movieclip_ids(raw, s.sec_end);
    int renamed = 0, deleted = 0, added = 0;
    std::vector<bool> del(s.ids.size(), false);
    std::vector<uint16_t> ids = s.ids;
    std::vector<std::string> names = s.names;

    auto bad_name = [](const std::string& n) { return n.empty() || n.size() > 254; };
    for (auto& op : ops) {
        char k = std::get<0>(op); int idx = std::get<1>(op);
        if (k == 'R') {
            if (idx < 0 || idx >= (int)names.size() || del[idx]) return "Error: неверный индекс";
            if (bad_name(std::get<3>(op))) return "Error: некорректное имя";
            if (names[idx] != std::get<3>(op)) { names[idx] = std::get<3>(op); renamed++; }
        } else if (k == 'D') {
            if (idx < 0 || idx >= (int)names.size() || del[idx]) return "Error: неверный индекс";
            del[idx] = true; deleted++;
        } else if (k == 'A') {
            int oid = std::get<2>(op);
            if (oid < 0 || oid > 65535) return "Error: неверный ID объекта " + std::to_string(oid);
            if (!mc.count(oid)) return "Error: MovieClip с ID " + std::to_string(oid) + " не найден";
            if (bad_name(std::get<3>(op))) return "Error: некорректное имя";
            ids.push_back((uint16_t)oid); names.push_back(std::get<3>(op)); del.push_back(false); added++;
        }
    }
    if (!renamed && !deleted && !added) return "Error: нет изменений";

    std::vector<uint16_t> fids; std::vector<std::string> fnames;
    std::set<std::string> seen;
    for (size_t i = 0; i < ids.size(); i++) {
        if (del[i]) continue;
        if (!seen.insert(names[i]).second) return "Error: имя \"" + names[i] + "\" уже используется";
        fids.push_back(ids[i]); fnames.push_back(names[i]);
    }
    if (fids.size() > 10000) return "Error: слишком много экспортнеймов";

    std::vector<uint8_t> nr(raw.begin(), raw.begin() + 17);
    uint16_t ec = (uint16_t)fids.size();
    nr.insert(nr.end(), (uint8_t*)&ec, (uint8_t*)&ec + 2);
    for (auto id : fids) nr.insert(nr.end(), (uint8_t*)&id, (uint8_t*)&id + 2);
    for (auto& n : fnames) { nr.push_back((uint8_t)n.size()); nr.insert(nr.end(), n.begin(), n.end()); }
    nr.insert(nr.end(), raw.begin() + s.sec_end, raw.end());

    std::vector<uint8_t> sc_hdr(sc_data.begin(), sc_data.begin() + hdr.comp.offset);
    if (sc_hdr.size() >= 16) {
        uint8_t digest[16]; compute_md5(nr.data(), nr.size(), digest);
        memcpy(sc_hdr.data() + sc_hdr.size() - 16, digest, 16);
    }
    std::vector<uint8_t> comp;
    try { comp = sc_compress(nr, hdr.comp.kind); }
    catch (std::exception& e) { return std::string("Error: compress: ") + e.what(); }
    sc_hdr.insert(sc_hdr.end(), comp.begin(), comp.end());

    std::string stem = sc_basename_no_ext(orig_sc);
    if (stem.size() < 4 || stem.substr(stem.size() - 4) != "_exp") stem += "_exp";
    { struct stat st; std::string cand = stem; int n = 2;
      while (stat((out_dir + "/" + cand + ".sc").c_str(), &st) == 0) cand = stem + "_" + std::to_string(n++);
      stem = cand; }
    std::string out_path = out_dir + "/" + stem + ".sc";
    if (!sc_write_file(out_path, sc_hdr)) return "Error: write failed: " + out_path;

    std::ostringstream r;
    r << "OK: export names (SC)\n";
    if (renamed) r << "  Renamed: " << renamed << "\n";
    if (deleted) r << "  Deleted: " << deleted << "\n";
    if (added) r << "  Added: " << added << "\n";
    r << "  Total: " << fids.size() << "\n  Output: " << stem << ".sc\n  Dir: " << out_dir;
    return r.str();
}
