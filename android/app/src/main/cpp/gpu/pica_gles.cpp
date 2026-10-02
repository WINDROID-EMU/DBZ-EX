#include "pica_gles.h"
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
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = uTransform * vec4(aPosition, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
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

PicaGLES::PicaGLES()
    : m_windowWidth(1280),
      m_windowHeight(720),
      m_topTexture(0),
      m_botTexture(0),
      m_program(0),
      m_vao(0),
      m_vbo(0) {}

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

    // Quad geometry: pos(x, y), uv(u, v)
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
    LOGI("Initializing OpenGL ES 3.0 PICA200 Presentation Pipeline...");
    CreateTextures();
    BuildShaders();
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.05f, 0.15f, 1.0f);
    return true;
}

void PicaGLES::Resize(int windowWidth, int windowHeight) {
    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;
    glViewport(0, 0, windowWidth, windowHeight);
}

void PicaGLES::RenderFrame(const uint8_t* topPixels, const uint8_t* botPixels) {
    static int s_renderLog = 0;
    if (++s_renderLog % 120 == 0) {
        uint32_t topNonZero = 0;
        if (topPixels) {
            for (size_t i = 0; i < TOP_WIDTH * TOP_HEIGHT * 4; i += 16) {
                if (topPixels[i] != 0) topNonZero++;
            }
        }
        LOGI("PicaGLES::RenderFrame called: topPixels=%p (nonZeroSample=%u), botPixels=%p (%dx%d)",
             topPixels, topNonZero, botPixels, m_windowWidth, m_windowHeight);
    }

    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);

    // Upload Top Screen Pixels
    if (topPixels) {
        glBindTexture(GL_TEXTURE_2D, m_topTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOP_WIDTH, TOP_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, topPixels);
    }

    // Upload Bottom Screen Pixels
    if (botPixels) {
        glBindTexture(GL_TEXTURE_2D, m_botTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, BOT_WIDTH, BOT_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, botPixels);
    }

    // Render Top Screen in viewport (e.g. Left/Top half or Main Screen)
    // Landscape Mode: Top Screen centered on Left / Main, Bottom Screen on Right
    int halfW = m_windowWidth / 2;
    glViewport(0, 0, halfW, m_windowHeight);
    glBindTexture(GL_TEXTURE_2D, m_topTexture);
    glUniform1i(m_uScreenTexture, 0);

    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(m_uTransform, 1, GL_FALSE, identity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Render Bottom Screen in viewport
    glViewport(halfW, 0, halfW, m_windowHeight);
    glBindTexture(GL_TEXTURE_2D, m_botTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glUseProgram(0);
}

void PicaGLES::DestroyGL() {
    if (m_topTexture) glDeleteTextures(1, &m_topTexture);
    if (m_botTexture) glDeleteTextures(1, &m_botTexture);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_program) glDeleteProgram(m_program);
    m_topTexture = 0;
    m_botTexture = 0;
}
