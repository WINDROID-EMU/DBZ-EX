#include "pica_fbo.h"
#include <android/log.h>
#include <string.h>

#define LOG_TAG "PicaFBO"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

PicaFBO::PicaFBO()
    : m_fbo(0),
      m_colorTex(0),
      m_depthRbo(0),
      m_width(0),
      m_height(0),
      m_nativeWidth(0),
      m_nativeHeight(0),
      m_scale(1),
      m_isDirty(false) {}

PicaFBO::~PicaFBO() {
    Destroy();
}

bool PicaFBO::Init(int width, int height, int scale) {
    Destroy();

    if (scale < 1) scale = 1;
    m_scale = scale;
    m_nativeWidth = width;
    m_nativeHeight = height;
    m_width = width * scale;
    m_height = height * scale;

    // 1. Gera e configura a textura de cor (RGBA8)
    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 2. Gera e configura o Depth/Stencil Renderbuffer (DEPTH24_STENCIL8)
    glGenRenderbuffers(1, &m_depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_width, m_height);

    // 3. Monta o Framebuffer Object (FBO) com os anexos de cor e depth/stencil
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depthRbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO creation failed, status: 0x%X", status);
        Destroy();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    // Limpa o buffer inicial para preto opaco
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    LOGI("PicaFBO inicializado: %dx%d (nativo: %dx%d, escala: %dx, fbo=%u, tex=%u, rbo=%u)",
         m_width, m_height, m_nativeWidth, m_nativeHeight, m_scale, m_fbo, m_colorTex, m_depthRbo);
    return true;
}

void PicaFBO::Clear(float r, float g, float b, float a) {
    if (!m_fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PicaFBO::Destroy() {
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_colorTex) {
        glDeleteTextures(1, &m_colorTex);
        m_colorTex = 0;
    }
    if (m_depthRbo) {
        glDeleteRenderbuffers(1, &m_depthRbo);
        m_depthRbo = 0;
    }
    m_width = 0;
    m_height = 0;
    m_isDirty = false;
}

void PicaFBO::Bind() {
    if (m_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glViewport(0, 0, m_width, m_height);
    }
}

void PicaFBO::Unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool PicaFBO::ReadPixelsRGBA(uint8_t* dstBuffer, bool flipY) {
    if (!m_fbo || !dstBuffer) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    if (!flipY) {
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, dstBuffer);
    } else {
        // Read into temporary buffer and flip vertically
        std::vector<uint8_t> temp(m_width * m_height * 4);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, temp.data());
        size_t rowStride = m_width * 4;
        for (int y = 0; y < m_height; ++y) {
            const uint8_t* srcRow = temp.data() + (m_height - 1 - y) * rowStride;
            uint8_t* dstRow = dstBuffer + y * rowStride;
            memcpy(dstRow, srcRow, rowStride);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool PicaFBO::SyncTo3DSFramebuffer(uint8_t* dstBuffer, uint32_t fbW, uint32_t fbH, uint32_t format, bool isBottom) {
    if (!m_fbo || !dstBuffer) return false;

    std::vector<uint8_t> rgba(m_width * m_height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Tabela LUT Morton 8x8 do PICA200
    static const uint32_t xLut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint32_t yLut[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };

    // Orientação nativa do 3DS: retrato 240x400 (ou 240x320)
    // Coordenada paisagem (X: 0..m_nativeWidth, Y: 0..m_nativeHeight) -> Retrato (pX = 239 - Y, pY = X + offset)
    uint32_t xOffset = (isBottom && fbH == 400) ? 40 : 0;
    size_t bpp = (format == 0) ? 4 : ((format == 1) ? 3 : 2);

    for (int sy = 0; sy < m_nativeHeight; ++sy) {
        int pX = 239 - sy;
        for (int sx = 0; sx < m_nativeWidth; ++sx) {
            int pY = sx + xOffset;
            if (pX >= 0 && pX < (int)fbW && pY >= 0 && pY < (int)fbH) {
                int srcX = sx * m_scale;
                int srcY = sy * m_scale;
                size_t srcIdx = (srcY * m_width + srcX) * 4;
                if (srcIdx + 3 >= rgba.size()) continue;

                uint8_t r = rgba[srcIdx + 0];
                uint8_t g = rgba[srcIdx + 1];
                uint8_t b = rgba[srcIdx + 2];
                uint8_t a = rgba[srcIdx + 3];

                uint32_t xMod = (uint32_t)pX & 7U;
                uint32_t yMod = (uint32_t)pY & 7U;
                uint32_t fbOff = (xLut[xMod] + yLut[yMod] + ((uint32_t)pX & ~7U) * 8U) * bpp + ((uint32_t)pY & ~7U) * fbW * bpp;

                if (format == 0) { // RGBA8 no 3DS: [0]=A, [1]=B, [2]=G, [3]=R
                    dstBuffer[fbOff + 0] = a;
                    dstBuffer[fbOff + 1] = b;
                    dstBuffer[fbOff + 2] = g;
                    dstBuffer[fbOff + 3] = r;
                } else if (format == 1) { // RGB8: [0]=B, [1]=G, [2]=R
                    dstBuffer[fbOff + 0] = b;
                    dstBuffer[fbOff + 1] = g;
                    dstBuffer[fbOff + 2] = r;
                } else if (format == 2) { // RGB565
                    uint16_t col565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                    dstBuffer[fbOff + 0] = col565 & 0xFF;
                    dstBuffer[fbOff + 1] = (col565 >> 8) & 0xFF;
                }
            }
        }
    }
    return true;
}

