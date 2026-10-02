#include "pica_cmd_processor.h"
#include "pica_registers.h"
#include "../horizon/horizon_os.h"
#include "rg_etc1.h"
#include <string.h>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "PicaCmdProc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

PicaCommandProcessor::PicaCommandProcessor() {
    memset(m_regs, 0, sizeof(m_regs));
    pthread_mutex_init(&m_gpuQueueMutex, nullptr);
}

PicaCommandProcessor::~PicaCommandProcessor() {
    pthread_mutex_destroy(&m_gpuQueueMutex);
}

void PicaCommandProcessor::FetchGPUQueue(std::vector<PicaDrawCall>& outQueue) {
    pthread_mutex_lock(&m_gpuQueueMutex);
    outQueue.swap(m_gpuQueue);
    m_gpuQueue.clear();
    pthread_mutex_unlock(&m_gpuQueueMutex);
}

bool PicaCommandProcessor::HasQueuedDraws() {
    pthread_mutex_lock(&m_gpuQueueMutex);
    bool has = !m_gpuQueue.empty();
    pthread_mutex_unlock(&m_gpuQueueMutex);
    return has;
}

void PicaCommandProcessor::ClearGPUQueue() {
    pthread_mutex_lock(&m_gpuQueueMutex);
    m_gpuQueue.clear();
    pthread_mutex_unlock(&m_gpuQueueMutex);
}

void PicaCommandProcessor::QueueClear(bool isBottom, float r, float g, float b, float a) {
    PicaDrawCall dc;
    dc.isClear = true;
    dc.isBottom = isBottom;
    dc.clearColor[0] = r;
    dc.clearColor[1] = g;
    dc.clearColor[2] = b;
    dc.clearColor[3] = a;

    pthread_mutex_lock(&m_gpuQueueMutex);
    m_gpuQueue.push_back(std::move(dc));
    pthread_mutex_unlock(&m_gpuQueueMutex);
}

uint32_t PicaCommandProcessor::GetRegister(uint32_t regId) const {
    if (regId < 0x400) return m_regs[regId];
    return 0;
}

void PicaCommandProcessor::SetRegister(uint32_t regId, uint32_t value, uint32_t mask) {
    if (regId >= 0x400) return;

    if (mask == 0xF || mask == 0) {
        m_regs[regId] = value;
    } else {
        uint32_t current = m_regs[regId];
        uint32_t writeMask = 0;
        if (mask & 1) writeMask |= 0x000000FF;
        if (mask & 2) writeMask |= 0x0000FF00;
        if (mask & 4) writeMask |= 0x00FF0000;
        if (mask & 8) writeMask |= 0xFF000000;
        m_regs[regId] = (current & ~writeMask) | (value & writeMask);
    }
}

void PicaCommandProcessor::ProcessCommandList(const uint32_t* buffer, size_t wordCount, HorizonOS* os) {
    if (!buffer || wordCount < 2) return;

    size_t idx = 0;
    while (idx + 1 < wordCount) {
        uint32_t param0 = buffer[idx];
        uint32_t header = buffer[idx + 1];

        uint32_t reg = header & 0x3FF;
        uint32_t mask = (header >> 16) & 0xF;
        uint32_t extra = (header >> 20) & 0xFF;
        bool inc = (header >> 31) != 0;

        SetRegister(reg, param0, mask);
        idx += 2;

        if (reg == GPUREG_DRAWARRAYS) {
            ExecuteDrawArrays(os);
        } else if (reg == GPUREG_DRAWELEMENTS) {
            ExecuteDrawElements(os);
        }

        for (uint32_t e = 0; e < extra && idx < wordCount; ++e, ++idx) {
            uint32_t targetReg = inc ? (reg + 1 + e) : reg;
            if (targetReg < 0x400) {
                SetRegister(targetReg, buffer[idx], mask);
                if (targetReg == GPUREG_DRAWARRAYS) {
                    ExecuteDrawArrays(os);
                } else if (targetReg == GPUREG_DRAWELEMENTS) {
                    ExecuteDrawElements(os);
                }
            }
        }

        // Align to 8 bytes if extra count was odd
        if ((extra & 1) != 0 && idx < wordCount) {
            idx++;
        }
    }
}

