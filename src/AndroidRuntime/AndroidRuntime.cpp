#ifdef __ANDROID__

#include "AndroidRuntime.h"
#include "../Engine.h"
#include "../../include/Platform/AssetSource.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Modularity::Platform {
    std::unique_ptr<AssetSource> MakeAndroidAssetSource(AAssetManager* mgr);
}

#include <cstdint>
#include <cstdlib>
#include <memory>

#define MODU_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "Modularity", __VA_ARGS__)
#define MODU_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "Modularity", __VA_ARGS__)
#define MODU_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Modularity", __VA_ARGS__)

namespace Modularity::AndroidRuntime {

namespace {

int   g_stdoutPipe[2] = {-1, -1};
int   g_stderrPipe[2] = {-1, -1};

void* PumpStdToLogcat(void* arg) {
    const int   fd     = static_cast<int>(reinterpret_cast<intptr_t>(arg) >> 1);
    const int   isErr  = static_cast<int>(reinterpret_cast<intptr_t>(arg) & 1);
    const char* tag    = isErr ? "Modularity.stderr" : "Modularity.stdout";
    const int   prio   = isErr ? ANDROID_LOG_WARN  : ANDROID_LOG_INFO;
    char        buf[512];
    ssize_t     n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        if (buf[n - 1] == '\n') --n;
        buf[n] = '\0';
        __android_log_print(prio, tag, "%s", buf);
    }
    return nullptr;
}

void RedirectStdioToLogcat() {
    if (g_stdoutPipe[0] >= 0) return;

    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    if (pipe(g_stdoutPipe) == 0) {
        dup2(g_stdoutPipe[1], STDOUT_FILENO);
        pthread_t t;
        pthread_create(&t, nullptr, PumpStdToLogcat,
                       reinterpret_cast<void*>(static_cast<intptr_t>(g_stdoutPipe[0]) << 1));
        pthread_detach(t);
    }
    if (pipe(g_stderrPipe) == 0) {
        dup2(g_stderrPipe[1], STDERR_FILENO);
        pthread_t t;
        pthread_create(&t, nullptr, PumpStdToLogcat,
                       reinterpret_cast<void*>((static_cast<intptr_t>(g_stderrPipe[0]) << 1) | 1));
        pthread_detach(t);
    }
}

struct RuntimeContext {
    android_app* app = nullptr;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig  config  = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    int surfaceWidth = 0;
    int surfaceHeight = 0;
    bool paused = false;

    static constexpr int kMaxTouchPointers = 10;
    struct TouchPointer {
        int32_t id = -1;
        float x = 0.0f;
        float y = 0.0f;
        bool active = false;
    };
    TouchPointer touches[kMaxTouchPointers];
    int touchCount = 0;

    float primaryX = 0.0f;
    float primaryY = 0.0f;

    struct PendingKey { int keyCode = 0; bool down = false; };
    std::deque<unsigned int> inputChars;
    std::deque<PendingKey> keyEvents;
    bool softKeyboardVisible = false;
};

RuntimeContext g_ctx;

std::mutex g_filePickerMutex;
std::vector<std::string> g_filePickerPaths;
std::string g_filePickerError;
bool g_filePickerPending = false;
bool g_filePickerResultReady = false;
bool g_filePickerCanceled = false;

const char* EglErrorString(EGLint err) {
    switch (err) {
        case EGL_SUCCESS:             return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:     return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:          return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:           return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:       return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG:          return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT:         return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:         return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH:           return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP:   return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW:   return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER:       return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE:         return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST:        return "EGL_CONTEXT_LOST";
        default:                      return "EGL_<unknown>";
    }
}

bool InitEGLDisplay() {
    if (g_ctx.display != EGL_NO_DISPLAY) return true;

    g_ctx.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_ctx.display == EGL_NO_DISPLAY) {
        MODU_LOGE("eglGetDisplay returned EGL_NO_DISPLAY");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_ctx.display, &major, &minor)) {
        MODU_LOGE("eglInitialize failed: %s", EglErrorString(eglGetError()));
        g_ctx.display = EGL_NO_DISPLAY;
        return false;
    }
    MODU_LOGI("EGL %d.%d initialized", major, minor);

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE
    };
    EGLint configCount = 0;
    if (!eglChooseConfig(g_ctx.display, configAttribs, &g_ctx.config, 1, &configCount) || configCount < 1) {
        MODU_LOGE("eglChooseConfig failed: %s", EglErrorString(eglGetError()));
        eglTerminate(g_ctx.display);
        g_ctx.display = EGL_NO_DISPLAY;
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    g_ctx.context = eglCreateContext(g_ctx.display, g_ctx.config, EGL_NO_CONTEXT, contextAttribs);
    if (g_ctx.context == EGL_NO_CONTEXT) {
        MODU_LOGE("eglCreateContext failed: %s", EglErrorString(eglGetError()));
        eglTerminate(g_ctx.display);
        g_ctx.display = EGL_NO_DISPLAY;
        g_ctx.config = nullptr;
        return false;
    }
    return true;
}

