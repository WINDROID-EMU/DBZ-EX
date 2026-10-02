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
}

PicaCommandProcessor::~PicaCommandProcessor() {}

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

    static int s_elemLog = 0;
    if (++s_elemLog % 30 == 1) {
        uint32_t bufLoc = m_regs[GPUREG_ATTRIBBUFFERS_LOC] << 3;
        uint32_t fmtLow = m_regs[GPUREG_ATTRIBBUFFERS_FORMAT_LOW];
        uint32_t fmtHigh = m_regs[GPUREG_ATTRIBBUFFERS_FORMAT_HIGH];
        uint32_t buf0Off = m_regs[GPUREG_ATTRIBBUFFER0_OFFSET];
        uint32_t buf0Cfg1 = m_regs[GPUREG_ATTRIBBUFFER0_CONFIG1];
        uint32_t buf0Cfg2 = m_regs[GPUREG_ATTRIBBUFFER0_CONFIG2];
        uint32_t vtxOff = m_regs[GPUREG_VERTEX_OFFSET];

        uint8_t texSample[16] = {0};
        uint8_t vtxSample[48] = {0};
        uint8_t idxSample[16] = {0};
        uint32_t vtxAddr = bufLoc + buf0Off;
        uint32_t idxAddr = bufLoc + (idxBufConfig & 0x0FFFFFFFU);
        if (os) {
            os->ReadBytes(texAddrPhys, texSample, 16);
            os->ReadBytes(vtxAddr, vtxSample, 48);
            os->ReadBytes(idxAddr, idxSample, 16);
        }
        float* fVtx = (float*)vtxSample;
        uint32_t vpW = m_regs[GPUREG_VIEWPORT_WIDTH];
        uint32_t vpH = m_regs[GPUREG_VIEWPORT_HEIGHT];
        uint32_t vpXY = m_regs[GPUREG_VIEWPORT_XY];
        LOGI("ExecuteDrawElements [#%u]: verts=%u, idxBuf=0x%08X (addr=0x%08X), colorBuf=0x%08X, tex=0x%08X (%ux%u, type=%u)",
             m_drawCallCount, numVertices, idxBufConfig, idxAddr, colorBufPhys, texAddrPhys, texW, texH, texType);
        LOGI("   Viewport: W=0x%X (float=%.2f) H=0x%X (float=%.2f) XY=0x%X",
             vpW, *(float*)&vpW, vpH, *(float*)&vpH, vpXY);
        LOGI("   idxSample: %02X %02X %02X %02X %02X %02X %02X %02X",
             idxSample[0], idxSample[1], idxSample[2], idxSample[3],
             idxSample[4], idxSample[5], idxSample[6], idxSample[7]);

        uint32_t buf1Off = m_regs[0x0206];
        uint32_t buf1Cfg2 = m_regs[0x0208];
        uint8_t buf1Sample[32] = {0};
        uint32_t buf1Addr = bufLoc + buf1Off;
        if (os && buf1Off > 0) {
            os->ReadBytes(buf1Addr, buf1Sample, 32);
        }
        float* fVtx1 = (float*)buf1Sample;
        uint32_t buf2Off = m_regs[0x0209];
        uint32_t buf2Cfg2 = m_regs[0x020B];
        uint8_t buf2Sample[32] = {0};
        uint32_t buf2Addr = bufLoc + buf2Off;
        if (os && buf2Off > 0) {
            os->ReadBytes(buf2Addr, buf2Sample, 32);
        }
        float* fVtx2 = (float*)buf2Sample;
        LOGI("   buf0Off=0x%X stride0=%u, buf2Off=0x%X stride2=%u",
             buf0Off, (buf0Cfg2 >> 16) & 0xFF, buf2Off, (buf2Cfg2 >> 16) & 0xFF);
        LOGI("   v0: (%.2f, %.2f, %.2f)  v1: (%.2f, %.2f, %.2f)  v2: (%.2f, %.2f, %.2f)  v3: (%.2f, %.2f, %.2f)",
             fVtx[0], fVtx[1], fVtx[2], fVtx[3], fVtx[4], fVtx[5], fVtx[6], fVtx[7], fVtx[8], fVtx[9], fVtx[10], fVtx[11]);
        LOGI("   uv0: (%.3f, %.3f)  uv1: (%.3f, %.3f)  uv2: (%.3f, %.3f)  uv3: (%.3f, %.3f)",
             fVtx2[0], fVtx2[1], fVtx2[2], fVtx2[3], fVtx2[4], fVtx2[5], fVtx2[6], fVtx2[7]);
        LOGI("   texSample: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             texSample[0], texSample[1], texSample[2], texSample[3],
             texSample[4], texSample[5], texSample[6], texSample[7],
             texSample[8], texSample[9], texSample[10], texSample[11],
             texSample[12], texSample[13], texSample[14], texSample[15]);
    }

    // Morton LUT for 8x8 tile
    static const uint32_t xLut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint32_t yLut[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };

    // Native 3DS framebuffers are portrait: 240x400 (top) and 240x320 (bottom)
    bool isBottom = (colorBufPhys >= 0x18200000) && (colorBufPhys != 0x18177000);
    uint32_t fbW = 240;
    uint32_t fbH = isBottom ? 320 : 400;
    uint8_t* fbPtr = os ? os->GetPointer(colorBufPhys) : nullptr;
    uint8_t* texPtr = os ? os->GetPointer(texAddrPhys) : nullptr;

    if (fbPtr && texPtr && texW > 0 && texH > 0) {
        // Texture sampler supporting ETC1A4 (13), ETC1 (12), and uncompressed RGBA8 (0)
        uint32_t lastBlockIdx = 0xFFFFFFFF;
        uint32_t cachedRGBA[16] = {0};
        uint8_t cachedAlpha[16] = {0};
        uint32_t tilesPerCol = (texH + 7) / 8;

        auto sampleTex = [&](int tx, int ty, uint8_t& outR, uint8_t& outG, uint8_t& outB, uint8_t& outA) {
            if (tx < 0 || tx >= (int)texW || ty < 0 || ty >= (int)texH) {
                outR = outG = outB = outA = 0;
                return;
            }
            if (texType == 13) { // GPU_ETC1A4 (16 bytes per 4x4 block)
                uint32_t tileX = (uint32_t)tx / 8;
                uint32_t tileY = (uint32_t)ty / 8;
                uint32_t subBx = ((uint32_t)tx & 7U) / 4;
                uint32_t subBy = ((uint32_t)ty & 7U) / 4;
                uint32_t subBlockIdx = (subBx & 1U) | ((subBy & 1U) << 1U);
                uint32_t blockOffset = (tileX * tilesPerCol + tileY) * 64 + subBlockIdx * 16;

                if (blockOffset != lastBlockIdx) {
                    lastBlockIdx = blockOffset;
                    const uint8_t* blk = texPtr + blockOffset;
                    // First 8 bytes are 16 4-bit alpha nibbles in 3DS Morton (Z-curve) order
                    for (int by = 0; by < 4; ++by) {
                        for (int bx = 0; bx < 4; ++bx) {
                            uint32_t m = (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2);
                            uint8_t nib = (m & 1) ? (blk[m / 2] >> 4) : (blk[m / 2] & 0x0F);
                            cachedAlpha[by * 4 + bx] = (nib << 4) | nib;
                        }
                    }
                    // Next 8 bytes are ETC1 block with reversed byte endianness on 3DS
                    uint8_t rev[8];
                    for (int b = 0; b < 8; ++b) {
                        rev[b] = blk[8 + (7 - b)];
                    }
                    rg_etc1::unpack_etc1_block(rev, cachedRGBA, false);
                }

                uint32_t px = (uint32_t)tx % 4;
                uint32_t py = (uint32_t)ty % 4;
                uint32_t pIdx = py * 4 + px; // Row-major within 4x4 block from rg_etc1
                outA = cachedAlpha[pIdx];
                uint32_t col = cachedRGBA[pIdx];
                outR = col & 0xFF;
                outG = (col >> 8) & 0xFF;
                outB = (col >> 16) & 0xFF;
            } else if (texType == 12) { // GPU_ETC1 (8 bytes per 4x4 block)
                uint32_t tileX = (uint32_t)tx / 8;
                uint32_t tileY = (uint32_t)ty / 8;
                uint32_t subBx = ((uint32_t)tx & 7U) / 4;
                uint32_t subBy = ((uint32_t)ty & 7U) / 4;
                uint32_t subBlockIdx = (subBx & 1U) | ((subBy & 1U) << 1U);
                uint32_t blockOffset = (tileX * tilesPerCol + tileY) * 32 + subBlockIdx * 8;

                if (blockOffset != lastBlockIdx) {
                    lastBlockIdx = blockOffset;
                    const uint8_t* blk = texPtr + blockOffset;
                    uint8_t rev[8];
                    for (int b = 0; b < 8; ++b) {
                        rev[b] = blk[7 - b];
                    }
                    rg_etc1::unpack_etc1_block(rev, cachedRGBA, false);
                }

                uint32_t px = (uint32_t)tx % 4;
                uint32_t py = (uint32_t)ty % 4;
                uint32_t pIdx = py * 4 + px;
                outA = 0xFF;
                uint32_t col = cachedRGBA[pIdx];
                outR = col & 0xFF;
                outG = (col >> 8) & 0xFF;
                outB = (col >> 16) & 0xFF;
            } else {
                uint32_t xMod = (uint32_t)tx & 7U;
                uint32_t yMod = (uint32_t)ty & 7U;
                uint32_t texOff = (xLut[xMod] + yLut[yMod] + ((uint32_t)tx & ~7U) * 8U) * 4 + ((uint32_t)ty & ~7U) * texW * 4;
                outR = texPtr[texOff + 0];
                outG = texPtr[texOff + 1];
                outB = texPtr[texOff + 2];
                outA = texPtr[texOff + 3];
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

                static int s_triLog = 0;
                if (++s_triLog % 60 == 1) {
                    LOGI("DrawElements Tri: colorBuf=0x%08X isBottom=%d scrSpace=%d pos0=(%.1f, %.1f) pos1=(%.1f, %.1f) pos2=(%.1f, %.1f) uv0=(%.1f, %.1f) uv1=(%.1f, %.1f) col0=(%.2f, %.2f, %.2f, %.2f)",
                         colorBufPhys, isBottom, isScreenSpace, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, cr0, cg0, cb0, ca0);
                }

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
                        int pY = sx;   // 3DS portrait Y is landscape X

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
                                        fbPtr[fbOff + 0] = finalR;
                                        fbPtr[fbOff + 1] = finalG;
                                        fbPtr[fbOff + 2] = finalB;
                                        fbPtr[fbOff + 3] = 0xFF;
                                    } else {
                                        uint32_t invA = 255 - finalA;
                                        fbPtr[fbOff + 0] = (finalR * finalA + fbPtr[fbOff + 0] * invA) / 255;
                                        fbPtr[fbOff + 1] = (finalG * finalA + fbPtr[fbOff + 1] * invA) / 255;
                                        fbPtr[fbOff + 2] = (finalB * finalA + fbPtr[fbOff + 2] * invA) / 255;
                                        fbPtr[fbOff + 3] = 0xFF;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
