#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <string>

#include "horizon/horizon_os.h"
#include "gpu/egl_manager.h"
#include "gpu/pica_gles.h"

#define LOG_TAG "DBZ_MainJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static ANativeWindow* g_Window = nullptr;
static EGLManager*    g_EGL = nullptr;
static PicaGLES*      g_Renderer = nullptr;

static pthread_t      g_RenderThread;
static bool           g_Running = false;
static bool           g_Paused = false;
static pthread_mutex_t g_Mutex = PTHREAD_MUTEX_INITIALIZER;

static void* GameThread(void* arg) {
    LOGI("Game and Render loop thread starting...");

    pthread_mutex_lock(&g_Mutex);
    if (g_EGL) {
        if (!g_EGL->MakeCurrent()) {
            LOGE("GameThread: Failed to make EGL context current!");
        } else {
            LOGI("GameThread: EGL context bound to render thread successfully.");
        }
    }
    if (g_Renderer && g_EGL) {
        g_Renderer->InitGL();
        g_Renderer->Resize(g_EGL->GetWidth(), g_EGL->GetHeight());
    }
    pthread_mutex_unlock(&g_Mutex);

    while (g_Running) {
        if (g_Paused || !g_EGL || !g_Renderer) {
            usleep(16000);
            continue;
        }

        pthread_mutex_lock(&g_Mutex);

        if (g_Horizon) {
            // Signal 3DS GPU VBlank interrupts (PDC0 = top screen, PDC1 = bottom screen)
            g_Horizon->QueueGspInterrupt(2); // PDC0
            g_Horizon->QueueGspInterrupt(3); // PDC1
            g_Horizon->SignalDspInterrupts();
            g_Horizon->UpdateHID();
        }

        uint8_t* topVRAM = g_Horizon ? g_Horizon->GetTopScreenVRAM() : nullptr;
        uint8_t* botVRAM = g_Horizon ? g_Horizon->GetBottomScreenVRAM() : nullptr;

        // Render PICA200 Dual Screens via OpenGL ES 3.0
        g_Renderer->RenderFrame(topVRAM, botVRAM);
        g_EGL->SwapBuffers();

        pthread_mutex_unlock(&g_Mutex);

        // Cap to ~60 FPS
        usleep(16666);
    }

    LOGI("Game and Render loop thread finished.");
    return nullptr;
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeInit(JNIEnv* env, jclass clazz, jstring internalPath) {
    const char* pathStr = env->GetStringUTFChars(internalPath, nullptr);
    LOGI("Native Init called with path: %s", pathStr);

    if (!g_Horizon) {
        g_Horizon = new HorizonOS();
        g_Horizon->Initialize(pathStr);
    }

    env->ReleaseStringUTFChars(internalPath, pathStr);
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeSurfaceCreated(JNIEnv* env, jclass clazz, jobject surface) {
    LOGI("nativeSurfaceCreated");
    g_Window = ANativeWindow_fromSurface(env, surface);

    g_EGL = new EGLManager();
    if (g_EGL->Init(g_Window)) {
        g_Renderer = new PicaGLES();
    }

    if (!g_Running) {
        g_Running = true;
        pthread_create(&g_RenderThread, nullptr, GameThread, nullptr);
        if (g_Horizon) {
            g_Horizon->StartGame();
        }
    }
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeSurfaceChanged(JNIEnv* env, jclass clazz, jint width, jint height) {
    LOGI("nativeSurfaceChanged: %dx%d", width, height);
    if (g_Renderer) {
        g_Renderer->Resize(width, height);
    }
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeSurfaceDestroyed(JNIEnv* env, jclass clazz) {
    LOGI("nativeSurfaceDestroyed");
    pthread_mutex_lock(&g_Mutex);

    if (g_Renderer) {
        delete g_Renderer;
        g_Renderer = nullptr;
    }
    if (g_EGL) {
        delete g_EGL;
        g_EGL = nullptr;
    }
    if (g_Window) {
        ANativeWindow_release(g_Window);
        g_Window = nullptr;
    }

    pthread_mutex_unlock(&g_Mutex);
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeTouch(JNIEnv* env, jclass clazz, jboolean pressed, jfloat x, jfloat y) {
    if (g_Horizon) {
        g_Horizon->SetTouch(pressed, (uint16_t)x, (uint16_t)y);
    }
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeButton(JNIEnv* env, jclass clazz, jint buttonMask, jboolean pressed) {
    if (g_Horizon) {
        g_Horizon->SetButtons(buttonMask);
    }
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativeResume(JNIEnv* env, jclass clazz) {
    LOGI("nativeResume");
    g_Paused = false;
}

JNIEXPORT void JNICALL
Java_com_dbz_butoden_MainActivity_nativePause(JNIEnv* env, jclass clazz) {
    LOGI("nativePause");
    g_Paused = true;
}

} // extern "C"