bool CreateEGLSurface(ANativeWindow* window) {
    if (!window || g_ctx.display == EGL_NO_DISPLAY || g_ctx.context == EGL_NO_CONTEXT) {
        return false;
    }

    EGLint nativeVisualId = 0;
    eglGetConfigAttrib(g_ctx.display, g_ctx.config, EGL_NATIVE_VISUAL_ID, &nativeVisualId);
    ANativeWindow_setBuffersGeometry(window, 0, 0, nativeVisualId);

    g_ctx.surface = eglCreateWindowSurface(g_ctx.display, g_ctx.config, window, nullptr);
    if (g_ctx.surface == EGL_NO_SURFACE) {
        MODU_LOGE("eglCreateWindowSurface failed: %s", EglErrorString(eglGetError()));
        return false;
    }

    if (!eglMakeCurrent(g_ctx.display, g_ctx.surface, g_ctx.surface, g_ctx.context)) {
        MODU_LOGE("eglMakeCurrent failed: %s", EglErrorString(eglGetError()));
        eglDestroySurface(g_ctx.display, g_ctx.surface);
        g_ctx.surface = EGL_NO_SURFACE;
        return false;
    }

    eglQuerySurface(g_ctx.display, g_ctx.surface, EGL_WIDTH,  &g_ctx.surfaceWidth);
    eglQuerySurface(g_ctx.display, g_ctx.surface, EGL_HEIGHT, &g_ctx.surfaceHeight);
    MODU_LOGI("EGL surface ready: %dx%d", g_ctx.surfaceWidth, g_ctx.surfaceHeight);
    MODU_LOGI("GL_VENDOR=%s",   glGetString(GL_VENDOR));
    MODU_LOGI("GL_RENDERER=%s", glGetString(GL_RENDERER));
    MODU_LOGI("GL_VERSION=%s",  glGetString(GL_VERSION));
    return true;
}

void DestroyEGLSurface() {
    if (g_ctx.display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(g_ctx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_ctx.surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_ctx.display, g_ctx.surface);
        g_ctx.surface = EGL_NO_SURFACE;
    }
    g_ctx.surfaceWidth = 0;
    g_ctx.surfaceHeight = 0;
}

void DestroyEGL() {
    DestroyEGLSurface();
    if (g_ctx.context != EGL_NO_CONTEXT) {
        eglDestroyContext(g_ctx.display, g_ctx.context);
        g_ctx.context = EGL_NO_CONTEXT;
    }
    if (g_ctx.display != EGL_NO_DISPLAY) {
        eglTerminate(g_ctx.display);
        g_ctx.display = EGL_NO_DISPLAY;
    }
    g_ctx.config = nullptr;
}

