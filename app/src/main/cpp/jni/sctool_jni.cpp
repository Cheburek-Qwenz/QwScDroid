/**
 * sctool_jni.cpp v2
 * Пакет задаётся через -DSCTOOL_JNI_CLASS в Android.mk
 */
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <fstream>
#include <utility>
#include <tuple>
#include "sc_core.h"

static std::string jstr(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r(c); e->ReleaseStringUTFChars(s, c);
    return r;
}
static jstring toj(JNIEnv* e, const std::string& s) { return e->NewStringUTF(s.c_str()); }

#ifndef SCTOOL_JNI_CLASS
#define SCTOOL_JNI_CLASS Java_com_compose_sctool_ScProcessor
#endif
#define _CAT(a,b) a##b
#define CAT(a,b) _CAT(a,b)
#define FN(name) CAT(SCTOOL_JNI_CLASS, name)

// Forward declarations
std::string sc_info      (const std::string&);
std::string sc_split     (const std::string&, const std::string&);
std::string sc_join      (const std::string&, const std::string&, const std::string&);
std::string sc_extract_ktx(const std::string&, int, const std::string&);
std::string sc_downgrade (const std::string&, const std::string&);
std::string ktx2png      (const std::string&, const std::string&);
std::string png2ktx      (const std::string&, const std::string&, const std::string&);
std::string png2sc       (const std::string&, const std::string&, int, int, const std::string&);
std::string sc2_png2sc   (const std::string&, const std::string&, int, const std::string&);
std::string png2sc_batch (const std::vector<std::pair<int,std::string>>&, const std::string&, const std::string&);
std::string sc2_png2sc_batch(const std::vector<std::pair<int,std::string>>&, const std::string&, const std::string&);
std::string sc1_exports_list(const std::string&);
std::string sc1_exports_edit(const std::vector<std::tuple<char,int,int,std::string>>&, const std::string&, const std::string&);
std::string sc2_exports_list(const std::string&);
std::string sc1_movieclips_list(const std::string&);
std::string sc2_movieclips_list(const std::string&);
std::string sc2_exports_edit(const std::vector<std::tuple<char,int,int,std::string>>&, const std::string&, const std::string&);
std::string gif2sc_impl  (const std::string&, const std::string&, int, int, int, const std::string&, double, int);
std::string sc2gif_impl  (const std::string&, const std::string&, int, int, const std::string&);
std::string sctx2png     (const std::string&, const std::string&);
std::string png2sctx     (const std::string&, const std::string&);
std::string cut_sprites  (const std::string&, const std::string&, const std::string&);
std::string sc2png       (const std::string&, int, const std::string&);
std::string sc_exports   (const std::string&, const std::string&);
std::string inject_preview(const std::string&, const std::string&);
std::string inject_put   (const std::string&, const std::string&, const std::string&, const std::string&);
std::string inject_list  (const std::string&);
std::string anim2static  (const std::string&, const std::string&);
std::string build_sc     (const std::string&, const std::string&, const std::string&, const std::string&);
std::string mc_copy      (const std::string&, const std::string&, const std::string&, const std::string&);

extern "C" {

JNIEXPORT jstring JNICALL FN(_nativeScInfo)(JNIEnv* e, jclass, jstring a)
    { return toj(e, sc_info(jstr(e,a))); }

JNIEXPORT jstring JNICALL FN(_nativeSplitSc)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, sc_split(jstr(e,a), jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativeJoinSc)(JNIEnv* e, jclass, jstring a, jstring b, jstring c)
    { return toj(e, sc_join(jstr(e,a), jstr(e,b), jstr(e,c))); }

JNIEXPORT jstring JNICALL FN(_nativeExtractKtx)(JNIEnv* e, jclass, jstring a, jint idx, jstring b)
    { return toj(e, sc_extract_ktx(jstr(e,a), (int)idx, jstr(e,b))); }

// nativeDowngrade и nativeDowngradeV реализованы в downgrade_jni.cpp

JNIEXPORT jstring JNICALL FN(_nativeKtx2Png)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, ktx2png(jstr(e,a), jstr(e,b))); }

