#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "common.hpp"
#include "raylib.h"

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <EGL/egl.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <time.h>

extern "C" struct android_app* GetAndroidApp(void);  // raylib, rcore_android.c
#endif

namespace gh::platform {

namespace fs = std::filesystem;

#if defined(__ANDROID__)

namespace {

void logInfo(const std::string& s) { __android_log_print(ANDROID_LOG_INFO, "GodsHand", "%s", s.c_str()); }

std::atomic<bool> g_background{false}, g_resumed{false};
std::atomic<float> g_volume{0.8f};
void (*g_raylibCmd)(android_app*, int32_t) = nullptr;
int32_t (*g_raylibInput)(android_app*, AInputEvent*) = nullptr;
std::vector<TouchEvent> g_touches;  // filled and read on the game thread

// Touch events carry their time in the CLOCK_MONOTONIC base: move it to nowSec().
double eventTime(int64_t ns) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = nowSec();
    const double ago = double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9 - double(ns) * 1e-9;
    return now - std::clamp(ago, 0.0, 1.0);
}

int32_t onInputEvent(android_app* app, AInputEvent* ev) {
    const int32_t handled = g_raylibInput ? g_raylibInput(app, ev) : 0;
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return handled;
    const int32_t src = AInputEvent_getSource(ev);
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN && (src & AINPUT_SOURCE_MOUSE) != AINPUT_SOURCE_MOUSE)
        return handled;
    const int32_t action = AMotionEvent_getAction(ev);
    const size_t idx = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const double t = eventTime(AMotionEvent_getEventTime(ev));
    auto push = [&](TouchEvent::Kind k, size_t i) {
        g_touches.push_back({k, int(AMotionEvent_getPointerId(ev, i)), AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i), t});
    };
    switch (action & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: push(TouchEvent::Down, idx); break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: push(TouchEvent::Up, idx); break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); ++i) push(TouchEvent::Move, i);
            break;
        case AMOTION_EVENT_ACTION_CANCEL: g_touches.push_back({TouchEvent::Cancel, -1, 0, 0, t}); break;
        default: break;
    }
    if (g_touches.size() > 4096) g_touches.erase(g_touches.begin(), g_touches.begin() + 2048);
    return handled;
}

// raylib opens files through the APK's assets and then <internal data path>/<name>,
// even for absolute paths, so fonts and audio in the game folder would not load:
// plain stdio instead.
unsigned char* loadFileData(const char* fileName, int* size) {
    *size = 0;
    FILE* f = std::fopen(fileName, "rb");
    if (!f) return nullptr;
    std::vector<unsigned char> buf;
    unsigned char tmp[1 << 16];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(f);
    auto* out = static_cast<unsigned char*>(std::malloc(std::max<size_t>(1, buf.size())));
    if (!out) return nullptr;
    if (!buf.empty()) std::memcpy(out, buf.data(), buf.size());
    *size = int(buf.size());
    return out;
}

char* loadFileText(const char* fileName) {
    int size = 0;
    unsigned char* d = loadFileData(fileName, &size);
    if (!d) return nullptr;
    auto* text = static_cast<char*>(std::realloc(d, size_t(size) + 1));
    if (!text) {
        std::free(d);
        return nullptr;
    }
    text[size] = 0;
    return text;
}

bool saveFileData(const char* fileName, void* data, int size) {
    FILE* f = std::fopen(fileName, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size_t(size), f) == size_t(size);
    return std::fclose(f) == 0 && ok;
}

bool saveFileText(const char* fileName, char* text) { return saveFileData(fileName, text, int(std::strlen(text))); }

void onAppCmd(android_app* app, int32_t cmd) {
    if (g_raylibCmd) g_raylibCmd(app, cmd);
    switch (cmd) {
        case APP_CMD_LOST_FOCUS:
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
            if (!g_background.exchange(true)) SetMasterVolume(0.0f);
            break;
        case APP_CMD_GAINED_FOCUS:
            if (g_background.exchange(false)) {
                SetMasterVolume(g_volume.load());
                g_resumed = true;
            }
            break;
        default: break;
    }
}

