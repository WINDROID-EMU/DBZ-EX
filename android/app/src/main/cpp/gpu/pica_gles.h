#ifndef PICA_GLES_H
#define PICA_GLES_H

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <stdint.h>

#define TOP_WIDTH    400
#define TOP_HEIGHT   240
#define BOT_WIDTH    320
#define BOT_HEIGHT   240

class PicaGLES {
public:
    PicaGLES();
    ~PicaGLES();

    bool InitGL();
    void Resize(int windowWidth, int windowHeight);
    void RenderFrame(const uint8_t* topPixels, const uint8_t* botPixels);
    void DestroyGL();

private:
    int m_windowWidth;
    int m_windowHeight;

    GLuint m_topTexture;
    GLuint m_botTexture;

    GLuint m_program;
    GLuint m_vao;
    GLuint m_vbo;

    GLint m_uScreenTexture;
    GLint m_uTransform;

    void CreateTextures();
    void BuildShaders();
};

#endif // PICA_GLES_H