// block теперь передаётся как строка "8x8"
JNIEXPORT jstring JNICALL FN(_nativePng2Ktx)(JNIEnv* e, jclass, jstring png, jstring block, jstring out)
    { return toj(e, png2ktx(jstr(e,png), jstr(e,block), jstr(e,out))); }

JNIEXPORT jstring JNICALL FN(_nativePng2Sc)(JNIEnv* e, jclass, jstring png, jstring orig, jint idx, jstring rot, jstring out) {

    std::string orig_path = jstr(e, orig);
    std::string png_path  = jstr(e, png);
    std::string out_dir   = jstr(e, out);

    /*
     * SC1 vs SC2 (v5/v6) — совершенно разные бинарные
     * форматы, разные пути замены спрайта.
     *
     * Версия закодирована в третьем байте файла
     * (сразу после магии "SC"), читаем напрямую
     * без полного sc_parse_header, чтобы не тянуть
     * лишние зависимости в этот тонкий JNI-слой.
     */
    std::vector<uint8_t> probe;
    if (sc_read_file(orig_path, probe) && probe.size() >= 3 &&
        probe[0] == 'S' && probe[1] == 'C' && probe[2] >= 5) {

        // SC2: rotate не поддерживается этим путём (нет
        // аналога в формате), параметр игнорируется.
        return toj(e, sc2_png2sc(png_path, orig_path, (int)idx, out_dir));
    }

    int r = 0;
    try { r = std::stoi(jstr(e, rot)); } catch (...) {}
    return toj(e, png2sc(png_path, orig_path, (int)idx, r, out_dir));
}


/*
 * Пакетная замена. listFile: строки "<shape_id>\t<путь к PNG>".
 * Вызов диспетчеризуется по версии так же, как _nativePng2Sc.
 */
JNIEXPORT jstring JNICALL FN(_nativePng2ScBatch)(JNIEnv* e, jclass, jstring list, jstring orig, jstring out) {
    std::string list_path = jstr(e, list);
    std::string orig_path = jstr(e, orig);
    std::string out_dir   = jstr(e, out);

    std::vector<std::pair<int,std::string>> items;
    {
        std::ifstream f(list_path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t t = line.find('\t');
            if (t == std::string::npos || t == 0) continue;
            int id = 0;
            try { id = std::stoi(line.substr(0, t)); } catch (...) { continue; }
            items.push_back({ id, line.substr(t + 1) });
        }
    }
    if (items.empty()) return toj(e, "Error: пустой список замен");

    std::vector<uint8_t> probe;
    if (sc_read_file(orig_path, probe) && probe.size() >= 3 &&
        probe[0] == 'S' && probe[1] == 'C' && probe[2] >= 5)
        return toj(e, sc2_png2sc_batch(items, orig_path, out_dir));

    return toj(e, png2sc_batch(items, orig_path, out_dir));
}


static bool is_sc2_file(const std::string& path) {
    std::vector<uint8_t> probe;
    return sc_read_file(path, probe) && probe.size() >= 3 &&
           probe[0] == 'S' && probe[1] == 'C' && probe[2] >= 5;
}

/* Список экспортнеймов: строки "objectId\tname". */
JNIEXPORT jstring JNICALL FN(_nativeGetExports)(JNIEnv* e, jclass, jstring path) {
    std::string p = jstr(e, path);
    return toj(e, is_sc2_file(p) ? sc2_exports_list(p) : sc1_exports_list(p));
}

/* Список MovieClip'ов: строки "id\tэкспортнейм\tкадры". */
JNIEXPORT jstring JNICALL FN(_nativeGetMovieClips)(JNIEnv* e, jclass, jstring path) {
    std::string p = jstr(e, path);
    return toj(e, is_sc2_file(p) ? sc2_movieclips_list(p) : sc1_movieclips_list(p));
}

/*
 * Правка экспортнеймов. opsFile: строки
 *   R<TAB>index<TAB>name  — переименование
 *   D<TAB>index           — удаление
 *   A<TAB>objectId<TAB>name — добавление
 */
