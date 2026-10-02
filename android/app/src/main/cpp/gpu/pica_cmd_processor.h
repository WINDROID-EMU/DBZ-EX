#ifndef PICA_CMD_PROCESSOR_H
#define PICA_CMD_PROCESSOR_H

#include <stdint.h>
#include <stddef.h>

#ifndef PICA_RENDERER_GPU
#define PICA_RENDERER_GPU 1 // 0 = CPU software rasterizer, 1 = Native Adreno GLES FBO
#endif

class HorizonOS;

enum class PicaScreenTarget {
    Top,
    Bottom
};

inline PicaScreenTarget GetScreenTarget(uint32_t colorBufPhys) {
    // 0x18177000 / 0x1F177000 (transferred to 0x1F300000) and VRAM-B are the bottom screen
    if (colorBufPhys == 0x18177000 || (colorBufPhys & 0x00FFFFFF) == 0x00177000 || colorBufPhys >= 0x18200000) {
        return PicaScreenTarget::Bottom;
    }
    return PicaScreenTarget::Top;
}

#include <vector>
#include <pthread.h>

struct PicaVertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
};

struct PicaDrawCall {
    bool isClear = false;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool isBottom = false;
    uint32_t texAddrPhys = 0;
    uint32_t texW = 0, texH = 0, texType = 0;
    std::vector<PicaVertex> vertices;
    std::vector<uint16_t> indices;
};

class PicaCommandProcessor {
public:
    PicaCommandProcessor();
    ~PicaCommandProcessor();

    void ProcessCommandList(const uint32_t* buffer, size_t wordCount, HorizonOS* os);

    uint32_t GetRegister(uint32_t regId) const;
    void SetRegister(uint32_t regId, uint32_t value, uint32_t mask);

    // Fila thread-safe de comandos de desenho para a GPU nativa
    void FetchGPUQueue(std::vector<PicaDrawCall>& outQueue);
    bool HasQueuedDraws();
    void ClearGPUQueue();
    void QueueClear(bool isBottom, float r, float g, float b, float a);

private:
    uint32_t m_regs[0x400];
    uint32_t m_drawCallCount = 0;

    pthread_mutex_t m_gpuQueueMutex;
    std::vector<PicaDrawCall> m_gpuQueue;

    void ExecuteDrawArrays(HorizonOS* os);
    void ExecuteDrawElements(HorizonOS* os);
};

#endif // PICA_CMD_PROCESSOR_H