// Copies one folder of the APK's assets (files only) into dest.
void copyAssetDir(AAssetManager* mgr, const std::string& dir, const std::string& dest) {
    AAssetDir* d = AAssetManager_openDir(mgr, dir.c_str());
    if (!d) return;
    std::error_code ec;
    fs::create_directories(dest, ec);
    while (const char* name = AAssetDir_getNextFileName(d)) {
        const std::string rel = dir + "/" + name;
        AAsset* a = AAssetManager_open(mgr, rel.c_str(), AASSET_MODE_STREAMING);
        if (!a) continue;
        const off_t len = AAsset_getLength(a);
        const std::string out = dest + name;
        std::error_code e2;
        if (fs::exists(out, e2) && off_t(fs::file_size(out, e2)) == len) {
            AAsset_close(a);
            continue;
        }
        if (FILE* f = std::fopen((out + ".part").c_str(), "wb")) {
            char buf[1 << 16];
            int n;
            bool ok = true;
            while ((n = AAsset_read(a, buf, sizeof buf)) > 0) ok = ok && std::fwrite(buf, 1, size_t(n), f) == size_t(n);
            ok = std::fclose(f) == 0 && ok;
            if (ok) fs::rename(out + ".part", out, e2);
            logInfo("copied " + rel + (ok && !e2 ? "" : " (failed)"));
        }
        AAsset_close(a);
    }
    AAssetDir_close(d);
}

// JNI helpers for the downloader.
struct Jni {
    JNIEnv* env = nullptr;
    bool attached = false;
    JavaVM* vm = nullptr;
    Jni() {
        android_app* app = GetAndroidApp();
        if (!app || !app->activity) return;
        vm = app->activity->vm;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
            else attached = true;
        }
    }
    ~Jni() {
        if (attached) vm->DetachCurrentThread();
    }
    // True (and the exception cleared) if the last call threw.
    bool failed(std::string& err, const char* what) {
        if (!env->ExceptionCheck()) return false;
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        err = what;
        if (ex) {
            jclass c = env->FindClass("java/lang/Throwable");
            jmethodID m = c ? env->GetMethodID(c, "toString", "()Ljava/lang/String;") : nullptr;
            if (m) {
                auto s = static_cast<jstring>(env->CallObjectMethod(ex, m));
                if (!env->ExceptionCheck() && s) {
                    const char* u = env->GetStringUTFChars(s, nullptr);
                    err += std::string(": ") + u;
                    env->ReleaseStringUTFChars(s, u);
                }
                env->ExceptionClear();
            }
        }
        return true;
    }
};

// GameActivity (android/java): a static int field and an instance method.
jclass activityClass(JNIEnv* env) {
    android_app* app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->clazz) return nullptr;
    return env->GetObjectClass(app->activity->clazz);
}

int staticInt(const char* name, int fallback) {
    Jni j;
    if (!j.env) return fallback;
    jclass cls = activityClass(j.env);
    if (!cls) return fallback;
    jfieldID f = j.env->GetStaticFieldID(cls, name, "I");
    int v = fallback;
    if (f) v = j.env->GetStaticIntField(cls, f);
    j.env->ExceptionClear();
    j.env->DeleteLocalRef(cls);
    return v;
}

}  // namespace

bool touchFirst() { return true; }
bool touchEvents() { return true; }

void takeTouchEvents(std::vector<TouchEvent>& out) {
    out.insert(out.end(), g_touches.begin(), g_touches.end());
    g_touches.clear();
}

void log(const std::string& s) { logInfo(s); }

bool pickModZips() {
    Jni j;
    if (!j.env) return false;
    jclass cls = activityClass(j.env);
    if (!cls) return false;
    jmethodID m = j.env->GetMethodID(cls, "pickModZips", "()V");
    bool ok = false;
    if (m) {
        j.env->CallVoidMethod(GetAndroidApp()->activity->clazz, m);
        std::string err;
        ok = !j.failed(err, "file picker");
    }
    j.env->ExceptionClear();
    j.env->DeleteLocalRef(cls);
    return ok;
}