void PicaCommandProcessor::ExecuteDrawArrays(HorizonOS* os) {
    m_drawCallCount++;
    uint32_t numVertices = m_regs[GPUREG_NUMVERTICES];
    uint32_t vtxBufPhys = m_regs[GPUREG_ATTRIBBUFFERS_LOC] << 3;
    uint32_t colorBufPhys = m_regs[GPUREG_COLORBUFFER_LOC] << 3;
    uint32_t texAddrPhys = m_regs[GPUREG_TEXUNIT0_ADDR1] << 3;
    uint32_t texDim = m_regs[GPUREG_TEXUNIT0_DIM];
    uint32_t texW = texDim & 0xFFFF;
    uint32_t texH = (texDim >> 16) & 0xFFFF;

    static int s_drawLog = 0;
    if (++s_drawLog % 30 == 1) {
        LOGI("ExecuteDrawArrays [#%u]: vertices=%u, vtxBuf=0x%08X, colorBuf=0x%08X, tex=0x%08X (%ux%u)",
             m_drawCallCount, numVertices, vtxBufPhys, colorBufPhys, texAddrPhys, texW, texH);
    }
}

void PicaCommandProcessor::ExecuteDrawElements(HorizonOS* os) {
    m_drawCallCount++;
    uint32_t numVertices = m_regs[GPUREG_NUMVERTICES];
    uint32_t idxBufConfig = m_regs[GPUREG_INDEXBUFFER_CONFIG];
    uint32_t colorBufPhys = m_regs[GPUREG_COLORBUFFER_LOC] << 3;
    uint32_t texAddrPhys = m_regs[GPUREG_TEXUNIT0_ADDR1] << 3;
    uint32_t texDim = m_regs[GPUREG_TEXUNIT0_DIM];
    uint32_t texW = texDim & 0xFFFF;
    uint32_t texH = (texDim >> 16) & 0xFFFF;
    uint32_t texType = m_regs[GPUREG_TEXUNIT0_TYPE];
    uint32_t fbDim = m_regs[GPUREG_FRAMEBUFFER_DIM];
    uint32_t fbFmt = m_regs[GPUREG_COLORBUFFER_FORMAT];
    uint32_t vtxBufPhys = m_regs[GPUREG_ATTRIBBUFFERS_LOC] << 3;

#ifdef DEBUG_PICA_GPU
    static int s_elemLog = 0;
    if (++s_elemLog % 60 == 1) {
        uint32_t bufLoc = m_regs[GPUREG_ATTRIBBUFFERS_LOC] << 3;
        uint32_t buf0Off = m_regs[GPUREG_ATTRIBBUFFER0_OFFSET];
        uint32_t idxAddr = bufLoc + (idxBufConfig & 0x0FFFFFFFU);
        LOGI("ExecuteDrawElements [#%u]: verts=%u, idxBuf=0x%08X (addr=0x%08X), colorBuf=0x%08X, tex=0x%08X (%ux%u, type=%u)",
             m_drawCallCount, numVertices, idxBufConfig, idxAddr, colorBufPhys, texAddrPhys, texW, texH, texType);
    }
#endif

#if PICA_RENDERER_GPU
    // --- Caminho nativo Adreno GPU: monta e enfileira draw call para o FBO ---
    bool isBottom = (GetScreenTarget(colorBufPhys) == PicaScreenTarget::Bottom);

    static int s_elemGpuLog = 0;
    if (++s_elemGpuLog % 60 == 1) {
        LOGI("ExecuteDrawElements [GPU]: verts=%u, colorBuf=0x%08X, isBottom=%d, tex=0x%08X (%ux%u, type=%u) fbDim=0x%08X vpW=0x%08X vpH=0x%08X vpXY=0x%08X",
             numVertices, colorBufPhys, (int)isBottom, texAddrPhys, texW, texH, texType,
             m_regs[GPUREG_FRAMEBUFFER_DIM], m_regs[GPUREG_VIEWPORT_WIDTH],
             m_regs[GPUREG_VIEWPORT_HEIGHT], m_regs[GPUREG_VIEWPORT_XY]);
    }

    uint32_t bufBase = m_regs[GPUREG_ATTRIBBUFFERS_LOC] << 3;
    uint32_t b0Off = m_regs[GPUREG_ATTRIBBUFFER0_OFFSET];
    uint32_t b2Off = m_regs[0x0209];
    uint32_t idxAddr = bufBase + (idxBufConfig & 0x0FFFFFFFU);

    const float* posPtr = os ? (const float*)os->GetPointer(bufBase + b0Off) : nullptr;
    const float* uvPtr  = os ? (const float*)os->GetPointer(bufBase + b2Off) : nullptr;
    const uint8_t* idxPtr = os ? (const uint8_t*)os->GetPointer(idxAddr) : nullptr;
    bool is16Bit = (idxBufConfig & (1U << 31)) != 0;

    uint32_t b1Off = m_regs[0x0206];
    uint32_t stride1 = (m_regs[0x0208] >> 16) & 0xFF;
    const float* colPtr = (os && b1Off > 0 && stride1 >= 16) ? (const float*)os->GetPointer(bufBase + b1Off) : nullptr;

    if (posPtr && uvPtr && idxPtr && numVertices >= 3) {
        float minX = 1e9f, maxX = -1e9f;
        float minY = 1e9f, maxY = -1e9f;
        for (uint32_t i = 0; i < numVertices; ++i) {
            uint32_t idx = is16Bit ? ((const uint16_t*)idxPtr)[i] : idxPtr[i];
            float vx = posPtr[idx * 3 + 0];
            float vy = posPtr[idx * 3 + 1];
            if (vx < minX) minX = vx;
            if (vx > maxX) maxX = vx;
            if (vy < minY) minY = vy;
            if (vy > maxY) maxY = vy;
        }
        bool isScreenSpace = (minX >= -5.0f && maxY <= 1.0f) || (maxX > 210.0f);

        float L_W = 400.0f;
        float L_H = 240.0f;
        float cx = 200.0f;
        float cy = 120.0f;

        PicaDrawCall dc;
        dc.isBottom = isBottom;
        dc.texAddrPhys = texAddrPhys;
        dc.texW = texW;
        dc.texH = texH;
        dc.texType = texType;

        dc.indices.reserve(numVertices);
        uint32_t maxIdx = 0;
        for (uint32_t i = 0; i < numVertices; ++i) {
            uint32_t idx = is16Bit ? ((const uint16_t*)idxPtr)[i] : idxPtr[i];
            dc.indices.push_back((uint16_t)idx);
            if (idx > maxIdx) maxIdx = idx;
        }

        dc.vertices.resize(maxIdx + 1);
        for (uint32_t idx = 0; idx <= maxIdx; ++idx) {
            float x = posPtr[idx * 3 + 0];
            float y = posPtr[idx * 3 + 1];
            float z = posPtr[idx * 3 + 2];

            float sx, sy;
            if (isScreenSpace) {
                sx = x;
                sy = (y < 0) ? -y : y;
            } else {
                sx = cx + x;
                sy = cy - y;
            }

            dc.vertices[idx].x = sx;
            dc.vertices[idx].y = sy;
            dc.vertices[idx].z = z;
            dc.vertices[idx].u = uvPtr[idx * 2 + 0];
            dc.vertices[idx].v = uvPtr[idx * 2 + 1];

            if (colPtr) {
                dc.vertices[idx].r = colPtr[idx * 4 + 0];
                dc.vertices[idx].g = colPtr[idx * 4 + 1];
                dc.vertices[idx].b = colPtr[idx * 4 + 2];
                dc.vertices[idx].a = colPtr[idx * 4 + 3];
            } else {
                dc.vertices[idx].r = 1.0f;
                dc.vertices[idx].g = 1.0f;
                dc.vertices[idx].b = 1.0f;
                dc.vertices[idx].a = 1.0f;
            }
        }

        pthread_mutex_lock(&m_gpuQueueMutex);
        m_gpuQueue.push_back(std::move(dc));
        pthread_mutex_unlock(&m_gpuQueueMutex);
    }
#else
    // --- Caminho legado: Rasterizador em CPU ---
    // Morton LUT for 8x8 tile
    static const uint32_t xLut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint32_t yLut[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };

    // Native 3DS framebuffers are portrait: 240x400 (top) and 240x320 (bottom)
    bool isBottom = (colorBufPhys == 0x18177000) || ((colorBufPhys & 0x00FFFFFF) == 0x00177000) ||
                    (colorBufPhys >= 0x18200000);
    uint32_t fbW = 240;
    uint32_t fbH = 400; // Both 3DS VRAM render targets are 240x400 (with bottom 320 centered)
    uint8_t* fbPtr = os ? os->GetPointer(colorBufPhys) : nullptr;
    uint8_t* texPtr = os ? os->GetPointer(texAddrPhys) : nullptr;

    if (fbPtr && texPtr && texW > 0 && texH > 0) {
        // Texture sampler supporting ETC1A4 (13), ETC1 (12), and uncompressed RGBA8 (0)
        uint32_t lastBlockIdx = 0xFFFFFFFF;
        uint32_t cachedRGBA[16] = {0};
        uint64_t cachedAlphaPacked = 0;
        uint32_t tilesPerRow = (texW + 7) / 8;

        auto sampleTex = [&](int tx, int ty, uint8_t& outR, uint8_t& outG, uint8_t& outB, uint8_t& outA) {
            tx = std::clamp(tx, 0, (int)texW - 1);
            ty = std::clamp(ty, 0, (int)texH - 1);
            uint32_t tileX = (uint32_t)tx / 8;
            uint32_t tileY = (uint32_t)ty / 8;
            uint32_t fineX = (uint32_t)tx % 8;
            uint32_t fineY = (uint32_t)ty % 8;

            if (texType == 13) { // GPU_ETC1A4 (16 bytes per 4x4 subtile, 64 bytes per 8x8 tile)
                uint32_t subBx = fineX / 4;
                uint32_t subBy = fineY / 4;
                uint32_t subBlockIdx = subBx + 2 * subBy;
                uint32_t blockOffset = (tileY * tilesPerRow + tileX) * 64 + subBlockIdx * 16;

                if (blockOffset != lastBlockIdx) {
                    lastBlockIdx = blockOffset;
                    const uint8_t* blk = texPtr + blockOffset;
                    memcpy(&cachedAlphaPacked, blk, sizeof(uint64_t));

                    uint8_t rev[8];
                    for (int b = 0; b < 8; ++b) {
                        rev[b] = blk[8 + (7 - b)];
                    }
                    rg_etc1::unpack_etc1_block(rev, cachedRGBA, false);
                }

                uint32_t subX = fineX % 4;
                uint32_t subY = fineY % 4;
                // Alpha in 3DS ETC1A4: 4 bits per texel, indexed by 4 * (subX * 4 + subY)
                uint32_t aShift = 4 * (subX * 4 + subY);
                uint8_t a4 = (uint8_t)((cachedAlphaPacked >> aShift) & 0x0F);
                outA = (a4 << 4) | a4;

                uint32_t pIdx = subY * 4 + subX;
                uint32_t col = cachedRGBA[pIdx];
                outR = col & 0xFF;
                outG = (col >> 8) & 0xFF;
                outB = (col >> 16) & 0xFF;
            } else if (texType == 12) { // GPU_ETC1 (8 bytes per 4x4 subtile, 32 bytes per 8x8 tile)
                uint32_t subBx = fineX / 4;
                uint32_t subBy = fineY / 4;
                uint32_t subBlockIdx = subBx + 2 * subBy;
                uint32_t blockOffset = (tileY * tilesPerRow + tileX) * 32 + subBlockIdx * 8;

                if (blockOffset != lastBlockIdx) {
                    lastBlockIdx = blockOffset;
                    const uint8_t* blk = texPtr + blockOffset;
                    uint8_t rev[8];
                    for (int b = 0; b < 8; ++b) {
                        rev[b] = blk[7 - b];
                    }
                    rg_etc1::unpack_etc1_block(rev, cachedRGBA, false);
                }

                uint32_t subX = fineX % 4;
                uint32_t subY = fineY % 4;
                uint32_t pIdx = subY * 4 + subX;
                outA = 0xFF;
                uint32_t col = cachedRGBA[pIdx];
                outR = col & 0xFF;
                outG = (col >> 8) & 0xFF;
                outB = (col >> 16) & 0xFF;
            } else { // RGBA8 (uncompressed 8x8 Morton tile, 256 bytes per tile)
                uint32_t texOff = (tileY * tilesPerRow + tileX) * 256 + (xLut[fineX] + yLut[fineY]) * 4;
                outA = texPtr[texOff + 0]; // 3DS RGBA8 layout: [0]=A, [1]=B, [2]=G, [3]=R
                outB = texPtr[texOff + 1];
                outG = texPtr[texOff + 2];
                outR = texPtr[texOff + 3];
            }
        };

        // Vertex & Index buffers
        uint32_t bufBase = m_regs[GPUREG_ATTRIBBUFFERS_LOC] << 3;
        uint32_t b0Off = m_regs[GPUREG_ATTRIBBUFFER0_OFFSET];
        uint32_t b2Off = m_regs[0x0209];
        uint32_t idxAddr = bufBase + (idxBufConfig & 0x0FFFFFFFU);

        const float* posPtr = os ? (const float*)os->GetPointer(bufBase + b0Off) : nullptr;
        const float* uvPtr  = os ? (const float*)os->GetPointer(bufBase + b2Off) : nullptr;
        const uint8_t* idxPtr = os ? (const uint8_t*)os->GetPointer(idxAddr) : nullptr;
        bool is16Bit = (idxBufConfig & (1U << 31)) != 0;

        uint32_t b1Off = m_regs[0x0206];
        uint32_t stride0 = (m_regs[GPUREG_ATTRIBBUFFER0_CONFIG2] >> 16) & 0xFF;
        uint32_t stride1 = (m_regs[0x0208] >> 16) & 0xFF;
        uint32_t stride2 = (m_regs[0x020B] >> 16) & 0xFF;

        static int s_attrLog = 0;
        if (++s_attrLog % 60 == 1) {
            LOGI("DrawElements attribs: b0=0x%X(str=%u) b1=0x%X(str=%u) b2=0x%X(str=%u) fmtLow=0x%08X fmtHigh=0x%08X texenv0_col=0x%08X blend_col=0x%08X col_op=0x%08X depth_mask=0x%08X",
                 b0Off, stride0, b1Off, stride1, b2Off, stride2,
                 m_regs[GPUREG_ATTRIBBUFFERS_FORMAT_LOW], m_regs[GPUREG_ATTRIBBUFFERS_FORMAT_HIGH],
                 m_regs[0x00C2], m_regs[0x0103], m_regs[0x0100], m_regs[0x0107]);
        }

        float L_W = isBottom ? 320.0f : 400.0f;
        float L_H = 240.0f;
        float cx = L_W / 2.0f;
        float cy = L_H / 2.0f;

        const float* colPtr = (os && b1Off > 0 && stride1 >= 16) ? (const float*)os->GetPointer(bufBase + b1Off) : nullptr;

        if (posPtr && uvPtr && idxPtr && numVertices >= 3) {
            auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
                return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
            };

            // Detect whether coordinates are in direct screen-space [0..W, 0..-H] or centered [-W/2..W/2, -H/2..H/2]
            float minX = 1e9f, maxX = -1e9f;
            float minY = 1e9f, maxY = -1e9f;
            for (uint32_t i = 0; i < numVertices; ++i) {
                uint32_t idx = is16Bit ? ((const uint16_t*)idxPtr)[i] : idxPtr[i];
                float vx = posPtr[idx * 3 + 0];
                float vy = posPtr[idx * 3 + 1];
                if (vx < minX) minX = vx;
                if (vx > maxX) maxX = vx;
                if (vy < minY) minY = vy;
                if (vy > maxY) maxY = vy;
            }
            bool isScreenSpace = (minX >= -5.0f && maxY <= 1.0f) || (maxX > 210.0f);

            for (uint32_t i = 0; i + 2 < numVertices; i += 3) {
                uint32_t i0 = is16Bit ? ((const uint16_t*)idxPtr)[i + 0] : idxPtr[i + 0];
                uint32_t i1 = is16Bit ? ((const uint16_t*)idxPtr)[i + 1] : idxPtr[i + 1];
                uint32_t i2 = is16Bit ? ((const uint16_t*)idxPtr)[i + 2] : idxPtr[i + 2];

                float x0 = posPtr[i0 * 3 + 0], y0 = posPtr[i0 * 3 + 1];
                float x1 = posPtr[i1 * 3 + 0], y1 = posPtr[i1 * 3 + 1];
                float x2 = posPtr[i2 * 3 + 0], y2 = posPtr[i2 * 3 + 1];

                float u0 = uvPtr[i0 * 2 + 0] * (float)texW, v0 = uvPtr[i0 * 2 + 1] * (float)texH;
                float u1 = uvPtr[i1 * 2 + 0] * (float)texW, v1 = uvPtr[i1 * 2 + 1] * (float)texH;
                float u2 = uvPtr[i2 * 2 + 0] * (float)texW, v2 = uvPtr[i2 * 2 + 1] * (float)texH;

                float cr0 = 1.0f, cg0 = 1.0f, cb0 = 1.0f, ca0 = 1.0f;
                float cr1 = 1.0f, cg1 = 1.0f, cb1 = 1.0f, ca1 = 1.0f;
                float cr2 = 1.0f, cg2 = 1.0f, cb2 = 1.0f, ca2 = 1.0f;
                if (colPtr) {
                    cr0 = colPtr[i0 * 4 + 0]; cg0 = colPtr[i0 * 4 + 1]; cb0 = colPtr[i0 * 4 + 2]; ca0 = colPtr[i0 * 4 + 3];
                    cr1 = colPtr[i1 * 4 + 0]; cg1 = colPtr[i1 * 4 + 1]; cb1 = colPtr[i1 * 4 + 2]; ca1 = colPtr[i1 * 4 + 3];
                    cr2 = colPtr[i2 * 4 + 0]; cg2 = colPtr[i2 * 4 + 1]; cb2 = colPtr[i2 * 4 + 2]; ca2 = colPtr[i2 * 4 + 3];
                }

                float sx0, sy0, sx1, sy1, sx2, sy2;
                if (isScreenSpace) {
                    sx0 = x0; sy0 = (y0 < 0) ? -y0 : y0;
                    sx1 = x1; sy1 = (y1 < 0) ? -y1 : y1;
                    sx2 = x2; sy2 = (y2 < 0) ? -y2 : y2;
                } else {
                    sx0 = cx + x0; sy0 = cy - y0;
                    sx1 = cx + x1; sy1 = cy - y1;
                    sx2 = cx + x2; sy2 = cy - y2;
                }

#ifdef DEBUG_PICA_GPU
                static int s_triLog = 0;
                if (++s_triLog % 60 == 1) {
                    LOGI("DrawElements Tri: colorBuf=0x%08X isBottom=%d scrSpace=%d pos0=(%.1f, %.1f) pos1=(%.1f, %.1f) pos2=(%.1f, %.1f) uv0=(%.1f, %.1f) uv1=(%.1f, %.1f) col0=(%.2f, %.2f, %.2f, %.2f)",
                         colorBufPhys, isBottom, isScreenSpace, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, cr0, cg0, cb0, ca0);
                }
#endif

                float area = edge(sx0, sy0, sx1, sy1, sx2, sy2);
                if (std::abs(area) < 0.0001f) continue;

                int minSX = std::max(0, (int)std::floor(std::min({sx0, sx1, sx2})));
                int maxSX = std::min((int)L_W - 1, (int)std::ceil(std::max({sx0, sx1, sx2})));
                int minSY = std::max(0, (int)std::floor(std::min({sy0, sy1, sy2})));
                int maxSY = std::min((int)L_H - 1, (int)std::ceil(std::max({sy0, sy1, sy2})));

                for (int sy = minSY; sy <= maxSY; ++sy) {
                    float py = (float)sy + 0.5f;
                    int pX = 239 - sy; // 3DS portrait X is inverted landscape Y
                    for (int sx = minSX; sx <= maxSX; ++sx) {
                        float px = (float)sx + 0.5f;
                        int pY = sx + (isBottom ? 40 : 0); // Bottom screen is centered at offset 40 in 240x400 buffer

                        float w0 = edge(sx1, sy1, sx2, sy2, px, py) / area;
                        float w1 = edge(sx2, sy2, sx0, sy0, px, py) / area;
                        float w2 = edge(sx0, sy0, sx1, sy1, px, py) / area;

                        if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f) {
                            float u = w0 * u0 + w1 * u1 + w2 * u2;
                            float v = w0 * v0 + w1 * v1 + w2 * v2;

                            uint8_t r, g, b, a;
                            sampleTex((int)std::round(u), (int)std::round(v), r, g, b, a);

                            float cr = w0 * cr0 + w1 * cr1 + w2 * cr2;
                            float cg = w0 * cg0 + w1 * cg1 + w2 * cg2;
                            float cb = w0 * cb0 + w1 * cb1 + w2 * cb2;
                            float ca = w0 * ca0 + w1 * ca1 + w2 * ca2;

                            uint8_t finalR = (uint8_t)std::clamp((float)r * cr, 0.0f, 255.0f);
                            uint8_t finalG = (uint8_t)std::clamp((float)g * cg, 0.0f, 255.0f);
                            uint8_t finalB = (uint8_t)std::clamp((float)b * cb, 0.0f, 255.0f);
                            uint8_t finalA = (uint8_t)std::clamp((float)a * ca, 0.0f, 255.0f);

                            if (finalA > 4) {
                                uint32_t xMod = (uint32_t)pX & 7U;
                                uint32_t yMod = (uint32_t)pY & 7U;
                                uint32_t fbOff = (xLut[xMod] + yLut[yMod] + ((uint32_t)pX & ~7U) * 8U) * 4 + ((uint32_t)pY & ~7U) * fbW * 4;
                                if (fbOff + 3 < fbW * fbH * 4) {
                                    if (finalA >= 250) {
                                        // Native 3DS RGBA8 framebuffer order is [0]=A, [1]=B, [2]=G, [3]=R
                                        fbPtr[fbOff + 0] = finalA;
                                        fbPtr[fbOff + 1] = finalB;
                                        fbPtr[fbOff + 2] = finalG;
                                        fbPtr[fbOff + 3] = finalR;
                                    } else {
                                        uint32_t invA = 255 - finalA;
                                        fbPtr[fbOff + 0] = finalA;
                                        fbPtr[fbOff + 1] = (finalB * finalA + fbPtr[fbOff + 1] * invA) / 255;
                                        fbPtr[fbOff + 2] = (finalG * finalA + fbPtr[fbOff + 2] * invA) / 255;
                                        fbPtr[fbOff + 3] = (finalR * finalA + fbPtr[fbOff + 3] * invA) / 255;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
#endif // !PICA_RENDERER_GPU
}