bool DrainEvents(int timeoutMs) {
    int events = 0;
    android_poll_source* source = nullptr;
    while (ALooper_pollOnce(timeoutMs, nullptr, &events,
                            reinterpret_cast<void**>(&source)) >= 0) {
        if (source) source->process(g_ctx.app, source);
        if (g_ctx.app && g_ctx.app->destroyRequested) return false;
        timeoutMs = 0;
    }
    return !(g_ctx.app && g_ctx.app->destroyRequested);
}

void HandleAppCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                if (!InitEGLDisplay() || !CreateEGLSurface(app->window)) {
                    MODU_LOGE("Failed to bring up EGL on INIT_WINDOW");
                }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            DestroyEGLSurface();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (g_ctx.surface != EGL_NO_SURFACE && g_ctx.display != EGL_NO_DISPLAY) {
                eglQuerySurface(g_ctx.display, g_ctx.surface, EGL_WIDTH,  &g_ctx.surfaceWidth);
                eglQuerySurface(g_ctx.display, g_ctx.surface, EGL_HEIGHT, &g_ctx.surfaceHeight);
                MODU_LOGI("Surface resized to %dx%d", g_ctx.surfaceWidth, g_ctx.surfaceHeight);
            }
            break;
        case APP_CMD_GAINED_FOCUS:
        case APP_CMD_RESUME:
            g_ctx.paused = false;
            break;
        case APP_CMD_LOST_FOCUS:
        case APP_CMD_PAUSE:
            g_ctx.paused = true;
            break;
        case APP_CMD_DESTROY:
            DestroyEGL();
            break;
        default: break;
    }
}

char AndroidKeycodeToChar(int32_t keyCode, bool shift) {
    if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z) {
        const char base = static_cast<char>('a' + (keyCode - AKEYCODE_A));
        return shift ? static_cast<char>(base - 32) : base;
    }
    if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9) {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[keyCode - AKEYCODE_0]
                     : static_cast<char>('0' + (keyCode - AKEYCODE_0));
    }
    switch (keyCode) {
        case AKEYCODE_SPACE:         return ' ';
        case AKEYCODE_COMMA:         return shift ? '<' : ',';
        case AKEYCODE_PERIOD:        return shift ? '>' : '.';
        case AKEYCODE_MINUS:         return shift ? '_' : '-';
        case AKEYCODE_EQUALS:        return shift ? '+' : '=';
        case AKEYCODE_SLASH:         return shift ? '?' : '/';
        case AKEYCODE_BACKSLASH:     return shift ? '|' : '\\';
        case AKEYCODE_SEMICOLON:     return shift ? ':' : ';';
        case AKEYCODE_APOSTROPHE:    return shift ? '"' : '\'';
        case AKEYCODE_LEFT_BRACKET:  return shift ? '{' : '[';
        case AKEYCODE_RIGHT_BRACKET: return shift ? '}' : ']';
        case AKEYCODE_GRAVE:         return shift ? '~' : '`';
        default:                     return 0;
    }
}

void ApplySoftKeyboard(android_app* app, bool show) {
    if (!app || !app->activity || !app->activity->vm || !app->activity->clazz) return;
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }

    jobject activity = app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jclass contextClass = env->FindClass("android/content/Context");
    jfieldID imsField = env->GetStaticFieldID(contextClass, "INPUT_METHOD_SERVICE", "Ljava/lang/String;");
    jobject imsName = env->GetStaticObjectField(contextClass, imsField);
    jmethodID getSystemService = env->GetMethodID(activityClass, "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject imm = env->CallObjectMethod(activity, getSystemService, imsName);
    if (imm) {
        jclass immClass = env->GetObjectClass(imm);
        jmethodID getWindow = env->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;");
        jobject window = env->CallObjectMethod(activity, getWindow);
        jclass windowClass = env->GetObjectClass(window);
        jmethodID getDecorView = env->GetMethodID(windowClass, "getDecorView", "()Landroid/view/View;");
        jobject decorView = env->CallObjectMethod(window, getDecorView);
        if (show) {
            jmethodID showSoftInput = env->GetMethodID(immClass, "showSoftInput",
                "(Landroid/view/View;I)Z");
            env->CallBooleanMethod(imm, showSoftInput, decorView, 0);
        } else {
            jclass viewClass = env->GetObjectClass(decorView);
            jmethodID getWindowToken = env->GetMethodID(viewClass, "getWindowToken",
                "()Landroid/os/IBinder;");
            jobject token = env->CallObjectMethod(decorView, getWindowToken);
            jmethodID hideSoftInput = env->GetMethodID(immClass, "hideSoftInputFromWindow",
                "(Landroid/os/IBinder;I)Z");
            env->CallBooleanMethod(imm, hideSoftInput, token, 0);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
}

bool AttachJniForActivity(JNIEnv*& env, bool& attached) {
    env = nullptr;
    attached = false;
    if (!g_ctx.app || !g_ctx.app->activity || !g_ctx.app->activity->vm ||
        !g_ctx.app->activity->clazz) {
        return false;
    }
    JavaVM* vm = g_ctx.app->activity->vm;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return true;
    }
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        env = nullptr;
        return false;
    }
    attached = true;
    return true;
}