std::string pickFolder(const std::string&) {
    Jni j;
    if (!j.env) return {};
    jclass cls = activityClass(j.env);
    if (!cls) return {};
    if (jmethodID m = j.env->GetMethodID(cls, "pickPsychFolder", "()V")) {
        j.env->CallVoidMethod(GetAndroidApp()->activity->clazz, m);
        std::string err;
        j.failed(err, "folder picker");
    }
    j.env->ExceptionClear();
    j.env->DeleteLocalRef(cls);
    return {};
}

Import importStatus(int* count) {
    const int s = staticInt("importState", 0);
    if (count) *count = staticInt("importCount", 0);
    return s >= 0 && s <= 4 ? Import(s) : Import::Idle;
}

void resetImport() {
    Jni j;
    if (!j.env) return;
    jclass cls = activityClass(j.env);
    if (!cls) return;
    if (jfieldID f = j.env->GetStaticFieldID(cls, "importState", "I")) j.env->SetStaticIntField(cls, f, 0);
    j.env->ExceptionClear();
    j.env->DeleteLocalRef(cls);
}

std::string dataDir() {
    android_app* app = GetAndroidApp();
    const char* p = app && app->activity ? app->activity->externalDataPath : nullptr;
    std::string s = p ? p : "";
    std::error_code ec;
    if (!s.empty()) fs::create_directories(s, ec);
    if (s.empty() || ec) s = app && app->activity && app->activity->internalDataPath ? app->activity->internalDataPath : ".";
    if (s.back() != '/') s += '/';
    logInfo("data folder: " + s);
    return s;
}

std::string userDir() { return dataDir(); }

void prepareDataDir(const std::string& base) {
    std::error_code ec;
    for (const char* d : {"mods", "songs", "replays", "saves", "cache"}) fs::create_directories(base + d, ec);
    android_app* app = GetAndroidApp();
    if (!app || !app->activity || !app->activity->assetManager) return;
    copyAssetDir(app->activity->assetManager, "assets/fonts", base + "assets/fonts/");
    copyAssetDir(app->activity->assetManager, "songs", base + "songs/");
}

void setVsync(bool on) {
    const EGLDisplay d = eglGetCurrentDisplay();
    if (d != EGL_NO_DISPLAY) eglSwapInterval(d, on ? 1 : 0);
}

void afterWindowInit() {
    g_background = false;
    g_resumed = false;
    g_touches.clear();
    SetLoadFileDataCallback(loadFileData);
    SetLoadFileTextCallback(loadFileText);
    SetSaveFileDataCallback(saveFileData);
    SetSaveFileTextCallback(saveFileText);
    android_app* app = GetAndroidApp();
    if (!app) return;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    if (app->onAppCmd != onAppCmd) {
        g_raylibCmd = app->onAppCmd;
        app->onAppCmd = onAppCmd;
    }
    if (app->onInputEvent != onInputEvent) {
        g_raylibInput = app->onInputEvent;
        app->onInputEvent = onInputEvent;
    }
}

bool inBackground() { return g_background.load(); }
bool takeResumed() { return g_resumed.exchange(false); }
void rememberVolume(float v) { g_volume = v; }

std::string displayPath(const std::string& path) {
    for (const char* prefix : {"/storage/emulated/0/", "/sdcard/"})
        if (path.rfind(prefix, 0) == 0) return path.substr(std::string(prefix).size());
    return path;
}

