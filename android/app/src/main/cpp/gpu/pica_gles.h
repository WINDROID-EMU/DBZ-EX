#ifndef PICA_GLES_H
#define PICA_GLES_H

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <stdint.h>

#include "pica_fbo.h"
#include "pica_cmd_processor.h"
#include <unordered_map>
#include <vector>

#define TOP_WIDTH    400
#define TOP_HEIGHT   240
#define BOT_WIDTH    400
#define BOT_HEIGHT   240

class HorizonOS;

class PicaGLES {
public:
    PicaGLES();
    ~PicaGLES();

    bool InitGL();
    void Resize(int windowWidth, int windowHeight);
    void RenderFrame(const uint8_t* topPixels, const uint8_t* botPixels);
    void DestroyGL();

    // Renderização nativa para os FBOs na GPU Adreno
    void ExecuteDrawCalls(const std::vector<PicaDrawCall>& draws, HorizonOS* os);

    PicaFBO& GetTopFBO() { return m_topFBO; }
    PicaFBO& GetBottomFBO() { return m_botFBO; }

private:
    int m_windowWidth;
    int m_windowHeight;

    // FBOs dedicados para OpenGL ES 3.0
    PicaFBO m_topFBO;
    PicaFBO m_botFBO;

    // Texturas legadas de upload via CPU
    GLuint m_topTexture;
    GLuint m_botTexture;

    // Shader e geometria de apresentação na tela
    GLuint m_program;
    GLuint m_vao;
    GLuint m_vbo;
    GLint m_uScreenTexture;
    GLint m_uTransform;
    GLint m_uFlipY;

    // Shader e geometria para rasterização de UI nos FBOs
    GLuint m_uiProgram;
    GLuint m_uiVao;
    GLuint m_uiVbo;
    GLuint m_uiIbo;
    GLint m_uScreenSize;
    GLint m_uTexture;
    GLint m_uHasTexture;

    // Cache de texturas na GPU indexado pelo endereço físico do 3DS
    std::unordered_map<uint32_t, GLuint> m_textureCache;
    GLuint GetOrCreateTexture(uint32_t texAddrPhys, uint32_t texW, uint32_t texH, uint32_t texType, HorizonOS* os);

    void CreateTextures();
    void BuildShaders();
    void BuildUIShaders();
};

#endif // PICA_GLES_H
