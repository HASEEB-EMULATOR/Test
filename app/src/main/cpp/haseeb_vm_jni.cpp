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

jint AsJint(haseeb::vm::VmError err) {
    return static_cast<jint>(err);
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_setPath(JNIEnv* env, jobject /* thiz */, jstring dir) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().SetPath(JStringToStd(env, dir)));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_setNativeLibDir(JNIEnv* env, jobject /* thiz */, jstring dir) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().SetNativeLibDir(JStringToStd(env, dir)));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_loadBootImage(JNIEnv* env, jobject /* thiz */, jstring path) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().LoadBootImage(JStringToStd(env, path)));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_loadSystemImage(JNIEnv* env, jobject /* thiz */, jstring path) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().LoadSystemImage(JStringToStd(env, path)));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_start(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint api,
    jint width,
    jint height,
    jint dpi,
    jint ram_mb
) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().Start(api, width, height, dpi, ram_mb));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_attachSurface(JNIEnv* env, jobject /* thiz */, jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    return AsJint(haseeb::engine::EngineRuntime::Instance().AttachSurface(window));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_sendTouch(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint action,
    jint x,
    jint y,
    jint id
) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().SendTouch(action, x, y, id));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_sendKey(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint code,
    jboolean down
) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().SendKey(code, down == JNI_TRUE));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_sendText(JNIEnv* env, jobject /* thiz */, jstring text) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().SendText(JStringToStd(env, text)));
}

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_setTargetFps(JNIEnv* /* env */, jobject /* thiz */, jint fps) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().SetTargetFps(fps));
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

JNIEXPORT jint JNICALL
Java_com_haseeb_gaming_Engine_stop(JNIEnv* /* env */, jobject /* thiz */) {
    return AsJint(haseeb::engine::EngineRuntime::Instance().Stop());
}

} // extern "C"
