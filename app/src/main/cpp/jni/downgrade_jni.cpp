#include <jni.h>
#include <dlfcn.h>
#include <string>
#include <android/log.h>

#define TAG "Downgrade"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef SCTOOL_JNI_CLASS
#define SCTOOL_JNI_CLASS Java_com_compose_sctool_ScProcessor
#endif
#define _CAT(a,b) a##b
#define CAT(a,b) _CAT(a,b)
#define FN(name) CAT(SCTOOL_JNI_CLASS, name)

typedef jstring (*DowngradeFn)(JNIEnv*, jobject, jstring, jstring, jfloat);

static void*       g_lib = nullptr;
static DowngradeFn g_fn  = nullptr;

static bool load_lib(JNIEnv* env) {
    if (g_fn) return true;

    jclass actClass = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(actClass, "currentApplication",
        "()Landroid/app/Application;");
    jobject app = env->CallStaticObjectMethod(actClass, curApp);
    jmethodID getAppInfo = env->GetMethodID(
        env->FindClass("android/content/Context"),
        "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject appInfo = env->CallObjectMethod(app, getAppInfo);
    jfieldID nativeLibField = env->GetFieldID(
        env->FindClass("android/content/pm/ApplicationInfo"),
        "nativeLibraryDir", "Ljava/lang/String;");
    jstring nativeLibDir = (jstring)env->GetObjectField(appInfo, nativeLibField);
    const char* dir = env->GetStringUTFChars(nativeLibDir, nullptr);
    std::string libPath = std::string(dir) + "/libScDowngrade.so";
    env->ReleaseStringUTFChars(nativeLibDir, dir);

    LOGI("Loading: %s", libPath.c_str());
    g_lib = dlopen(libPath.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!g_lib) { LOGE("dlopen: %s", dlerror()); return false; }

    g_fn = (DowngradeFn)dlsym(g_lib, "Java_com_scdowngrader_app_NativeLib_downgrade");
    if (!g_fn) { LOGE("dlsym: %s", dlerror()); dlclose(g_lib); g_lib=nullptr; return false; }

    LOGI("libScDowngrade loaded OK");
    return true;
}

extern "C" {

JNIEXPORT jstring JNICALL FN(_nativeDowngrade)(JNIEnv* env, jclass,
        jstring input, jstring version_str, jstring out_dir) {

    if (!load_lib(env))
        return env->NewStringUTF("Error: cannot load libScDowngrade.so");

    const char* in_c = env->GetStringUTFChars(input, nullptr);
    const char* vs_c = env->GetStringUTFChars(version_str, nullptr);
    const char* od_c = env->GetStringUTFChars(out_dir, nullptr);

    std::string in_str(in_c), vs_str(vs_c), od_str(od_c);
    env->ReleaseStringUTFChars(input, in_c);
    env->ReleaseStringUTFChars(version_str, vs_c);
    env->ReleaseStringUTFChars(out_dir, od_c);

    // Версия
    jfloat ver = -1.0f;
    if (vs_str != "auto") {
        try { ver = std::stof(vs_str); } catch (...) {}
    }

    // Выходной путь
    size_t slash = in_str.rfind('/');
    std::string fname = (slash == std::string::npos) ? in_str : in_str.substr(slash+1);
    size_t dot = fname.rfind('.');
    std::string stem = (dot == std::string::npos) ? fname : fname.substr(0, dot);
    std::string out_str = od_str + "/" + stem + "_dg.sc";

    jstring j_in  = env->NewStringUTF(in_str.c_str());
    jstring j_out = env->NewStringUTF(out_str.c_str());
    jstring result = g_fn(env, nullptr, j_in, j_out, ver);
    env->DeleteLocalRef(j_in);
    env->DeleteLocalRef(j_out);

    const char* res_c = result ? env->GetStringUTFChars(result, nullptr) : nullptr;
    std::string res_str = res_c ? res_c : "";
    if (res_c) env->ReleaseStringUTFChars(result, res_c);

    if (res_str.empty() || res_str.find("uccess") != std::string::npos || res_str.find("OK") != std::string::npos) {
        std::string ver_s = (ver < 0) ? "auto" : vs_str;
        return env->NewStringUTF(("OK: Downgraded (v" + ver_s + ")\n  Output: " + stem + "_dg.sc\n  Dir: " + od_str).c_str());
    }
    return env->NewStringUTF(("Error: " + res_str).c_str());
}

} // extern "C"