void DetachJniForActivity(bool attached) {
    if (attached && g_ctx.app && g_ctx.app->activity && g_ctx.app->activity->vm) {
        g_ctx.app->activity->vm->DetachCurrentThread();
    }
}

jmethodID FindFilePickerMethod(JNIEnv* env, jclass activityClass) {
    if (!env || !activityClass) return nullptr;
    jmethodID method = env->GetMethodID(activityClass, "launchModularityFilePicker", "(Z)V");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        method = nullptr;
    }
    return method;
}

std::string JStringToString(JNIEnv* env, jstring value) {
    if (!env || !value) return {};
    const char* raw = env->GetStringUTFChars(value, nullptr);
    if (!raw) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return {};
    }
    std::string out(raw);
    env->ReleaseStringUTFChars(value, raw);
    return out;
}

int32_t HandleInputEvent(android_app* /*app*/, AInputEvent* event) {
    const int32_t type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t keyCode = AKeyEvent_getKeyCode(event);
        if (keyCode == AKEYCODE_BACK || keyCode == AKEYCODE_HOME ||
            keyCode == AKEYCODE_VOLUME_UP || keyCode == AKEYCODE_VOLUME_DOWN ||
            keyCode == AKEYCODE_APP_SWITCH) {
            return 0;
        }
        const int32_t action = AKeyEvent_getAction(event);
        const bool down = (action == AKEY_EVENT_ACTION_DOWN);
        const bool up   = (action == AKEY_EVENT_ACTION_UP);
        if (!down && !up) return 0;
        g_ctx.keyEvents.push_back({static_cast<int>(keyCode), down});
        if (down) {
            const bool shift = (AKeyEvent_getMetaState(event) & AMETA_SHIFT_ON) != 0;
            if (const char c = AndroidKeycodeToChar(keyCode, shift)) {
                g_ctx.inputChars.push_back(static_cast<unsigned int>(c));
            }
        }
        return 1;
    }

    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;

    const int32_t source = AInputEvent_getSource(event);
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) == 0) return 0;

    const int32_t rawAction = AMotionEvent_getAction(event);
    const int32_t action    = rawAction & AMOTION_EVENT_ACTION_MASK;
    const size_t  count     = AMotionEvent_getPointerCount(event);

    const size_t actionIndex =
        static_cast<size_t>((rawAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

    const bool cancel = (action == AMOTION_EVENT_ACTION_CANCEL);

    int live = 0;
    if (!cancel) {
        for (size_t p = 0; p < count && live < RuntimeContext::kMaxTouchPointers; ++p) {
            const bool lifting =
                (action == AMOTION_EVENT_ACTION_UP) ||
                (action == AMOTION_EVENT_ACTION_POINTER_UP && p == actionIndex);
            if (lifting) continue;

            RuntimeContext::TouchPointer& slot = g_ctx.touches[live];
            slot.id     = AMotionEvent_getPointerId(event, p);
            slot.x      = AMotionEvent_getX(event, p);
            slot.y      = AMotionEvent_getY(event, p);
            slot.active = true;
            ++live;
        }
    }
    g_ctx.touchCount = live;

    if (live > 0) {
        g_ctx.primaryX = g_ctx.touches[0].x;
        g_ctx.primaryY = g_ctx.touches[0].y;
    }
    return 1;
}

} // namespace

