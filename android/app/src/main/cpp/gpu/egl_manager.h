#ifndef EGL_MANAGER_H
#define EGL_MANAGER_H

#include <EGL/egl.h>
#include <android/native_window.h>

class EGLManager {
public:
    EGLManager();
    ~EGLManager();

    bool Init(ANativeWindow* window);
    bool MakeCurrent();
    bool DetachCurrent();
    void SwapBuffers();
    void Terminate();

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    EGLDisplay m_display;
    EGLSurface m_surface;
    EGLContext m_context;
    EGLConfig  m_config;
    ANativeWindow* m_window;
    int m_width;
    int m_height;
};

#endif // EGL_MANAGER_H
