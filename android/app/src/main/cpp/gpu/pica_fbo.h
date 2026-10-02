#ifndef PICA_FBO_H
#define PICA_FBO_H

#include <GLES3/gl3.h>
#include <stdint.h>
#include <vector>

class PicaFBO {
public:
    PicaFBO();
    ~PicaFBO();

    bool Init(int width, int height, int scale = 1);
    void Destroy();

    void Bind();
    void Unbind();
    void Clear(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f);

    GLuint GetTexture() const { return m_colorTex; }
    GLuint GetFBO() const { return m_fbo; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    int GetNativeWidth() const { return m_nativeWidth; }
    int GetNativeHeight() const { return m_nativeHeight; }
    int GetScale() const { return m_scale; }

    bool IsDirty() const { return m_isDirty; }
    void SetDirty(bool dirty) { m_isDirty = dirty; }

    // Sincronização: lê de volta os pixels do FBO para a memória da CPU
    bool ReadPixelsRGBA(uint8_t* dstBuffer, bool flipY);

    // Sincronização direta para o layout Morton 8x8 do 3DS (para DisplayTransfer / MemoryFill)
    bool SyncTo3DSFramebuffer(uint8_t* dstBuffer, uint32_t fbW, uint32_t fbH, uint32_t format, bool isBottom);

private:
    GLuint m_fbo;
    GLuint m_colorTex;
    GLuint m_depthRbo;
    int m_width;
    int m_height;
    int m_nativeWidth;
    int m_nativeHeight;
    int m_scale;
    bool m_isDirty;
};

#endif // PICA_FBO_H