bool PollEvents() {
    if (!g_ctx.app || g_ctx.app->destroyRequested) return false;
    return DrainEvents(0);
}

bool PresentFrame() {
    if (g_ctx.display == EGL_NO_DISPLAY || g_ctx.surface == EGL_NO_SURFACE) {
        return false;
    }
    if (!eglSwapBuffers(g_ctx.display, g_ctx.surface)) {
        EGLint err = eglGetError();
        if (err == EGL_BAD_SURFACE) {
            MODU_LOGW("eglSwapBuffers: surface invalid, releasing");
            DestroyEGLSurface();
        } else {
            MODU_LOGE("eglSwapBuffers failed: %s", EglErrorString(err));
        }
        return false;
    }
    return true;
}

bool HasRenderSurface() {
    return g_ctx.surface != EGL_NO_SURFACE && !g_ctx.paused;
}

void GetSurfaceSize(int* outWidth, int* outHeight) {
    if (outWidth)  *outWidth  = g_ctx.surfaceWidth;
    if (outHeight) *outHeight = g_ctx.surfaceHeight;
}

const char* GetInternalDataPath() {
    if (g_ctx.app && g_ctx.app->activity) {
        return g_ctx.app->activity->internalDataPath;
    }
    return nullptr;
}

void* GetJavaVM() {
    if (g_ctx.app && g_ctx.app->activity) {
        return static_cast<void*>(g_ctx.app->activity->vm);
    }
    return nullptr;
}

void* GetActivityObject() {
    if (g_ctx.app && g_ctx.app->activity) {
        return static_cast<void*>(g_ctx.app->activity->clazz);
    }
    return nullptr;
}

void* GetEglDisplay() {
    return (g_ctx.display == EGL_NO_DISPLAY) ? nullptr : static_cast<void*>(g_ctx.display);
}

void* GetEglConfig() {
    return static_cast<void*>(g_ctx.config);
}

void* GetEglContext() {
    return (g_ctx.context == EGL_NO_CONTEXT) ? nullptr : static_cast<void*>(g_ctx.context);
}

int GetPointerCount() {
    return g_ctx.touchCount;
}

bool GetPointer(int i, int* outId, float* outX, float* outY) {
    if (i < 0 || i >= g_ctx.touchCount) return false;
    const RuntimeContext::TouchPointer& slot = g_ctx.touches[i];
    if (outId) *outId = static_cast<int>(slot.id);
    if (outX)  *outX  = slot.x;
    if (outY)  *outY  = slot.y;
    return true;
}

bool GetPrimaryPointer(float* outX, float* outY, bool* outActive) {
    if (outX)      *outX      = g_ctx.primaryX;
    if (outY)      *outY      = g_ctx.primaryY;
    if (outActive) *outActive = g_ctx.touchCount > 0;
    return true;
}

float GetDisplayDensityScale() {
    if (g_ctx.app && g_ctx.app->config) {
        const int32_t density = AConfiguration_getDensity(g_ctx.app->config);
        if (density > 0 && density != ACONFIGURATION_DENSITY_ANY &&
            density != ACONFIGURATION_DENSITY_NONE) {
            float scale = static_cast<float>(density) / 160.0f;
            if (scale < 1.0f) scale = 1.0f;
            if (scale > 4.0f) scale = 4.0f;
            return scale;
        }
    }
    return 2.0f;
}

void SetSoftKeyboardVisible(bool visible) {
    if (visible == g_ctx.softKeyboardVisible) return;
    g_ctx.softKeyboardVisible = visible;
    ApplySoftKeyboard(g_ctx.app, visible);
}