JNIEXPORT jstring JNICALL FN(_nativeEditExports)(JNIEnv* e, jclass, jstring opsFile, jstring orig, jstring out) {
    std::string ops_path = jstr(e, opsFile);
    std::string orig_path = jstr(e, orig);
    std::string out_dir = jstr(e, out);
    std::vector<std::tuple<char,int,int,std::string>> ops;
    {
        std::ifstream f(ops_path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() < 3 || line[1] != '\t') continue;
            char k = line[0];
            std::string rest = line.substr(2);
            size_t t = rest.find('\t');
            try {
                if (k == 'R' && t != std::string::npos)
                    ops.push_back(std::make_tuple('R', std::stoi(rest.substr(0, t)), 0, rest.substr(t + 1)));
                else if (k == 'D')
                    ops.push_back(std::make_tuple('D', std::stoi(rest), 0, std::string()));
                else if (k == 'A' && t != std::string::npos)
                    ops.push_back(std::make_tuple('A', 0, std::stoi(rest.substr(0, t)), rest.substr(t + 1)));
            } catch (...) {}
        }
    }
    if (ops.empty()) return toj(e, "Error: нет изменений");
    return toj(e, is_sc2_file(orig_path) ? sc2_exports_edit(ops, orig_path, out_dir)
                                         : sc1_exports_edit(ops, orig_path, out_dir));
}


/* GIF -> SC2. bg — цвет заливки прозрачных мест, "RRGGBB" (пусто = чёрный). */
JNIEXPORT jstring JNICALL FN(_nativeGif2Sc)(JNIEnv* e, jclass, jstring gif, jstring name, jint fps,
                                            jint screen, jstring bg, jint texFormat, jstring out) {
    std::string b = jstr(e, bg);
    int rgb = -1;
    if (b.size() == 6) { try { rgb = std::stoi(b, nullptr, 16); } catch (...) { rgb = -1; } }
    return toj(e, gif2sc_impl(jstr(e, gif), jstr(e, name), (int)fps, (int)screen, rgb, jstr(e, out),
                              texFormat == 1 ? 400e6 : 320e6, (int)texFormat));
}

/* SC2 -> GIF. name — экспортнейм MovieClip (пусто = самый длинный), maxSide — большая сторона в px. */
JNIEXPORT jstring JNICALL FN(_nativeSc2Gif)(JNIEnv* e, jclass, jstring sc, jstring name, jint maxSide,
                                            jint fps, jstring out) {
    return toj(e, sc2gif_impl(jstr(e, sc), jstr(e, name), (int)maxSide, (int)fps, jstr(e, out)));
}

JNIEXPORT jstring JNICALL FN(_nativeSctx2Png)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, sctx2png(jstr(e,a), jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativePng2Sctx)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, png2sctx(jstr(e,a), jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativeCutSprites)(JNIEnv* e, jclass, jstring a, jstring b, jstring c)
    { return toj(e, cut_sprites(jstr(e,a), jstr(e,b), jstr(e,c))); }

JNIEXPORT jstring JNICALL FN(_nativeSc2Png)(JNIEnv* e, jclass, jstring a, jint idx, jstring b)
    { return toj(e, sc2png(jstr(e,a), (int)idx, jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativeExports)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, sc_exports(jstr(e,a), jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativeInjectPreview)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, inject_preview(jstr(e,a), jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativeInjectPut)(JNIEnv* e, jclass, jstring a, jstring b, jstring name, jstring out)
    { return toj(e, inject_put(jstr(e,a), jstr(e,b), jstr(e,name), jstr(e,out))); }

JNIEXPORT jstring JNICALL FN(_nativeInjectList)(JNIEnv* e, jclass, jstring a)
    { return toj(e, inject_list(jstr(e,a))); }

JNIEXPORT jstring JNICALL FN(_nativeAnim2Static)(JNIEnv* e, jclass, jstring a, jstring b)
    { return toj(e, anim2static(jstr(e,a), jstr(e,b))); }

JNIEXPORT jstring JNICALL FN(_nativeBuildSc)(JNIEnv* e, jclass, jstring paths, jstring names, jstring block, jstring out)
    { return toj(e, build_sc(jstr(e,paths), jstr(e,names), jstr(e,block), jstr(e,out))); }

JNIEXPORT jstring JNICALL FN(_nativeMcCopy)(JNIEnv* e, jclass, jstring src, jstring dst, jstring names, jstring out)
    { return toj(e, mc_copy(jstr(e,src), jstr(e,dst), jstr(e,names), jstr(e,out))); }

} // extern "C"
