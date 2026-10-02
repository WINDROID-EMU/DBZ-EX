#include "pica_gles.h"
#include "rg_etc1.h"
#include "../horizon/horizon_os.h"
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "PicaGLES"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char* kVertexShaderSource =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPosition;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "uniform mat4 uTransform;\n"
    "uniform int uFlipY;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = uTransform * vec4(aPosition, 0.0, 1.0);\n"
    "    vTexCoord = vec2(aTexCoord.x, uFlipY != 0 ? (1.0 - aTexCoord.y) : aTexCoord.y);\n"
    "}\n";

static const char* kFragmentShaderSource =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTexCoord;\n"
    "uniform sampler2D uScreenTexture;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(uScreenTexture, vTexCoord);\n"
    "}\n";

// Shader nativo para renderização direta de UI 2D no FBO
static const char* kUIVertexShaderSource =
    "#version 300 es\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "uniform vec2 uScreenSize;\n"
    "out vec2 vTexCoord;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    float ndcX = (aPos.x / (uScreenSize.x * 0.5)) - 1.0;\n"
    "    float ndcY = 1.0 - (aPos.y / (uScreenSize.y * 0.5));\n"
    "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "}\n";

static const char* kUIFragmentShaderSource =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTexture;\n"
    "uniform int uHasTexture;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 texCol = (uHasTexture != 0) ? texture(uTexture, vTexCoord) : vec4(1.0);\n"
    "    vec4 finalCol = texCol * vColor;\n"
    "    if (finalCol.a < 0.01) discard;\n"
    "    fragColor = finalCol;\n"
    "}\n";

PicaGLES::PicaGLES()
    : m_windowWidth(1280),
      m_windowHeight(720),
      m_topTexture(0),
      m_botTexture(0),
      m_program(0),
      m_vao(0),
      m_vbo(0),
      m_uScreenTexture(-1),
      m_uTransform(-1),
      m_uFlipY(-1),
      m_uiProgram(0),
      m_uiVao(0),
      m_uiVbo(0),
      m_uiIbo(0),
      m_uScreenSize(-1),
      m_uTexture(-1),
      m_uHasTexture(-1) {}

PicaGLES::~PicaGLES() {
    DestroyGL();
}

static GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char info[512];
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        LOGE("Shader compilation failed: %s", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

void PicaGLES::BuildShaders() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShaderSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);

    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    m_uScreenTexture = glGetUniformLocation(m_program, "uScreenTexture");
    m_uTransform = glGetUniformLocation(m_program, "uTransform");
    m_uFlipY = glGetUniformLocation(m_program, "uFlipY");

    // Geometria do quad: pos(x, y), uv(u, v)
    float quadVertices[] = {
        // x,     y,     u,   v
        -1.0f,  1.0f,  0.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
    };

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
}

void PicaGLES::BuildUIShaders() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kUIVertexShaderSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kUIFragmentShaderSource);

    m_uiProgram = glCreateProgram();
    glAttachShader(m_uiProgram, vs);
    glAttachShader(m_uiProgram, fs);
    glLinkProgram(m_uiProgram);

    glDeleteShader(vs);
    glDeleteShader(fs);

    m_uScreenSize = glGetUniformLocation(m_uiProgram, "uScreenSize");
    m_uTexture = glGetUniformLocation(m_uiProgram, "uTexture");
    m_uHasTexture = glGetUniformLocation(m_uiProgram, "uHasTexture");

    glGenVertexArrays(1, &m_uiVao);
    glGenBuffers(1, &m_uiVbo);
    glGenBuffers(1, &m_uiIbo);

    glBindVertexArray(m_uiVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_uiVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_uiIbo);

    // Layout: PicaVertex (x, y, z; u, v; r, g, b, a)
    size_t stride = sizeof(PicaVertex);

    // Location 0: aPos (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);

    // Location 1: aTexCoord (vec2)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));

    // Location 2: aColor (vec4)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));

    glBindVertexArray(0);
}