bool PollInputChar(unsigned int* outChar) {
    if (g_ctx.inputChars.empty()) return false;
    if (outChar) *outChar = g_ctx.inputChars.front();
    g_ctx.inputChars.pop_front();
    return true;
}

bool PollKeyEvent(int* outKeyCode, bool* outDown) {
    if (g_ctx.keyEvents.empty()) return false;
    const RuntimeContext::PendingKey k = g_ctx.keyEvents.front();
    g_ctx.keyEvents.pop_front();
    if (outKeyCode) *outKeyCode = k.keyCode;
    if (outDown)    *outDown    = k.down;
    return true;
}

bool SupportsFilePicker() {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (!AttachJniForActivity(env, attached)) {
        return false;
    }

    jobject activity = g_ctx.app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    const bool supported = FindFilePickerMethod(env, activityClass) != nullptr;
    if (activityClass) env->DeleteLocalRef(activityClass);
    DetachJniForActivity(attached);
    return supported;
}

bool RequestFilePicker(bool allowMultiple, std::string& error) {
    error.clear();

    JNIEnv* env = nullptr;
    bool attached = false;
    if (!AttachJniForActivity(env, attached)) {
        error = "Android activity is not available.";
        return false;
    }

    jobject activity = g_ctx.app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID launchPicker = FindFilePickerMethod(env, activityClass);
    if (!launchPicker) {
        if (activityClass) env->DeleteLocalRef(activityClass);
        DetachJniForActivity(attached);
        error = "Android file picker bridge is not packaged in this APK.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_filePickerMutex);
        if (g_filePickerPending) {
            if (activityClass) env->DeleteLocalRef(activityClass);
            DetachJniForActivity(attached);
            error = "Android file picker is already open.";
            return false;
        }
        g_filePickerPaths.clear();
        g_filePickerError.clear();
        g_filePickerCanceled = false;
        g_filePickerResultReady = false;
        g_filePickerPending = true;
    }

    env->CallVoidMethod(activity, launchPicker, allowMultiple ? JNI_TRUE : JNI_FALSE);
    const bool failed = env->ExceptionCheck();
    if (failed) {
        env->ExceptionClear();
        std::lock_guard<std::mutex> lock(g_filePickerMutex);
        g_filePickerPending = false;
        g_filePickerResultReady = false;
        error = "Failed to open Android file picker.";
    }

    if (activityClass) env->DeleteLocalRef(activityClass);
    DetachJniForActivity(attached);
    return !failed;
}

bool PollFilePickerResult(FilePickerResult& result) {
    std::lock_guard<std::mutex> lock(g_filePickerMutex);
    if (!g_filePickerResultReady) {
        return false;
    }
    result.paths = g_filePickerPaths;
    result.error = g_filePickerError;
    result.canceled = g_filePickerCanceled;
    g_filePickerPaths.clear();
    g_filePickerError.clear();
    g_filePickerCanceled = false;
    g_filePickerResultReady = false;
    return true;
}

std::string GetExternalDocumentsPath() {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (!AttachJniForActivity(env, attached)) return {};

    std::string result;
    jobject activity = g_ctx.app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = activityClass
        ? env->GetMethodID(activityClass, "modularityExternalDocumentsPath", "()Ljava/lang/String;")
        : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (method) {
        jstring js = static_cast<jstring>(env->CallObjectMethod(activity, method));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else if (js) {
            result = JStringToString(env, js);
            env->DeleteLocalRef(js);
        }
    }
    if (activityClass) env->DeleteLocalRef(activityClass);
    DetachJniForActivity(attached);
    return result;
}

bool HasAllFilesAccess() {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (!AttachJniForActivity(env, attached)) return false;

    bool granted = false;
    jobject activity = g_ctx.app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = activityClass
        ? env->GetMethodID(activityClass, "modularityHasStorageAccess", "()Z")
        : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (method) {
        granted = env->CallBooleanMethod(activity, method) == JNI_TRUE;
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            granted = false;
        }
    }
    if (activityClass) env->DeleteLocalRef(activityClass);
    DetachJniForActivity(attached);
    return granted;
}

