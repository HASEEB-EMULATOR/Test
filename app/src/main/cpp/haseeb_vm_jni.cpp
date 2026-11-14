#include "haseeb/engine_runtime.hpp"

#include <android/native_window_jni.h>
#include <jni.h>
#include <string>

namespace {

std::string JStringToStd(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out(p ? p : "");
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_setPath(JNIEnv* env, jobject /* thiz */, jstring path) {
    haseeb::engine::EngineRuntime::Instance().SetPath(JStringToStd(env, path));
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_setNativeLibDir(JNIEnv* env, jobject /* thiz */, jstring lib_dir) {
    haseeb::engine::EngineRuntime::Instance().SetNativeLibDir(JStringToStd(env, lib_dir));
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_configureGuestProps(
    JNIEnv* env,
    jobject /* thiz */,
    jstring model,
    jstring brand,
    jstring manufacturer,
    jstring device,
    jstring name,
    jstring marketname,
    jstring fingerprint,
    jboolean nav_bar_off,
    jboolean magisk_prompt_enabled,
    jint target_fps
) {
    haseeb::engine::GuestConfig cfg;
    cfg.model = JStringToStd(env, model);
    cfg.brand = JStringToStd(env, brand);
    cfg.manufacturer = JStringToStd(env, manufacturer);
    cfg.device = JStringToStd(env, device);
    cfg.name = JStringToStd(env, name);
    cfg.marketname = JStringToStd(env, marketname);
    cfg.fingerprint = JStringToStd(env, fingerprint);
    cfg.nav_bar_off = (nav_bar_off == JNI_TRUE);
    cfg.magisk_prompt_enabled = (magisk_prompt_enabled == JNI_TRUE);
    cfg.target_fps = target_fps;
    haseeb::engine::EngineRuntime::Instance().ConfigureGuestProps(cfg);
}

JNIEXPORT jboolean JNICALL
Java_com_haseeb_gaming_Engine_start(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint api,
    jint width,
    jint height,
    jint dpi,
    jint ram_mb
) {
    return haseeb::engine::EngineRuntime::Instance().Start(api, width, height, dpi, ram_mb)
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_attachSurface(JNIEnv* env, jobject /* thiz */, jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    haseeb::engine::EngineRuntime::Instance().AttachSurface(window);
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_sendTouch(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint action,
    jint x,
    jint y,
    jint id
) {
    haseeb::engine::EngineRuntime::Instance().SendTouch(action, x, y, id);
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_sendKey(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint code,
    jboolean down
) {
    haseeb::engine::EngineRuntime::Instance().SendKey(code, down == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_sendText(JNIEnv* env, jobject /* thiz */, jstring text) {
    haseeb::engine::EngineRuntime::Instance().SendText(JStringToStd(env, text));
}

JNIEXPORT jstring JNICALL
Java_com_haseeb_gaming_Engine_readLog(JNIEnv* env, jobject /* thiz */, jint max_bytes) {
    std::string log = haseeb::engine::EngineRuntime::Instance().ReadLog(max_bytes);
    return env->NewStringUTF(log.c_str());
}

JNIEXPORT jfloat JNICALL
Java_com_haseeb_gaming_Engine_getGuestFps(JNIEnv* /* env */, jobject /* thiz */) {
    return haseeb::engine::EngineRuntime::Instance().GetGuestFps();
}

JNIEXPORT jboolean JNICALL
Java_com_haseeb_gaming_Engine_isRunning(JNIEnv* /* env */, jobject /* thiz */) {
    return haseeb::engine::EngineRuntime::Instance().IsRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_haseeb_gaming_Engine_stop(JNIEnv* /* env */, jobject /* thiz */) {
    haseeb::engine::EngineRuntime::Instance().Stop();
}

} // extern "C"
