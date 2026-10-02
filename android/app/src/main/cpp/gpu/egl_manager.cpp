#include "egl_manager.h"
#include <android/log.h>

#define LOG_TAG "EGLManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

EGLManager::EGLManager()
    : m_display(EGL_NO_DISPLAY),
      m_surface(EGL_NO_SURFACE),
      m_context(EGL_NO_CONTEXT),
      m_config(nullptr),
      m_window(nullptr),
      m_width(0),
      m_height(0) {}

EGLManager::~EGLManager() {
    Terminate();
}

bool EGLManager::Init(ANativeWindow* window) {
    m_window = window;
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(m_display, &major, &minor)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLint numConfigs;
    if (!eglChooseConfig(m_display, attribs, &m_config, 1, &numConfigs) || numConfigs <= 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
    if (m_context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    EGLint visualId = 0;
    if (eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &visualId)) {
        ANativeWindow_setBuffersGeometry(m_window, 0, 0, visualId);
    }

    m_surface = eglCreateWindowSurface(m_display, m_config, m_window, nullptr);
    if (m_surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
        LOGE("eglMakeCurrent failed");
        return false;
    }

    eglQuerySurface(m_display, m_surface, EGL_WIDTH, &m_width);
    eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &m_height);

    // Detach from creator thread so render thread can take ownership
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    LOGI("EGL Initialized successfully: %dx%d (OpenGL ES 3.0)", m_width, m_height);
    return true;
}

bool EGLManager::MakeCurrent() {
    if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE && m_context != EGL_NO_CONTEXT) {
        return eglMakeCurrent(m_display, m_surface, m_surface, m_context) == EGL_TRUE;
    }
    return false;
}

bool EGLManager::DetachCurrent() {
    if (m_display != EGL_NO_DISPLAY) {
        return eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
    }
    return false;
}

void EGLManager::SwapBuffers() {
    if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE) {
        eglSwapBuffers(m_display, m_surface);
    }
}

void EGLManager::Terminate() {
    if (m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != EGL_NO_SURFACE) {
            eglDestroySurface(m_display, m_surface);
            m_surface = EGL_NO_SURFACE;
        }
        if (m_context != EGL_NO_CONTEXT) {
            eglDestroyContext(m_display, m_context);
            m_context = EGL_NO_CONTEXT;
        }
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
    }
}