bool RequestAllFilesAccess(std::string& error) {
    error.clear();

    JNIEnv* env = nullptr;
    bool attached = false;
    if (!AttachJniForActivity(env, attached)) {
        error = "Android activity is not available.";
        return false;
    }

    jobject activity = g_ctx.app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = activityClass
        ? env->GetMethodID(activityClass, "modularityRequestStorageAccess", "()V")
        : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    bool ok = false;
    if (!method) {
        error = "Storage access bridge is not packaged in this APK.";
    } else {
        env->CallVoidMethod(activity, method);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            error = "Failed to request storage access.";
        } else {
            ok = true;
        }
    }
    if (activityClass) env->DeleteLocalRef(activityClass);
    DetachJniForActivity(attached);
    return ok;
}

extern "C" JNIEXPORT void JNICALL
Java_com_modularity_android_ModularityNativeActivity_nativeOnFilePickerResult(
    JNIEnv* env,
    jclass /*clazz*/,
    jobjectArray paths,
    jstring error,
    jboolean canceled) {
    std::vector<std::string> copiedPaths;
    if (env && paths) {
        const jsize count = env->GetArrayLength(paths);
        copiedPaths.reserve(static_cast<size_t>(count));
        for (jsize i = 0; i < count; ++i) {
            jstring item = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
            if (!item) continue;
            copiedPaths.push_back(JStringToString(env, item));
            env->DeleteLocalRef(item);
        }
    }

    std::string errorText = JStringToString(env, error);
    if (env && env->ExceptionCheck()) {
        env->ExceptionClear();
        if (errorText.empty()) {
            errorText = "Android file picker callback failed while reading selected paths.";
        }
    }

    std::lock_guard<std::mutex> lock(g_filePickerMutex);
    g_filePickerPaths = std::move(copiedPaths);
    g_filePickerError = std::move(errorText);
    g_filePickerCanceled = canceled == JNI_TRUE;
    g_filePickerPending = false;
    g_filePickerResultReady = true;
}

void Run(android_app* app) {
    RedirectStdioToLogcat();

    g_ctx = RuntimeContext{};
    g_ctx.app = app;
    app->userData = &g_ctx;
    app->onAppCmd = HandleAppCmd;
    app->onInputEvent = HandleInputEvent;

    if (app->activity) {
        constexpr uint32_t kWindowFlagFullscreen = 0x00000400u;
        ANativeActivity_setWindowFlags(app->activity, kWindowFlagFullscreen, 0);
    }

    MODU_LOGI("Modularity Android runtime starting");

    while (!app->destroyRequested && g_ctx.surface == EGL_NO_SURFACE) {
        if (!DrainEvents(-1)) break;
    }
    if (app->destroyRequested) {
        MODU_LOGI("Destroy requested before EGL came up");
        DestroyEGL();
        return;
    }

    if (app->activity && app->activity->assetManager) {
        Modularity::Platform::SetAssetSource(
            Modularity::Platform::MakeAndroidAssetSource(app->activity->assetManager));
        MODU_LOGI("AssetSource installed (AAssetManager)");
    } else {
        MODU_LOGW("No AAssetManager available; engine will fall back to filesystem reads");
    }

    {
        Engine engine;
        if (!engine.init()) {
            MODU_LOGE("Engine::init() failed on Android");
        } else {
            MODU_LOGI("Engine initialized; entering main loop");
            engine.run();
            engine.shutdown();
        }
    }

    MODU_LOGI("Modularity Android runtime exiting");
    DestroyEGL();
}

} // namespace Modularity::AndroidRuntime

extern "C" void android_main(struct android_app* app) {
    Modularity::AndroidRuntime::Run(app);
}

#endif // __ANDROID__