bool download(const std::string& url, const std::string& dest, std::string& err) {
    Jni j;
    JNIEnv* env = j.env;
    if (!env) { err = "no Java environment"; return false; }
    env->PushLocalFrame(64);
    bool ok = false;
    FILE* f = nullptr;
    jobject conn = nullptr, in = nullptr;
    jclass connCls = nullptr, inCls = nullptr;
    do {
        jclass urlCls = env->FindClass("java/net/URL");
        jmethodID ctor = urlCls ? env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V") : nullptr;
        if (!ctor || j.failed(err, "java.net.URL")) break;
        jobject u = env->NewObject(urlCls, ctor, env->NewStringUTF(url.c_str()));
        if (j.failed(err, "bad URL")) break;
        conn = env->CallObjectMethod(u, env->GetMethodID(urlCls, "openConnection", "()Ljava/net/URLConnection;"));
        if (j.failed(err, "cannot connect") || !conn) break;
        connCls = env->FindClass("java/net/HttpURLConnection");
        if (!connCls || j.failed(err, "HttpURLConnection")) break;
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "setConnectTimeout", "(I)V"), 20000);
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "setReadTimeout", "(I)V"), 60000);
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "setInstanceFollowRedirects", "(Z)V"), JNI_TRUE);
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"),
                            env->NewStringUTF("User-Agent"), env->NewStringUTF("GodsHand"));
        if (j.failed(err, "request setup")) break;
        const jint code = env->CallIntMethod(conn, env->GetMethodID(connCls, "getResponseCode", "()I"));
        if (j.failed(err, "no response")) break;
        if (code < 200 || code >= 300) {
            err = "HTTP " + std::to_string(code);
            break;
        }
        in = env->CallObjectMethod(conn, env->GetMethodID(connCls, "getInputStream", "()Ljava/io/InputStream;"));
        if (j.failed(err, "cannot read") || !in) break;
        inCls = env->FindClass("java/io/InputStream");
        jmethodID readM = env->GetMethodID(inCls, "read", "([B)I");
        jbyteArray buf = env->NewByteArray(1 << 16);
        f = std::fopen(dest.c_str(), "wb");
        if (!f) { err = "cannot write " + dest; break; }
        std::vector<jbyte> tmp(1 << 16);
        bool readOk = true;
        for (;;) {
            const jint n = env->CallIntMethod(in, readM, buf);
            if (j.failed(err, "download interrupted")) { readOk = false; break; }
            if (n < 0) break;
            if (n == 0) continue;
            env->GetByteArrayRegion(buf, 0, n, tmp.data());
            if (std::fwrite(tmp.data(), 1, size_t(n), f) != size_t(n)) { err = "disk full?"; readOk = false; break; }
        }
        ok = readOk;
    } while (false);
    if (f && std::fclose(f) != 0) ok = false;
    if (in && inCls) {
        env->CallVoidMethod(in, env->GetMethodID(inCls, "close", "()V"));
        env->ExceptionClear();
    }
    if (conn && connCls) {
        env->CallVoidMethod(conn, env->GetMethodID(connCls, "disconnect", "()V"));
        env->ExceptionClear();
    }
    env->PopLocalFrame(nullptr);
    if (!ok) {
        std::error_code ec;
        fs::remove(dest, ec);
        logInfo("download failed: " + url + " (" + err + ")");
    }
    return ok;
}

#else  // desktop

bool touchFirst() { return false; }
bool touchEvents() { return false; }
void takeTouchEvents(std::vector<TouchEvent>&) {}
void log(const std::string&) {}
std::string dataDir() { return ""; }
std::string userDir() {
#if defined(_WIN32)
    const wchar_t* app = _wgetenv(L"APPDATA");
    if (!app || !*app) return "";
    const std::u8string u = (fs::path(app) / L"GodsHand").u8string();
    return std::string(u.begin(), u.end()) + "/";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/Library/Application Support/GodsHand/" : std::string();
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg == '/') return std::string(xdg) + "/GodsHand/";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.local/share/GodsHand/" : std::string();
#endif
}
void prepareDataDir(const std::string&) {}
void afterWindowInit() {}
void setVsync(bool on) {
    if (on) SetWindowState(FLAG_VSYNC_HINT);
    else ClearWindowState(FLAG_VSYNC_HINT);
}
bool inBackground() { return false; }
bool takeResumed() { return false; }
void rememberVolume(float) {}
std::string displayPath(const std::string& path) { return path; }
bool download(const std::string&, const std::string&, std::string& err) {
    err = "not available on this platform";
    return false;
}
bool pickModZips() { return false; }
std::string pickFolder(const std::string& title) { return pickFolderNative(title); }
Import importStatus(int* count) {
    if (count) *count = 0;
    return Import::Idle;
}
void resetImport() {}

#endif

}  // namespace gh::platform