void PicaGLES::CreateTextures() {
    glGenTextures(1, &m_topTexture);
    glBindTexture(GL_TEXTURE_2D, m_topTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TOP_WIDTH, TOP_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenTextures(1, &m_botTexture);
    glBindTexture(GL_TEXTURE_2D, m_botTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, BOT_WIDTH, BOT_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
}

bool PicaGLES::InitGL() {
    LOGI("Initializing OpenGL ES 3.0 PICA200 Pipeline (FBOs + Presentation)...");
    m_topFBO.Init(TOP_WIDTH, TOP_HEIGHT);
    m_botFBO.Init(BOT_WIDTH, BOT_HEIGHT);

    CreateTextures();
    BuildShaders();
    BuildUIShaders();

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    return true;
}

void PicaGLES::Resize(int windowWidth, int windowHeight) {
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    glViewport(0, 0, windowWidth, windowHeight);
}

GLuint PicaGLES::GetOrCreateTexture(uint32_t texAddrPhys, uint32_t texW, uint32_t texH, uint32_t texType, HorizonOS* os) {
    if (!os || texAddrPhys == 0 || texW == 0 || texH == 0) return 0;

    auto it = m_textureCache.find(texAddrPhys);
    if (it != m_textureCache.end()) {
        return it->second;
    }

    uint8_t* texPtr = os->GetPointer(texAddrPhys);
    if (!texPtr) return 0;

    std::vector<uint8_t> decodedRGBA(texW * texH * 4, 0);
    uint32_t tilesPerRow = (texW + 7) / 8;
    uint32_t tilesPerCol = (texH + 7) / 8;

    static const uint32_t xLut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint32_t yLut[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };

    if (texType == 13) { // GPU_ETC1A4 (16 bytes por subtile 4x4, 64 bytes por tile 8x8)
        for (uint32_t tileY = 0; tileY < tilesPerCol; ++tileY) {
            for (uint32_t tileX = 0; tileX < tilesPerRow; ++tileX) {
                for (uint32_t subBlockIdx = 0; subBlockIdx < 4; ++subBlockIdx) {
                    uint32_t subBx = subBlockIdx % 2;
                    uint32_t subBy = subBlockIdx / 2;
                    uint32_t blockOffset = (tileY * tilesPerRow + tileX) * 64 + subBlockIdx * 16;
                    const uint8_t* blk = texPtr + blockOffset;

                    uint64_t alphaPacked;
                    memcpy(&alphaPacked, blk, sizeof(uint64_t));

                    uint8_t rev[8];
                    for (int b = 0; b < 8; ++b) {
                        rev[b] = blk[8 + (7 - b)];
                    }
                    uint32_t cachedRGBA[16];
                    rg_etc1::unpack_etc1_block(rev, cachedRGBA, false);

                    for (uint32_t subY = 0; subY < 4; ++subY) {
                        for (uint32_t subX = 0; subX < 4; ++subX) {
                            uint32_t px = tileX * 8 + subBx * 4 + subX;
                            uint32_t py = tileY * 8 + subBy * 4 + subY;
                            if (px < texW && py < texH) {
                                uint32_t aShift = 4 * (subX * 4 + subY);
                                uint8_t a4 = (uint8_t)((alphaPacked >> aShift) & 0x0F);
                                uint8_t a = (a4 << 4) | a4;

                                uint32_t col = cachedRGBA[subY * 4 + subX];
                                uint8_t r = col & 0xFF;
                                uint8_t g = (col >> 8) & 0xFF;
                                uint8_t b = (col >> 16) & 0xFF;

                                size_t dst = (py * texW + px) * 4;
                                decodedRGBA[dst + 0] = r;
                                decodedRGBA[dst + 1] = g;
                                decodedRGBA[dst + 2] = b;
                                decodedRGBA[dst + 3] = a;
                            }
                        }
                    }
                }
            }
        }
    } else if (texType == 12) { // GPU_ETC1 (8 bytes por subtile 4x4, 32 bytes por tile 8x8)
        for (uint32_t tileY = 0; tileY < tilesPerCol; ++tileY) {
            for (uint32_t tileX = 0; tileX < tilesPerRow; ++tileX) {
                for (uint32_t subBlockIdx = 0; subBlockIdx < 4; ++subBlockIdx) {
                    uint32_t subBx = subBlockIdx % 2;
                    uint32_t subBy = subBlockIdx / 2;
                    uint32_t blockOffset = (tileY * tilesPerRow + tileX) * 32 + subBlockIdx * 8;
                    const uint8_t* blk = texPtr + blockOffset;

                    uint8_t rev[8];
                    for (int b = 0; b < 8; ++b) {
                        rev[b] = blk[7 - b];
                    }
                    uint32_t cachedRGBA[16];
                    rg_etc1::unpack_etc1_block(rev, cachedRGBA, false);

                    for (uint32_t subY = 0; subY < 4; ++subY) {
                        for (uint32_t subX = 0; subX < 4; ++subX) {
                            uint32_t px = tileX * 8 + subBx * 4 + subX;
                            uint32_t py = tileY * 8 + subBy * 4 + subY;
                            if (px < texW && py < texH) {
                                uint32_t col = cachedRGBA[subY * 4 + subX];
                                uint8_t r = col & 0xFF;
                                uint8_t g = (col >> 8) & 0xFF;
                                uint8_t b = (col >> 16) & 0xFF;

                                size_t dst = (py * texW + px) * 4;
                                decodedRGBA[dst + 0] = r;
                                decodedRGBA[dst + 1] = g;
                                decodedRGBA[dst + 2] = b;
                                decodedRGBA[dst + 3] = 0xFF;
                            }
                        }
                    }
                }
            }
        }
    } else { // RGBA8 (Morton 8x8)
        for (uint32_t tileY = 0; tileY < tilesPerCol; ++tileY) {
            for (uint32_t tileX = 0; tileX < tilesPerRow; ++tileX) {
                for (uint32_t fineY = 0; fineY < 8; ++fineY) {
                    for (uint32_t fineX = 0; fineX < 8; ++fineX) {
                        uint32_t px = tileX * 8 + fineX;
                        uint32_t py = tileY * 8 + fineY;
                        if (px < texW && py < texH) {
                            uint32_t texOff = (tileY * tilesPerRow + tileX) * 256 + (xLut[fineX] + yLut[fineY]) * 4;
                            size_t dst = (py * texW + px) * 4;
                            decodedRGBA[dst + 0] = texPtr[texOff + 3]; // R
                            decodedRGBA[dst + 1] = texPtr[texOff + 2]; // G
                            decodedRGBA[dst + 2] = texPtr[texOff + 1]; // B
                            decodedRGBA[dst + 3] = texPtr[texOff + 0]; // A
                        }
                    }
                }
            }
        }
    }

    GLuint glTex = 0;
    glGenTextures(1, &glTex);
    glBindTexture(GL_TEXTURE_2D, glTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, decodedRGBA.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    m_textureCache[texAddrPhys] = glTex;
    return glTex;
}

void PicaGLES::ExecuteDrawCalls(const std::vector<PicaDrawCall>& draws, HorizonOS* os) {
    if (draws.empty()) return;

    static int s_gpuDrawCount = 0;
    if (++s_gpuDrawCount % 30 == 1) {
        LOGI("ExecuteDrawCalls: executing %zu draws (draw0: isBottom=%d, verts=%zu, indices=%zu, tex=0x%X %ux%u)",
             draws.size(), draws[0].isBottom, draws[0].vertices.size(), draws[0].indices.size(),
             draws[0].texAddrPhys, draws[0].texW, draws[0].texH);
        for (size_t d = 0; d < std::min((size_t)3, draws.size()); ++d) {
            const auto& dc = draws[d];
            for (size_t v = 0; v < std::min((size_t)4, dc.vertices.size()); ++v) {
                LOGI("  draw[%zu] v[%zu]: pos=(%.1f, %.1f, %.1f) uv=(%.3f, %.3f)",
                     d, v, dc.vertices[v].x, dc.vertices[v].y, dc.vertices[v].z,
                     dc.vertices[v].u, dc.vertices[v].v);
            }
        }
    }

    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

    glUseProgram(m_uiProgram);
    glBindVertexArray(m_uiVao);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    PicaFBO* currentBoundFBO = nullptr;
    GLuint lastBoundTex = 0xFFFFFFFF;

    for (const auto& dc : draws) {
        PicaFBO* targetFBO = dc.isBottom ? &m_botFBO : &m_topFBO;
        if (dc.isClear) {
            targetFBO->Clear(dc.clearColor[0], dc.clearColor[1], dc.clearColor[2], dc.clearColor[3]);
            targetFBO->SetDirty(true);
            currentBoundFBO = nullptr;
            continue;
        }

        if (dc.vertices.empty() || dc.indices.empty()) continue;

        if (targetFBO != currentBoundFBO) {
            currentBoundFBO = targetFBO;
            currentBoundFBO->Bind();
            glViewport(0, 0, currentBoundFBO->GetWidth(), currentBoundFBO->GetHeight());
            glUniform2f(m_uScreenSize, (float)currentBoundFBO->GetNativeWidth(), (float)currentBoundFBO->GetNativeHeight());
            currentBoundFBO->SetDirty(true);
        }

        GLuint tex = 0;
        if (dc.texAddrPhys != 0 && dc.texW > 0 && dc.texH > 0) {
            tex = GetOrCreateTexture(dc.texAddrPhys, dc.texW, dc.texH, dc.texType, os);
        }

        if (tex != 0) {
            if (tex != lastBoundTex) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(m_uTexture, 0);
                glUniform1i(m_uHasTexture, 1);
                lastBoundTex = tex;
            }
        } else {
            glUniform1i(m_uHasTexture, 0);
            lastBoundTex = 0;
        }

        glBindBuffer(GL_ARRAY_BUFFER, m_uiVbo);
        glBufferData(GL_ARRAY_BUFFER, dc.vertices.size() * sizeof(PicaVertex), dc.vertices.data(), GL_DYNAMIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_uiIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, dc.indices.size() * sizeof(uint16_t), dc.indices.data(), GL_DYNAMIC_DRAW);

        glDrawElements(GL_TRIANGLES, (GLsizei)dc.indices.size(), GL_UNSIGNED_SHORT, 0);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
}

void PicaGLES::RenderFrame(const uint8_t* topPixels, const uint8_t* botPixels) {
    static int s_frameLog = 0;
    if (++s_frameLog % 60 == 1) {
        LOGI("RenderFrame: topFBO_tex=%u dirty=%d, botFBO_tex=%u dirty=%d (res: %dx%d)",
             m_topFBO.GetTexture(), (int)m_topFBO.IsDirty(),
             m_botFBO.GetTexture(), (int)m_botFBO.IsDirty(),
             m_windowWidth, m_windowHeight);
    }

    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);

#if PICA_RENDERER_GPU
    // No modo GPU nativo: apresenta diretamente as texturas dos FBOs (Zero cópia para CPU!)
    // FBO em OpenGL tem origem no canto inferior esquerdo, portanto uFlipY = 1 é necessário
    GLuint topTex = m_topFBO.GetTexture();
    GLuint botTex = m_botFBO.GetTexture();
    glUniform1i(m_uFlipY, 1);
#else
    // Modo legado: upload de pixels de memória da CPU
    if (topPixels) {
        glBindTexture(GL_TEXTURE_2D, m_topTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_WIDTH, TOP_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, topPixels);
    }
    if (botPixels) {
        glBindTexture(GL_TEXTURE_2D, m_botTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, BOT_WIDTH, BOT_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, botPixels);
    }

    GLuint topTex = m_topTexture;
    GLuint botTex = m_botTexture;
    glUniform1i(m_uFlipY, 0);
#endif

    int halfW = m_windowWidth / 2;

    // Tela Superior: 400x240 (5:3)
    float targetRatioTop = 400.0f / 240.0f;
    int topW = halfW;
    int topH = (int)(halfW / targetRatioTop);
    if (topH > m_windowHeight) {
        topH = m_windowHeight;
        topW = (int)(m_windowHeight * targetRatioTop);
    }
    int topX = (halfW - topW) / 2;
    int topY = (m_windowHeight - topH) / 2;

    glViewport(topX, topY, topW, topH);
    glBindTexture(GL_TEXTURE_2D, topTex);
    glUniform1i(m_uScreenTexture, 0);

    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(m_uTransform, 1, GL_FALSE, identity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Tela Inferior: 400x240 (5:3)
    int botW = topW;
    int botH = topH;
    int botX = halfW + (halfW - botW) / 2;
    int botY = (m_windowHeight - botH) / 2;

    glViewport(botX, botY, botW, botH);
    glBindTexture(GL_TEXTURE_2D, botTex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glUseProgram(0);
}

void PicaGLES::DestroyGL() {
    m_topFBO.Destroy();
    m_botFBO.Destroy();

    if (m_topTexture) glDeleteTextures(1, &m_topTexture);
    if (m_botTexture) glDeleteTextures(1, &m_botTexture);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_program) glDeleteProgram(m_program);

    if (m_uiVao) glDeleteVertexArrays(1, &m_uiVao);
    if (m_uiVbo) glDeleteBuffers(1, &m_uiVbo);
    if (m_uiIbo) glDeleteBuffers(1, &m_uiIbo);
    if (m_uiProgram) glDeleteProgram(m_uiProgram);

    for (auto& pair : m_textureCache) {
        if (pair.second) {
            glDeleteTextures(1, &pair.second);
        }
    }
    m_textureCache.clear();

    m_topTexture = 0;
    m_botTexture = 0;
    m_program = 0;
    m_uiProgram = 0;
}

