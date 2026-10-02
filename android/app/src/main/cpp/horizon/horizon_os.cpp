#include "horizon_os.h"
#include "gpu/pica_display_transfer.h"
#include "gpu/pica_cmd_processor.h"
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <vector>
#include <string>

#define LOG_TAG "HorizonOS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

HorizonOS* g_Horizon = nullptr;

// External symbols from entries.c
extern "C" {
    extern const uint32_t recomp_entry_count;
    extern const Entry recomp_entries[];
}

HorizonOS::HorizonOS() 
    : m_currentHeapEnd(HEAP_BASE),
      m_nextTlsAddr(TLS_BASE),
      m_nextHandle(0x100),
      m_running(false),
      m_mainFpscr(0),
      m_gspSharedMemHandle(0),
      m_gspSharedMemAddr(0),
      m_gspSharedMemPtr(nullptr),
      m_gspInterruptEventHandle(0),
      m_topFbFormat(0),
      m_botFbFormat(0) {
    memset(m_readPages, 0, sizeof(m_readPages));
    memset(m_writePages, 0, sizeof(m_writePages));
    memset(m_mainVfp, 0, sizeof(m_mainVfp));
    m_topFbAddr[0] = m_topFbAddr[1] = 0;
    m_botFbAddr[0] = m_botFbAddr[1] = 0;

    m_topFrameRGBA.resize(400 * 240 * 4, 0);
    m_botFrameRGBA.resize(320 * 240 * 4, 0);
    pthread_mutex_init(&m_frameMutex, nullptr);

    m_host.read8 = HostRead8;
    m_host.read16 = HostRead16;
    m_host.read32 = HostRead32;
    m_host.write8 = HostWrite8;
    m_host.write16 = HostWrite16;
    m_host.write32 = HostWrite32;
    m_host.interpret = HostInterpret;
    m_host.lookup = HostLookup;

    m_picaCmdProc = new PicaCommandProcessor();
}

HorizonOS::~HorizonOS() {
    if (m_picaCmdProc) {
        delete m_picaCmdProc;
        m_picaCmdProc = nullptr;
    }
    for (void* ptr : m_allocatedBlocks) {
        free(ptr);
    }
    for (auto& pair : m_openFiles) {
        if (pair.second.fp) {
            fclose(pair.second.fp);
        }
    }
    for (auto& pair : m_events) {
        pthread_mutex_destroy(&pair.second->mutex);
        pthread_cond_destroy(&pair.second->cond);
        delete pair.second;
    }
    for (auto& pair : m_threads) {
        delete pair.second;
    }
    pthread_mutex_destroy(&m_frameMutex);
}

#include "code_bin.h"

bool HorizonOS::Initialize(const std::string& storagePath) {
    m_storagePath = storagePath;
    m_running = false;
    LOGI("Initializing Horizon OS for Dragon Ball Z Extreme Butoden...");

    // 1. Map Code, Data, and BSS region (0x00100000, 16MB) and copy binary image
    MapMemory(CODE_BASE, 16 * 1024 * 1024, true, true);
    uint8_t* codePtr = GetPointer(CODE_BASE);
    if (codePtr) {
        memcpy(codePtr, g_CodeBin, CODE_BIN_SIZE);
        LOGI("Successfully loaded code.bin (%d bytes) at 0x%08X", CODE_BIN_SIZE, CODE_BASE);
    }

    // 2. Map Main Thread Stack (0x0FFFF000 - 8MB)
    uint32_t stackBottom = STACK_TOP - STACK_SIZE;
    MapMemory(stackBottom, STACK_SIZE, true, true);

    // 3. Map VRAM (0x18000000, 6MB) and its virtual mapping (0x1F000000, 6MB)
    MapMemory(VRAM_BASE, VRAM_SIZE, true, true);
    uint32_t vramPageStart = VRAM_BASE >> HORIZON_PAGE_SHIFT;
    uint32_t vramVirtPageStart = 0x1F000000u >> HORIZON_PAGE_SHIFT;
    uint32_t vramPageCount = VRAM_SIZE >> HORIZON_PAGE_SHIFT;
    for (uint32_t i = 0; i < vramPageCount; ++i) {
        m_readPages[vramVirtPageStart + i] = m_readPages[vramPageStart + i];
        m_writePages[vramVirtPageStart + i] = m_writePages[vramPageStart + i];
    }

    // 4. Map Linear Memory / FCRAM (0x14000000, 64MB) and mirror to physical FCRAM (0x20000000) and 0x30000000
    const uint32_t fcramSize = 64 * 1024 * 1024;
    MapMemory(0x14000000u, fcramSize, true, true);
    uint32_t fcramVirtPageStart = 0x14000000u >> HORIZON_PAGE_SHIFT;
    uint32_t fcramPhysPageStart = 0x20000000u >> HORIZON_PAGE_SHIFT;
    uint32_t fcramNewVirtPageStart = 0x30000000u >> HORIZON_PAGE_SHIFT;
    uint32_t fcramPageCount = fcramSize >> HORIZON_PAGE_SHIFT;
    for (uint32_t i = 0; i < fcramPageCount; ++i) {
        m_readPages[fcramPhysPageStart + i] = m_readPages[fcramVirtPageStart + i];
        m_writePages[fcramPhysPageStart + i] = m_writePages[fcramVirtPageStart + i];
        m_readPages[fcramNewVirtPageStart + i] = m_readPages[fcramVirtPageStart + i];
        m_writePages[fcramNewVirtPageStart + i] = m_writePages[fcramVirtPageStart + i];
    }

    // 5. Map Initial Heap (0x08000000, 32MB)
    MapMemory(HEAP_BASE, 32 * 1024 * 1024, true, true);
    m_currentHeapEnd = HEAP_BASE + (32 * 1024 * 1024);

    // 6. Map Initial TLS area
    MapMemory(TLS_BASE, 1024 * 1024, true, true);

    // 7. Map Shared Memory (HID/GSP) at 0x10000000
    MapMemory(0x10000000u, 1024 * 1024, true, true);

    // Setup dedicated GSP Shared Memory block at 0x10002000
    m_gspSharedMemAddr = 0x10002000u;
    m_gspSharedMemPtr = GetPointer(m_gspSharedMemAddr);
    if (m_gspSharedMemPtr) {
        memset(m_gspSharedMemPtr, 0, 0x1000);
        m_gspSharedMemPtr[2] = 1; // GSP InterruptRelayQueue enabled flag (CTR-SDK requirement)
    }
    m_gspSharedMemHandle = ++m_nextHandle;
    m_serviceHandles[m_gspSharedMemHandle] = "shared_mem:gsp";

    // Setup dedicated HID Shared Memory block handle (at 0x10000000)
    m_hidSharedMemHandle = ++m_nextHandle;
    m_serviceHandles[m_hidSharedMemHandle] = "shared_mem:hid";

    // 8. Map MMIO dummy region
    MapMemory(MMIO_BASE, MMIO_SIZE, true, true);

    LOGI("Memory mapping initialized successfully. GSP shared memory at 0x%08X (handle 0x%X), HID handle 0x%X",
         m_gspSharedMemAddr, m_gspSharedMemHandle, m_hidSharedMemHandle);
    return true;
}

void HorizonOS::MapMemory(uint32_t addr, uint32_t size, bool read, bool write) {
    uint32_t startPage = addr >> HORIZON_PAGE_SHIFT;
    uint32_t pageCount = (size + HORIZON_PAGE_MASK) >> HORIZON_PAGE_SHIFT;

    void* block = calloc(pageCount, HORIZON_PAGE_SIZE);
    m_allocatedBlocks.push_back(block);

    uint8_t* ptr = (uint8_t*)block;
    for (uint32_t i = 0; i < pageCount; ++i) {
        uint32_t page = startPage + i;
        if (page < NUM_PAGES) {
            if (read) m_readPages[page] = ptr + (i * HORIZON_PAGE_SIZE);
            if (write) m_writePages[page] = ptr + (i * HORIZON_PAGE_SIZE);
        }
    }
}

void HorizonOS::UnmapMemory(uint32_t addr, uint32_t size) {
    uint32_t startPage = addr >> HORIZON_PAGE_SHIFT;
    uint32_t pageCount = (size + HORIZON_PAGE_MASK) >> HORIZON_PAGE_SHIFT;
    for (uint32_t i = 0; i < pageCount; ++i) {
        uint32_t page = startPage + i;
        if (page < NUM_PAGES) {
            m_readPages[page] = nullptr;
            m_writePages[page] = nullptr;
        }
    }
}

uint8_t* HorizonOS::GetPointer(uint32_t addr) {
    uint32_t page = addr >> HORIZON_PAGE_SHIFT;
    if (page < NUM_PAGES && m_readPages[page]) {
        return m_readPages[page] + (addr & HORIZON_PAGE_MASK);
    }
    return nullptr;
}

bool HorizonOS::ReadBytes(uint32_t addr, void* dest, size_t size) {
    uint8_t* out = (uint8_t*)dest;
    while (size > 0) {
        uint32_t page = addr >> HORIZON_PAGE_SHIFT;
        if (page >= NUM_PAGES || !m_readPages[page]) return false;
        uint32_t offset = addr & HORIZON_PAGE_MASK;
        uint32_t chunk = HORIZON_PAGE_SIZE - offset;
        if (chunk > size) chunk = size;
        memcpy(out, m_readPages[page] + offset, chunk);
        addr += chunk;
        out += chunk;
        size -= chunk;
    }
    return true;
}

bool HorizonOS::WriteBytes(uint32_t addr, const void* src, size_t size) {
    const uint8_t* in = (const uint8_t*)src;
    while (size > 0) {
        uint32_t page = addr >> HORIZON_PAGE_SHIFT;
        if (page >= NUM_PAGES || !m_writePages[page]) return false;
        uint32_t offset = addr & HORIZON_PAGE_MASK;
        uint32_t chunk = HORIZON_PAGE_SIZE - offset;
        if (chunk > size) chunk = size;
        memcpy(m_writePages[page] + offset, in, chunk);
        addr += chunk;
        in += chunk;
        size -= chunk;
    }
    return true;
}

void HorizonOS::NotifyFramebufferUpdated(uint32_t address, uint32_t width, uint32_t height, uint32_t format) {
    pthread_mutex_lock(&m_frameMutex);
    
    // Differentiate Top Screen vs Bottom Screen by VRAM address and dimensions
    bool isBottomScreen = (address >= 0x1F200000) || (width == 320 && height == 240) || (width == 240 && height == 320);
    bool isTopScreen = !isBottomScreen && ((width == 400 && height == 240) || (width == 240 && height == 400));

    if (isTopScreen) {
        size_t bpp = (format == 0) ? 4 : ((format == 1) ? 3 : 2);
        std::vector<uint8_t> raw(width * height * bpp);
        if (ReadBytes(address, raw.data(), raw.size())) {
            uint32_t nonZero = 0;
            for (size_t i = 0; i < raw.size(); i += 8) {
                if (raw[i] != 0) nonZero++;
            }
            static int s_topLog = 0;
            if (++s_topLog % 30 == 0 || nonZero > 100) {
                LOGI("NotifyFramebufferUpdated Top: addr=0x%08X %ux%u fmt=%u nonZeroSample=%u/%zu",
                     address, width, height, format, nonZero, raw.size());
            }

            for (uint32_t y = 0; y < 240; ++y) {
                for (uint32_t x = 0; x < 400; ++x) {
                    uint32_t srcIdx = (width == 240 && height == 400)
                                          ? (x * 240 + (239 - y)) * bpp
                                          : (x + y * 400) * bpp;
                    uint32_t dstIdx = (x + y * 400) * 4;
                    if (format == 0) { // RGBA8
                        m_topFrameRGBA[dstIdx + 0] = raw[srcIdx + 0];
                        m_topFrameRGBA[dstIdx + 1] = raw[srcIdx + 1];
                        m_topFrameRGBA[dstIdx + 2] = raw[srcIdx + 2];
                        m_topFrameRGBA[dstIdx + 3] = raw[srcIdx + 3];
                    } else if (format == 1) { // RGB8
                        m_topFrameRGBA[dstIdx + 0] = raw[srcIdx + 0];
                        m_topFrameRGBA[dstIdx + 1] = raw[srcIdx + 1];
                        m_topFrameRGBA[dstIdx + 2] = raw[srcIdx + 2];
                        m_topFrameRGBA[dstIdx + 3] = 0xFF;
                    } else if (format == 2) { // RGB565
                        uint16_t p = raw[srcIdx] | (raw[srcIdx + 1] << 8);
                        m_topFrameRGBA[dstIdx + 0] = ((p >> 11) & 0x1F) * 255 / 31;
                        m_topFrameRGBA[dstIdx + 1] = ((p >> 5) & 0x3F) * 255 / 63;
                        m_topFrameRGBA[dstIdx + 2] = (p & 0x1F) * 255 / 31;
                        m_topFrameRGBA[dstIdx + 3] = 0xFF;
                    }
                }
            }
        }
    } else if (isBottomScreen) {
        size_t bpp = (format == 0) ? 4 : ((format == 1) ? 3 : 2);
        std::vector<uint8_t> raw(width * height * bpp);
        if (ReadBytes(address, raw.data(), raw.size())) {
            uint32_t nonZero = 0;
            for (size_t i = 0; i < raw.size(); i += 8) {
                if (raw[i] != 0) nonZero++;
            }
            static int s_botLog = 0;
            if (++s_botLog % 30 == 0 || nonZero > 100) {
                LOGI("NotifyFramebufferUpdated Bottom: addr=0x%08X %ux%u fmt=%u nonZeroSample=%u/%zu",
                     address, width, height, format, nonZero, raw.size());
            }

            // Bottom screen active width is 320, height is 240
            uint32_t xOffset = (width == 240 && height == 400) ? 40 : 0;
            for (uint32_t y = 0; y < 240; ++y) {
                for (uint32_t x = 0; x < 320; ++x) {
                    uint32_t srcX = x + xOffset;
                    uint32_t srcIdx = (width == 240)
                                          ? (srcX * 240 + (239 - y)) * bpp
                                          : (srcX + y * width) * bpp;
                    uint32_t dstIdx = (x + y * 320) * 4;
                    if (srcIdx + bpp <= raw.size()) {
                        if (format == 0) { // RGBA8
                            m_botFrameRGBA[dstIdx + 0] = raw[srcIdx + 0];
                            m_botFrameRGBA[dstIdx + 1] = raw[srcIdx + 1];
                            m_botFrameRGBA[dstIdx + 2] = raw[srcIdx + 2];
                            m_botFrameRGBA[dstIdx + 3] = raw[srcIdx + 3];
                        } else if (format == 1) { // RGB8
                            m_botFrameRGBA[dstIdx + 0] = raw[srcIdx + 0];
                            m_botFrameRGBA[dstIdx + 1] = raw[srcIdx + 1];
                            m_botFrameRGBA[dstIdx + 2] = raw[srcIdx + 2];
                            m_botFrameRGBA[dstIdx + 3] = 0xFF;
                        } else if (format == 2) { // RGB565
                            uint16_t p = raw[srcIdx] | (raw[srcIdx + 1] << 8);
                            m_botFrameRGBA[dstIdx + 0] = ((p >> 11) & 0x1F) * 255 / 31;
                            m_botFrameRGBA[dstIdx + 1] = ((p >> 5) & 0x3F) * 255 / 63;
                            m_botFrameRGBA[dstIdx + 2] = (p & 0x1F) * 255 / 31;
                            m_botFrameRGBA[dstIdx + 3] = 0xFF;
                        }
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&m_frameMutex);
}

uint8_t* HorizonOS::GetTopScreenVRAM() {
    return m_topFrameRGBA.data();
}

uint8_t* HorizonOS::GetBottomScreenVRAM() {
    return m_botFrameRGBA.data();
}

void HorizonOS::SignalEvent(uint32_t handle) {
    auto it = m_events.find(handle);
    if (it != m_events.end()) {
        pthread_mutex_lock(&it->second->mutex);
        it->second->signaled = true;
        pthread_cond_broadcast(&it->second->cond);
        pthread_mutex_unlock(&it->second->mutex);
        if (handle != 0x127 && handle != 0x128 && handle != 0x12E) {
            LOGI("SignalEvent: handle 0x%X ('%s')", handle, it->second->name.c_str());
        }
    }
}

bool HorizonOS::CheckEventSignaled(HorizonEvent* evt) {
    if (!evt) return false;
    if (evt->is_timer) {
        if (!evt->signaled && evt->next_fire_ns > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            if (now_ns >= evt->next_fire_ns) {
                evt->signaled = true;
                if (evt->interval_ns > 0) {
                    evt->next_fire_ns = now_ns + (uint64_t)evt->interval_ns;
                } else {
                    evt->next_fire_ns = 0;
                }
            }
        }
    }
    return evt->signaled;
}

void HorizonOS::SignalDspInterrupts() {
    for (uint32_t h : m_dspInterruptEvents) {
        SignalEvent(h);
    }
}

void HorizonOS::QueueGspInterrupt(uint8_t interruptId) {
    if (!m_gspSharedMemPtr) {
        m_gspSharedMemPtr = GetPointer(0x10002000);
        if (!m_gspSharedMemPtr) return;
    }
    
    // Relay block at start of shared memory
    uint8_t index = m_gspSharedMemPtr[0];
    uint8_t count = m_gspSharedMemPtr[1];
    if (count < 0x34) {
        uint8_t slot = (index + count) % 0x34;
        m_gspSharedMemPtr[0x0C + slot] = interruptId;
        m_gspSharedMemPtr[1] = count + 1;
        m_gspSharedMemPtr[2] = 1; // Queue active flag
    }
    
    if (m_gspInterruptEventHandle) {
        SignalEvent(m_gspInterruptEventHandle);
    }

    // On VBlank (interrupt 2 = PDC0 top screen), check if GSP release right was requested
    if (interruptId == 2) {
        int32_t* gspLock = (int32_t*)GetPointer(0x003A5CA4);
        uint8_t* f76 = (uint8_t*)GetPointer(0x003A5C40 + 0x76);
        if (f76 && *f76 == 1 && gspLock && *gspLock == -1) {
            *f76 = 2;
            *gspLock = 0;
            for (auto& pair : m_threads) {
                HorizonThread* t = pair.second;
                if (t && t->active && t->wait_type == WAIT_ARBITER && t->arbiter_addr == 0x003A5CA4) {
                    t->wait_type = WAIT_NONE;
                    t->ctx.r[0] = 0;
                }
            }
        }
    }
}

void HorizonOS::ProcessGspCommandQueue() {
    if (!m_gspSharedMemPtr) {
        m_gspSharedMemPtr = GetPointer(0x10002000);
        if (!m_gspSharedMemPtr) return;
    }

    uint32_t queueOffset = 0x800;
    uint32_t* queueHeaderPtr = (uint32_t*)(m_gspSharedMemPtr + queueOffset);
    uint32_t queueHeader = *queueHeaderPtr;
    uint32_t index = queueHeader & 0xFF;
    uint32_t count = (queueHeader >> 8) & 0xFF;
    uint32_t status = (queueHeader >> 16) & 0xFF;
    uint32_t shouldStop = (queueHeader >> 24) & 0xFF;

    while (count > 0 && status == 0 && shouldStop == 0) {
        uint32_t* cmdWords = (uint32_t*)(m_gspSharedMemPtr + queueOffset + 0x20 + index * 0x20);
        uint32_t control = cmdWords[0];
        uint32_t cmdId = control & 0xFF;
        uint32_t p0 = cmdWords[1];
        uint32_t p1 = cmdWords[2];
        uint32_t p2 = cmdWords[3];
        uint32_t p3 = cmdWords[4];
        uint32_t p4 = cmdWords[5];
        uint32_t p5 = cmdWords[6];
        uint32_t p6 = cmdWords[7];

        LOGI("GSP Processing Cmd ID=%d, p0=0x%08X, p1=0x%08X, p2=0x%08X", cmdId, p0, p1, p2);

        switch (cmdId) {
            case 0: { // RequestDma
                std::vector<uint8_t> dmaBuf(p2);
                if (ReadBytes(p0, dmaBuf.data(), p2)) {
                    WriteBytes(p1, dmaBuf.data(), p2);
                }
                QueueGspInterrupt(6); // Dma
                break;
            }
            case 1: { // SubmitCommandList
                uint32_t* cmds = (uint32_t*)GetPointer(p0);
                if (cmds && p1 >= 8 && m_picaCmdProc) {
                    m_picaCmdProc->ProcessCommandList(cmds, p1 / 4, this);
                } else if (cmds && p1 >= 16) {
                    LOGI("GSP SubmitCommandList: addr=0x%08X size=0x%X (cmd0=0x%08X cmd1=0x%08X)",
                         p0, p1, cmds[0], cmds[1]);
                } else {
                    LOGI("GSP SubmitCommandList: addr=0x%08X size=0x%X", p0, p1);
                }
                QueueGspInterrupt(5); // P3d (draw finished)
                break;
            }
            case 2: { // MemoryFill
                PicaMemoryFill fill;
                fill.startAddress = p0;
                fill.endAddress = p2;
                fill.value = p1;
                fill.control = p6 & 0xFFFF;
                ExecutePicaMemoryFill(fill, this);
                if (p3 != 0) {
                    fill.startAddress = p3;
                    fill.endAddress = p5;
                    fill.value = p4;
                    fill.control = (p6 >> 16) & 0xFFFF;
                    ExecutePicaMemoryFill(fill, this);
                }
                QueueGspInterrupt(0); // Psc0
                break;
            }
            case 3: { // DisplayTransfer
                PicaDisplayTransfer dt;
                dt.inputAddress = p0;
                dt.outputAddress = p1;
                dt.outputSize = p2;
                dt.inputSize = p3;
                dt.flags = p4;
                std::string dtErr;
                if (!ExecutePicaDisplayTransfer(dt, this, &dtErr)) {
                    LOGE("ExecutePicaDisplayTransfer error: %s", dtErr.c_str());
                } else {
                    LOGI("DisplayTransfer executed successfully: out=0x%08X, size=0x%08X", p1, p2);
                }
                QueueGspInterrupt(4); // Ppf (display transfer finished)
                break;
            }
            case 4: { // TextureCopy
                QueueGspInterrupt(6);
                break;
            }
            default:
                break;
        }

        index = (index + 1) % 15;
        count--;
    }

    *queueHeaderPtr = index | (count << 8) | (status << 16) | (shouldStop << 24);
}

void HorizonOS::HandleSVC(Context* ctx) {
    uint32_t svc = ctx->svc;
    if (svc != 0x0A && svc != 0x18 && svc != 0x19 && svc != 0x22 && svc != 0x24 && svc != 0x28 && svc != 0x3C) {
        LOGI("SVC 0x%02X called from PC=0x%08X (r0=0x%X, r1=0x%X)", svc, ctx->r[15], ctx->r[0], ctx->r[1]);
    }
    switch (svc) {
        // ControlMemory
        case 0x01: {
            uint32_t op = ctx->r[0];
            uint32_t baseOp = op & 0xFF;
            uint32_t addr0 = ctx->r[1];
            uint32_t addr1 = ctx->r[2];
            uint32_t size = ctx->r[3];
            uint32_t perm = ctx->r[4];

            if (baseOp == 3) { // COMMIT / ALLOC
                uint32_t addrOut = addr0;
                bool isLinear = ((op & 0x10000) != 0);
                if (addrOut == 0) {
                    if (isLinear) {
                        addrOut = m_currentLinearEnd;
                        m_currentLinearEnd += ((size + HORIZON_PAGE_MASK) & ~HORIZON_PAGE_MASK);
                    } else {
                        addrOut = m_currentHeapEnd;
                        m_currentHeapEnd += ((size + HORIZON_PAGE_MASK) & ~HORIZON_PAGE_MASK);
                    }
                }
                MapMemory(addrOut, size, true, true);
                if (isLinear) {
                    // Mirror virtual linear memory (0x14000000) to physical FCRAM (0x20000000)
                    uint32_t vPage = addrOut >> HORIZON_PAGE_SHIFT;
                    uint32_t pPage = (0x20000000u + (addrOut - 0x14000000u)) >> HORIZON_PAGE_SHIFT;
                    uint32_t pCount = (size + HORIZON_PAGE_MASK) >> HORIZON_PAGE_SHIFT;
                    for (uint32_t i = 0; i < pCount; ++i) {
                        if (pPage + i < NUM_PAGES && vPage + i < NUM_PAGES) {
                            m_readPages[pPage + i] = m_readPages[vPage + i];
                            m_writePages[pPage + i] = m_writePages[vPage + i];
                        }
                    }
                }
                ctx->r[1] = addrOut;
                ctx->r[0] = 0; // SUCCESS
                LOGI("ControlMemory(COMMIT): op=0x%X addr0=0x%08X size=0x%X -> outAddr=0x%08X (linear=%d)",
                     op, addr0, size, addrOut, (int)isLinear);
            } else if (baseOp == 1) { // FREE
                ctx->r[1] = addr0;
                ctx->r[0] = 0;
                LOGI("ControlMemory(FREE): addr0=0x%08X size=0x%X", addr0, size);
            } else {
                ctx->r[1] = addr0;
                ctx->r[0] = 0;
            }
            break;
        }

        // QueryMemory
        case 0x02: {
            ctx->r[0] = 0;
            break;
        }

        // ExitProcess
        case 0x03: {
            LOGI("Game requested ExitProcess (svc 0x03)");
            ctx->exit = EXIT_UNWIND;
            break;
        }

        // CreateThread
        case 0x08: {
            uint32_t entry = ctx->r[1];
            uint32_t arg = ctx->r[2];
            uint32_t stackTop = ctx->r[3];
            uint32_t prio = ctx->r[4];

            HorizonThread* thread = new HorizonThread();
            thread->entry_point = entry;
            thread->stack_top = stackTop;
            thread->priority = prio;
            thread->tls_addr = m_nextTlsAddr;
            m_nextTlsAddr += 0x1000;
            thread->active = true;

            memset(&thread->ctx, 0, sizeof(Context));
            thread->ctx.r[0] = arg;
            thread->ctx.r[13] = stackTop; // SP
            thread->ctx.r[15] = entry;    // PC
            thread->ctx.tls = thread->tls_addr;
            thread->ctx.read_pages = m_readPages;
            thread->ctx.write_pages = m_writePages;
            thread->ctx.vfp = thread->vfp_regs;
            thread->ctx.fpscr = &thread->fpscr;
            thread->ctx.host = &m_host;
            thread->ctx.user = this;

            uint32_t handle = ++m_nextHandle;
            m_threads[handle] = thread;
            ctx->r[1] = handle;
            ctx->r[0] = 0; // Result: SUCCESS
            LOGI("ALLOC_HANDLE: 0x%X for CreateThread (entry=0x%08X)", handle, entry);
            break;
        }

        // ExitThread
        case 0x09: {
            LOGI("Thread at entry 0x%08X called svcExitThread (0x09)", m_currentThread ? m_currentThread->entry_point : 0);
            ctx->exit = EXIT_UNWIND;
            if (m_currentThread) {
                m_currentThread->active = false;
            }
            break;
        }

        // SleepThread (r0: nanoseconds low, r1: nanoseconds high)
        case 0x0A: {
            int64_t ns = ((int64_t)ctx->r[1] << 32) | ctx->r[0];
            if (ns > 0 && m_currentThread) {
                m_currentThread->wait_type = WAIT_SLEEP;
                m_currentThread->wait_timeout = ns;
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                m_currentThread->wait_start_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
            } else {
                sched_yield();
            }
            ctx->r[0] = 0;
            break;
        }

        // CreateMutex
        case 0x13: {
            uint32_t handle = ++m_nextHandle;
            ctx->r[1] = handle;
            ctx->r[0] = 0;
            LOGI("ALLOC_HANDLE: 0x%X for CreateMutex", handle);
            break;
        }

        // ReleaseMutex
        case 0x14: {
            ctx->r[0] = 0;
            break;
        }


        // CreateEvent
        case 0x17: {
            HorizonEvent* evt = new HorizonEvent();
            pthread_mutex_init(&evt->mutex, nullptr);
            pthread_cond_init(&evt->cond, nullptr);
            evt->signaled = false;
            evt->manual_reset = (ctx->r[1] == 1);
            evt->name = "SVC 0x17 CreateEvent";

            uint32_t handle = ++m_nextHandle;
            m_events[handle] = evt;
            ctx->r[1] = handle;
            ctx->r[0] = 0;
            LOGI("ALLOC_HANDLE: 0x%X for CreateEvent (manual=%d)", handle, evt->manual_reset);
            break;
        }

        // SignalEvent
        case 0x18: {
            uint32_t handle = ctx->r[0];
            SignalEvent(handle);
            ctx->r[0] = 0;
            break;
        }

        // ClearEvent
        case 0x19: {
            uint32_t handle = ctx->r[0];
            auto it = m_events.find(handle);
            if (it != m_events.end()) {
                pthread_mutex_lock(&it->second->mutex);
                it->second->signaled = false;
                pthread_mutex_unlock(&it->second->mutex);
            }
            ctx->r[0] = 0;
            break;
        }

        // CreateTimer (0x1A)
        case 0x1A: {
            HorizonEvent* timer = new HorizonEvent();
            pthread_mutex_init(&timer->mutex, nullptr);
            pthread_cond_init(&timer->cond, nullptr);
            timer->signaled = false;
            timer->is_timer = true;
            timer->manual_reset = (ctx->r[1] == 1);
            timer->name = "Timer";

            uint32_t handle = ++m_nextHandle;
            m_events[handle] = timer;
            ctx->r[1] = handle;
            ctx->r[0] = 0; // SUCCESS
            LOGI("ALLOC_HANDLE: 0x%X for CreateTimer (resetType=%u)", handle, ctx->r[1]);
            break;
        }

        // SetTimer (0x1B)
        case 0x1B: {
            uint32_t handle = ctx->r[0];
            int64_t initial = ((int64_t)ctx->r[2] << 32) | (uint32_t)ctx->r[1];
            int64_t interval = ((int64_t)ctx->r[4] << 32) | (uint32_t)ctx->r[3];
            auto it = m_events.find(handle);
            if (it != m_events.end()) {
                pthread_mutex_lock(&it->second->mutex);
                it->second->is_timer = true;
                it->second->interval_ns = interval;
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                uint64_t delay_ns = (initial < 0) ? (uint64_t)(-initial) : (uint64_t)initial;
                it->second->next_fire_ns = now_ns + delay_ns;
                it->second->signaled = (delay_ns == 0);
                pthread_mutex_unlock(&it->second->mutex);
            }
            ctx->r[0] = 0;
            break;
        }

        // CancelTimer (0x1C)
        case 0x1C: {
            uint32_t handle = ctx->r[0];
            auto it = m_events.find(handle);
            if (it != m_events.end()) {
                pthread_mutex_lock(&it->second->mutex);
                it->second->signaled = false;
                it->second->next_fire_ns = 0;
                pthread_mutex_unlock(&it->second->mutex);
            }
            ctx->r[0] = 0;
            break;
        }

        // ClearTimer (0x1D)
        case 0x1D: {
            uint32_t handle = ctx->r[0];
            auto it = m_events.find(handle);
            if (it != m_events.end()) {
                pthread_mutex_lock(&it->second->mutex);
                it->second->signaled = false;
                pthread_mutex_unlock(&it->second->mutex);
            }
            ctx->r[0] = 0;
            break;
        }

        // MapMemoryBlock
        case 0x1F: {
            uint32_t handle = ctx->r[0];
            uint32_t addr = ctx->r[1];
            LOGI("MapMemoryBlock: handle=0x%X, addr=0x%08X", handle, addr);
            if (handle == m_gspSharedMemHandle || addr == 0x10002000) {
                if (!GetPointer(addr)) {
                    MapMemory(addr, 0x10000, true, true);
                }
                m_gspSharedMemAddr = addr;
                m_gspSharedMemPtr = GetPointer(addr);
                if (m_gspSharedMemPtr) {
                    m_gspSharedMemPtr[2] = 1; // GSP InterruptRelayQueue enabled flag (CTR-SDK requirement)
                }
                LOGI("MapMemoryBlock: GSP SharedMem mapped to 0x%08X (ptr=%p)", addr, m_gspSharedMemPtr);
            } else {
                if (!GetPointer(addr)) {
                    MapMemory(addr, 0x10000, true, true);
                }
            }
            ctx->r[0] = 0; // SUCCESS
            break;
        }

        // CreateAddressArbiter
        case 0x21: {
            uint32_t handle = ++m_nextHandle;
            ctx->r[1] = handle;
            ctx->r[0] = 0;
            LOGI("ALLOC_HANDLE: 0x%X for CreateAddressArbiter", handle);
            break;
        }

        // ArbitrateAddress (Azahar / CTR-SDK logic)
        case 0x22: {
            uint32_t arbiterHandle = ctx->r[0];
            uint32_t addr = ctx->r[1];
            uint32_t type = ctx->r[2];
            int32_t value = (int32_t)ctx->r[3];
            int64_t ns = -1LL;

            if (type == 3 || type == 4) {
                uint32_t* spPtr = (uint32_t*)GetPointer(ctx->r[13]);
                if (spPtr) {
                    ns = ((int64_t)spPtr[1] << 32) | spPtr[0];
                }
            }

            int32_t* curValPtr = (int32_t*)GetPointer(addr);
            int32_t curVal = curValPtr ? *curValPtr : 0;
            LOGI("ArbitrateAddress: caller=0x%08X (thread=0x%08X), addr=0x%08X, type=%u, value=%d, *addr=%d, ns=%lld",
                 ctx->r[15], m_currentThread ? m_currentThread->entry_point : 0, addr, type, value, curVal, (long long)ns);

            if (type == 0) { // Signal: wake up waiting threads
                int count = 0;
                for (auto& pair : m_threads) {
                    HorizonThread* t = pair.second;
                    if (t && t->active && t->wait_type == WAIT_ARBITER && t->arbiter_addr == addr) {
                        t->wait_type = WAIT_NONE;
                        count++;
                        if (value > 0 && count >= value) break;
                    }
                }
                ctx->r[0] = 0;
            } else if (type == 1 || type == 3) { // WaitIfLessThan
                int32_t* valPtr = (int32_t*)GetPointer(addr);
                int32_t currentVal = valPtr ? *valPtr : 0;
                if (currentVal < value) {
                    if (m_currentThread) {
                        m_currentThread->wait_type = WAIT_ARBITER;
                        m_currentThread->arbiter_addr = addr;
                        m_currentThread->arbiter_value = value;
                        m_currentThread->arbiter_type = type;
                        m_currentThread->wait_timeout = ns;
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        m_currentThread->wait_start_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
                    }
                }
                ctx->r[0] = 0;
            } else if (type == 2 || type == 4) { // DecrementAndWaitIfLessThan
                int32_t* valPtr = (int32_t*)GetPointer(addr);
                int32_t currentVal = valPtr ? *valPtr : 0;
                if (currentVal < value) {
                    if (valPtr) *valPtr = currentVal - 1;
                    if (m_currentThread) {
                        m_currentThread->wait_type = WAIT_ARBITER;
                        m_currentThread->arbiter_addr = addr;
                        m_currentThread->arbiter_value = value;
                        m_currentThread->arbiter_type = type;
                        m_currentThread->wait_timeout = ns;
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        m_currentThread->wait_start_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
                    }
                }
                ctx->r[0] = 0;
            } else {
                ctx->r[0] = 0;
            }
            break;
        }

        // CloseHandle
        case 0x23: {
            ctx->r[0] = 0;
            break;
        }

        // WaitSynchronization1 (0x24) & WaitSynchronizationN (0x25)
        case 0x24:
        case 0x25: {
            if (svc == 0x24) {
                uint32_t handle = ctx->r[0];
                int64_t ns = ((int64_t)ctx->r[3] << 32) | (uint32_t)ctx->r[2];
                auto it = m_events.find(handle);
                bool signaled = false;
                if (it != m_events.end()) {
                    pthread_mutex_lock(&it->second->mutex);
                    if (CheckEventSignaled(it->second)) {
                        signaled = true;
                        if (!it->second->manual_reset) {
                            it->second->signaled = false;
                        }
                    }
                    pthread_mutex_unlock(&it->second->mutex);
                } else {
                    signaled = true;
                }

                if (signaled) {
                    ctx->r[0] = 0; // SUCCESS
                } else if (ns == 0) {
                    ctx->r[0] = 0xD88007FA; // TIMEOUT
                } else {
                    if (m_currentThread) {
                        m_currentThread->wait_type = WAIT_SYNCH_1;
                        m_currentThread->wait_handle = handle;
                        m_currentThread->wait_timeout = ns;
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        m_currentThread->wait_start_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
                        if (handle != 0x127 && handle != 0x128) {
                            LOGI("Thread[entry=0x%08X] SUSPENDED on WaitSynchronization1(handle=0x%X, name='%s', ns=%lld)",
                                 m_currentThread->entry_point, handle, it != m_events.end() ? it->second->name.c_str() : "unknown", (long long)ns);
                        }
                    }
                }
            } else {
                // SVC 0x25: WaitSynchronizationN
                uint32_t handlesPtr = ctx->r[1];
                uint32_t handleCount = ctx->r[2];
                int64_t ns = ((int64_t)ctx->r[4] << 32) | (uint32_t)ctx->r[0];
                uint32_t* handles = (uint32_t*)GetPointer(handlesPtr);
                static int s_synchNLog = 0;
                if (++s_synchNLog % 100 == 1) {
                    uint32_t h0 = handles ? handles[0] : 0;
                    auto it0 = m_events.find(h0);
                    LOGI("WaitSynchronizationN: count=%u, ptr=0x%08X, ns=%lld, h0=0x%X (name='%s', sig=%d, manual=%d, timer=%d), h1=0x%X",
                         handleCount, handlesPtr, (long long)ns, h0,
                         it0 != m_events.end() ? it0->second->name.c_str() : "unknown",
                         it0 != m_events.end() ? (int)it0->second->signaled : -1,
                         it0 != m_events.end() ? (int)it0->second->manual_reset : -1,
                         it0 != m_events.end() ? (int)it0->second->is_timer : -1,
                         (handles && handleCount > 1) ? handles[1] : 0);
                }
                int signaledIdx = -1;
                if (handles && handleCount > 0 && handleCount <= 64) {
                    for (uint32_t i = 0; i < handleCount; ++i) {
                        uint32_t h = handles[i];
                        auto it = m_events.find(h);
                        if (it != m_events.end()) {
                            pthread_mutex_lock(&it->second->mutex);
                            if (CheckEventSignaled(it->second)) {
                                signaledIdx = (int)i;
                                if (!it->second->manual_reset) {
                                    it->second->signaled = false;
                                }
                                pthread_mutex_unlock(&it->second->mutex);
                                break;
                            }
                            pthread_mutex_unlock(&it->second->mutex);
                        }
                    }
                }

                if (s_synchNLog % 100 == 1) {
                    LOGI("WaitSynchronizationN: signaledIdx=%d", signaledIdx);
                }

                if (signaledIdx >= 0) {
                    ctx->r[0] = 0; // SUCCESS
                    ctx->r[1] = (uint32_t)signaledIdx;
                } else if (ns == 0) {
                    ctx->r[0] = 0xD88007FA; // TIMEOUT
                } else {
                    if (m_currentThread && handles) {
                        m_currentThread->wait_type = WAIT_SYNCH_N;
                        m_currentThread->wait_handles.assign(handles, handles + handleCount);
                        m_currentThread->wait_timeout = ns;
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        m_currentThread->wait_start_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
                        LOGI("Thread[entry=0x%08X] SUSPENDED on WaitSynchronizationN(count=%u, ns=%lld)",
                             m_currentThread->entry_point, handleCount, (long long)ns);
                    }
                }
            }
            break;
        }

        // DuplicateHandle
        case 0x27: {
            uint32_t handle = ctx->r[1];
            uint32_t newHandle = ++m_nextHandle;
            if (m_serviceHandles.count(handle)) {
                m_serviceHandles[newHandle] = m_serviceHandles[handle];
            }
            if (m_events.count(handle)) {
                m_events[newHandle] = m_events[handle];
            }
            ctx->r[1] = newHandle;
            ctx->r[0] = 0; // SUCCESS
            LOGI("ALLOC_HANDLE: 0x%X for DuplicateHandle from 0x%X", newHandle, handle);
            break;
        }

        // GetSystemTick
        case 0x28: {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
            uint64_t ticks = (ns * 268123480ULL) / 1000000000ULL;
            ctx->r[0] = (uint32_t)(ticks & 0xFFFFFFFF);
            ctx->r[1] = (uint32_t)(ticks >> 32);
            break;
        }

        // GetProcessId
        case 0x35: {
            uint32_t* out = (uint32_t*)GetPointer(ctx->r[0]);
            if (out) *out = 1;
            ctx->r[0] = 0;
            break;
        }

        // OutputDebugString / Break
        case 0x3C: {
            ctx->r[0] = 0;
            break;
        }

        // ConnectToPort
        case 0x2D: {
            uint32_t handle = ++m_nextHandle;
            char* name = (char*)GetPointer(ctx->r[1]);
            if (name) {
                m_serviceHandles[handle] = std::string(name, strnlen(name, 12));
                LOGI("ConnectToPort: connected to port '%s' -> assigned handle 0x%X", name, handle);
            }
            ctx->r[1] = handle;
            ctx->r[0] = 0;
            break;
        }

        // SendSyncRequest (IPC)
        case 0x32: {
            HandleSyncRequest(ctx);
            break;
        }

        // GetResourceLimit
        case 0x38: {
            uint32_t handle = ++m_nextHandle;
            ctx->r[1] = handle;
            ctx->r[0] = 0;
            LOGI("ALLOC_HANDLE: 0x%X for GetResourceLimit", handle);
            break;
        }

        // GetResourceLimitLimitValues
        case 0x39: {
            int64_t* values = (int64_t*)GetPointer(ctx->r[0]);
            if (values) {
                for (int i = 0; i < ctx->r[3]; i++) {
                    values[i] = 128 * 1024 * 1024;
                }
            }
            ctx->r[0] = 0;
            break;
        }

        // GetResourceLimitCurrentValues
        case 0x3A: {
            int64_t* values = (int64_t*)GetPointer(ctx->r[0]);
            if (values) {
                for (int i = 0; i < ctx->r[3]; i++) {
                    values[i] = 0;
                }
            }
            ctx->r[0] = 0;
            break;
        }

        // Default: return SUCCESS to prevent halting
        default:
            ctx->r[0] = 0;
            break;
    }
}

static void* MainThreadRunner(void* arg) {
    HorizonOS* os = (HorizonOS*)arg;
    os->RunMainThread(CODE_BASE);
    return nullptr;
}

void HorizonOS::StartGame() {
    if (m_running) return;
    m_running = true;
    pthread_t thread;
    pthread_create(&thread, nullptr, MainThreadRunner, this);
    pthread_detach(thread);
}

void HorizonOS::StopGame() {
    m_running = false;
}

void HorizonOS::RunMainThread(uint32_t entryPoint) {
    LOGI("RunMainThread starting at entry point 0x%08X...", entryPoint);
    
    HorizonThread* mainThread = new HorizonThread();
    mainThread->entry_point = entryPoint;
    mainThread->stack_top = STACK_TOP;
    mainThread->priority = 0x30;
    mainThread->tls_addr = TLS_BASE;
    mainThread->active = true;

    memset(&mainThread->ctx, 0, sizeof(Context));
    mainThread->ctx.r[13] = STACK_TOP;
    mainThread->ctx.r[15] = entryPoint;
    mainThread->ctx.tls = TLS_BASE;
    mainThread->ctx.read_pages = m_readPages;
    mainThread->ctx.write_pages = m_writePages;
    mainThread->ctx.vfp = m_mainVfp;
    mainThread->ctx.fpscr = &m_mainFpscr;
    mainThread->ctx.host = &m_host;
    mainThread->ctx.user = this;

    m_threads[0] = mainThread; // Handle 0 is main thread

    uint32_t last_pcs[16] = {0};
    int pc_idx = 0;

    while (m_running) {
        std::vector<HorizonThread*> active_threads;
        for (auto& pair : m_threads) {
            if (pair.second->active) {
                active_threads.push_back(pair.second);
            }
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

        bool any_ran = false;

        for (HorizonThread* t : active_threads) {
            if (t->wait_type == WAIT_SYNCH_1) {
                auto it = m_events.find(t->wait_handle);
                bool ready = false;
                if (it != m_events.end()) {
                    pthread_mutex_lock(&it->second->mutex);
                    if (CheckEventSignaled(it->second)) {
                        ready = true;
                        if (!it->second->manual_reset) {
                            it->second->signaled = false;
                        }
                    }
                    pthread_mutex_unlock(&it->second->mutex);
                } else {
                    ready = true;
                }

                if (ready) {
                    t->ctx.r[0] = 0; // SUCCESS
                    t->wait_type = WAIT_NONE;
                } else if (t->wait_timeout >= 0 && (now_ms - t->wait_start_ms) >= (uint64_t)(t->wait_timeout / 1000000LL)) {
                    t->ctx.r[0] = 0xD88007FA; // TIMEOUT
                    t->wait_type = WAIT_NONE;
                } else {
                    continue; // Skip running this thread
                }
            } else if (t->wait_type == WAIT_SYNCH_N) {
                int signaledIdx = -1;
                for (size_t i = 0; i < t->wait_handles.size(); ++i) {
                    auto it = m_events.find(t->wait_handles[i]);
                    if (it != m_events.end()) {
                        pthread_mutex_lock(&it->second->mutex);
                        if (CheckEventSignaled(it->second)) {
                            signaledIdx = (int)i;
                            if (!it->second->manual_reset) {
                                it->second->signaled = false;
                            }
                            pthread_mutex_unlock(&it->second->mutex);
                            break;
                        }
                        pthread_mutex_unlock(&it->second->mutex);
                    }
                }

                if (signaledIdx >= 0) {
                    t->ctx.r[0] = 0; // SUCCESS
                    t->ctx.r[1] = (uint32_t)signaledIdx;
                    t->wait_type = WAIT_NONE;
                } else if (t->wait_timeout >= 0 && (now_ms - t->wait_start_ms) >= (uint64_t)(t->wait_timeout / 1000000LL)) {
                    t->ctx.r[0] = 0xD88007FA; // TIMEOUT
                    t->wait_type = WAIT_NONE;
                } else {
                    continue; // Skip running this thread
                }
            } else if (t->wait_type == WAIT_SLEEP) {
                if (now_ms >= t->wait_start_ms + (uint64_t)(t->wait_timeout / 1000000LL)) {
                    t->wait_type = WAIT_NONE;
                } else {
                    continue; // Still sleeping
                }
            } else if (t->wait_type == WAIT_ARBITER) {
                int32_t* valPtr = (int32_t*)GetPointer(t->arbiter_addr);
                int32_t curVal = valPtr ? *valPtr : 0;
                bool satisfied = false;
                if (t->arbiter_type == 1 || t->arbiter_type == 3) {
                    if (curVal >= t->arbiter_value) {
                        satisfied = true;
                    }
                } else if (t->arbiter_type == 2 || t->arbiter_type == 4) {
                    if (curVal >= t->arbiter_value) {
                        satisfied = true;
                    }
                }
                if (satisfied) {
                    t->wait_type = WAIT_NONE;
                    t->ctx.r[0] = 0;
                } else if (t->wait_timeout >= 0 && (now_ms - t->wait_start_ms) >= (uint64_t)(t->wait_timeout / 1000000LL)) {
                    t->ctx.r[0] = 0xD88007FA; // TIMEOUT
                    t->wait_type = WAIT_NONE;
                } else {
                    continue; // Still waiting on address arbiter
                }
            }

            any_ran = true;
            m_currentThread = t;
            Context& ctx = t->ctx;
            ctx.exit = EXIT_NONE;
            ctx.budget = 200000;

            while (ctx.exit == EXIT_NONE) {
                Code code = m_host.lookup(&ctx, ctx.r[15] | ctx.thumb);
                if (!code) {
                    LOGE("Warning: Missing code at PC=0x%08X (Thumb=%d), LR=0x%08X, entry=0x%08X",
                         ctx.r[15], ctx.thumb, ctx.r[14], t->entry_point);
                    if (ctx.r[15] == 0 || ctx.r[14] == 0 || ctx.r[15] == ctx.r[14]) {
                        LOGI("Thread at entry 0x%08X terminated due to null PC/LR", t->entry_point);
                        t->active = false;
                        ctx.exit = EXIT_UNWIND;
                        break;
                    }
                    ctx.thumb = ctx.r[14] & 1;
                    ctx.r[15] = ctx.r[14] & (ctx.thumb ? ~1u : ~3u);
                    continue;
                }
                
                last_pcs[pc_idx] = ctx.r[15];
                pc_idx = (pc_idx + 1) % 16;
                
                code(&ctx);
            }

            if (ctx.exit == EXIT_SVC) {
                HandleSVC(&ctx);
            }
            if (ctx.exit == EXIT_BUDGET) {
                ctx.exit = EXIT_NONE;
            } else if (ctx.exit == EXIT_UNWIND) {
                LOGI("Thread at entry 0x%08X reached EXIT_UNWIND at PC=0x%08X", t->entry_point, ctx.r[15]);
                t->active = false;
            }
        }
        
        if (!any_ran) {
            usleep(1000); // 1ms sleep to yield CPU when all threads are waiting
        }



        static uint64_t last_dump_ms = 0;
        if (now_ms - last_dump_ms >= 2000) {
            last_dump_ms = now_ms;
            int32_t* gspLock = (int32_t*)GetPointer(0x003A5CA4);
            uint8_t* f76 = (uint8_t*)GetPointer(0x003A5C40 + 0x76);
            uint8_t* f77 = (uint8_t*)GetPointer(0x003A5C40 + 0x77);
            uint32_t qPtr = *(uint32_t*)GetPointer(0x003A5C40 + 0x34);
            uint8_t* q = (uint8_t*)GetPointer(qPtr);
            LOGI("GSP State (0x3A5C40): lock(0x3A5CA4)=%d, owner=0x%X, count=%d, f76=%d, f77=%d, qPtr=0x%08X (idx=%d, cnt=%d)",
                 gspLock ? gspLock[0] : -999,
                 gspLock ? *(uint32_t*)(gspLock + 1) : 0,
                 gspLock ? gspLock[2] : 0,
                 f76 ? *f76 : -1, f77 ? *f77 : -1,
                 qPtr, q ? q[0] : -1, q ? q[1] : -1);
            LOGI("Active threads count: %zu", active_threads.size());
            for (size_t i = 0; i < active_threads.size(); i++) {
                Context& actx = active_threads[i]->ctx;
                LOGI("Thread[%zu] entry=0x%08X, PC=0x%08X, SP=0x%08X, LR=0x%08X, wait=%d, r0=0x%X, r1=0x%X",
                     i, active_threads[i]->entry_point, actx.r[15], actx.r[13], actx.r[14], (int)active_threads[i]->wait_type, actx.r[0], actx.r[1]);
            }
        }
    }
    LOGI("RunMainThread exited.");
}

void HorizonOS::HandleSyncRequest(Context* ctx) {
    uint32_t tls = ctx->tls;
    uint32_t* cmdbuf = (uint32_t*)GetPointer(tls + 0x80);
    if (!cmdbuf) {
        ctx->r[0] = 0;
        return;
    }

    uint32_t sessionHandle = ctx->r[0];
    uint32_t header = cmdbuf[0];
    uint32_t commandId = header >> 16;
    
    std::string service = "";
    auto sIt = m_serviceHandles.find(sessionHandle);
    if (sIt != m_serviceHandles.end()) {
        service = sIt->second;
    }

    uint32_t req[64];
    memcpy(req, cmdbuf, sizeof(req));

    if (commandId != 0x000B) {
        LOGI("IPC SyncRequest: session=0x%X (svc='%s') header=0x%08X (cmd=0x%04X)",
             sessionHandle, service.c_str(), header, commandId);
    }

    // Default response header
    cmdbuf[0] = (commandId << 16) | 0x0040;
    cmdbuf[1] = 0; // Default SUCCESS to keep game progressing

    // 1. Service Manager (srv:)
    if (service == "srv:" || service.empty()) {
        if (commandId == 0x0001) { // RegisterClient
            cmdbuf[0] = 0x00010040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0002) { // EnableNotification
            HorizonEvent* notifEvt = new HorizonEvent();
            pthread_mutex_init(&notifEvt->mutex, nullptr);
            pthread_cond_init(&notifEvt->cond, nullptr);
            notifEvt->signaled = false;
            notifEvt->manual_reset = false;
            uint32_t notifHandle = ++m_nextHandle;
            m_events[notifHandle] = notifEvt;

            cmdbuf[0] = 0x00020042;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0; // Copy handle descriptor
            cmdbuf[3] = notifHandle;
            LOGI("srv:EnableNotification -> handle 0x%X", notifHandle);
        } else if (commandId == 0x0005) { // GetServiceHandle
            char name[9] = {0};
            memcpy(name, &req[1], 4);
            memcpy(name + 4, &req[2], 4);
            uint32_t newHandle = ++m_nextHandle;
            m_serviceHandles[newHandle] = std::string(name);
            LOGI("srv:GetServiceHandle for '%s' -> assigned handle 0x%X", name, newHandle);
            
            cmdbuf[0] = 0x00050042;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0; // Move handle descriptor
            cmdbuf[3] = newHandle;
        } else if (commandId == 0x000B) { // ReceiveNotification
            cmdbuf[0] = 0x000B0080;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0; // Notification ID 0 (none pending)
        }
    }
    // 2. Applet Management (apt:u / APT:A)
    else if (service.find("apt") != std::string::npos || service.find("APT") != std::string::npos) {
        if (commandId == 0x0001) { // GetLockHandle
            uint32_t lockHandle = ++m_nextHandle;
            m_serviceHandles[lockHandle] = "mutex:apt_lock";
            cmdbuf[0] = 0x000100C2;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = req[1]; // attributes
            cmdbuf[3] = 0;
            cmdbuf[4] = 0; // copy handle descriptor
            cmdbuf[5] = lockHandle;
            LOGI("APT GetLockHandle -> assigned lock handle 0x%X", lockHandle);
        } else if (commandId == 0x0002) { // Initialize
            HorizonEvent* notifEvt = new HorizonEvent();
            pthread_mutex_init(&notifEvt->mutex, nullptr);
            pthread_cond_init(&notifEvt->cond, nullptr);
            notifEvt->signaled = false;
            notifEvt->manual_reset = false;
            uint32_t notifHandle = ++m_nextHandle;
            m_events[notifHandle] = notifEvt;

            HorizonEvent* paramEvt = new HorizonEvent();
            pthread_mutex_init(&paramEvt->mutex, nullptr);
            pthread_cond_init(&paramEvt->cond, nullptr);
            paramEvt->signaled = true; // Signal initial wakeup
            paramEvt->manual_reset = false;
            uint32_t paramHandle = ++m_nextHandle;
            m_events[paramHandle] = paramEvt;

            cmdbuf[0] = 0x00020043;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0x04000000; // two copy handles descriptor
            cmdbuf[3] = notifHandle;
            cmdbuf[4] = paramHandle;
            LOGI("APT Initialize -> notif=0x%X, param=0x%X", notifHandle, paramHandle);
        } else if (commandId == 0x0003) { // Enable
            cmdbuf[0] = 0x00030040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0004) { // GetAppletManInfo
            cmdbuf[0] = 0x00040080;
            cmdbuf[1] = 0;
            cmdbuf[2] = 1; // AppletState: Active
        } else if (commandId == 0x0005) { // GlanceParameter
            cmdbuf[0] = 0x00050080;
            cmdbuf[1] = 0;
            cmdbuf[2] = 0;
        } else if (commandId == 0x000B) { // InquireNotification
            cmdbuf[0] = 0x000B0080;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0; // Signal: NONE
        } else if (commandId == 0x000D) { // ReceiveParameter
            uint32_t size = req[2];
            uint32_t* staticBufTable = (uint32_t*)GetPointer(ctx->tls + 0x180);
            uint32_t destAddr = 0;
            if (staticBufTable) {
                destAddr = staticBufTable[1];
                if (destAddr != 0 && size > 0) {
                    std::vector<uint8_t> zeroes(size, 0);
                    WriteBytes(destAddr, zeroes.data(), size);
                }
            }
            cmdbuf[0] = 0x000D0104;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0; // Sender appId
            cmdbuf[3] = 1; // Signal type 1 (Wakeup)
            cmdbuf[4] = 0; // Actual size
            cmdbuf[5] = 0x10;
            cmdbuf[6] = 0;
            cmdbuf[7] = (size << 14) | 2;
            cmdbuf[8] = destAddr;
            LOGI("APT: ReceiveParameter -> signal=1 (Wakeup), size=%u, dest=0x%08X", size, destAddr);
        } else if (commandId == 0x004B) { // AppletUtility
            uint32_t* staticBufTable = (uint32_t*)GetPointer(ctx->tls + 0x180);
            uint32_t destAddr = 0;
            uint32_t desc = 0;
            if (staticBufTable) {
                desc = staticBufTable[0];
                destAddr = staticBufTable[1];
                if (destAddr != 0) {
                    uint8_t zero = 0;
                    WriteBytes(destAddr, &zero, 1);
                }
            }
            cmdbuf[0] = 0x004B0082;
            cmdbuf[1] = 0;
            cmdbuf[2] = desc;
            cmdbuf[3] = destAddr;
            LOGI("APT: AppletUtility -> dest=0x%08X", destAddr);
        }
    }
    // 3. GPU Service (gsp::Gpu)
    else if (service.find("gsp") != std::string::npos || service.find("GSP") != std::string::npos) {
        if (commandId == 0x0013) { // RegisterInterruptRelayQueue
            uint32_t interruptEvt = req[3];
            m_gspInterruptEventHandle = interruptEvt;
            if (m_events.count(interruptEvt)) {
                m_events[interruptEvt]->name = "gsp:interruptRelay";
            }
            LOGI("GSP RegisterInterruptRelayQueue: game interrupt event handle 0x%X, returning shared mem 0x%X",
                 interruptEvt, m_gspSharedMemHandle);

            cmdbuf[0] = 0x00130082;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0;
            cmdbuf[3] = 0; // Copy handle descriptor
            cmdbuf[4] = m_gspSharedMemHandle;
        } else if (commandId == 0x000C) { // TriggerCmdReqQueue
            ProcessGspCommandQueue();
            cmdbuf[0] = 0x000C0040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0005) { // SetBufferSwap
            uint32_t screenId = req[1];
            if (screenId == 0) {
                m_topFbAddr[0] = req[2];
                m_topFbAddr[1] = req[3];
                m_topFbFormat = req[5];
            } else if (screenId == 1) {
                m_botFbAddr[0] = req[2];
                m_botFbAddr[1] = req[3];
                m_botFbFormat = req[5];
            }
            LOGI("GSP SetBufferSwap: screen=%d, left=0x%08X, right=0x%08X", screenId, req[2], req[3]);
            cmdbuf[0] = 0x00050040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0016 || commandId == 0x0017) { // AcquireRight / ReleaseRight
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0001 || commandId == 0x0002) { // WriteHwRegs
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0008 || commandId == 0x0009) { // Flush/Invalidate DataCache
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        }
    }
    // 4. Open File Operations (file:romfs or other files)
    else if (service.rfind("file:", 0) == 0) {
        if (commandId == 0x0802) { // FSFILE_Read (0x080200C2)
            uint64_t offset = ((uint64_t)req[2] << 32) | req[1];
            uint32_t size = req[3];
            uint32_t descriptor = req[4];
            uint32_t bufAddr = req[5]; // Destination buffer in guest memory
            auto it = m_openFiles.find(sessionHandle);
            size_t bytesRead = 0;
            if (it != m_openFiles.end() && it->second.fp) {
                uint64_t actualOffset = offset + it->second.romfsOffset;
                std::vector<uint8_t> temp(size);
                fseeko(it->second.fp, (off_t)actualOffset, SEEK_SET);
                bytesRead = fread(temp.data(), 1, size, it->second.fp);
                WriteBytes(bufAddr, temp.data(), bytesRead);
                LOGI("FSFILE_Read: handle 0x%X, offset=%lld (actual=0x%llX), size=%u -> read %zu bytes into 0x%08X",
                     sessionHandle, (long long)offset, (long long)actualOffset, size, bytesRead, bufAddr);
            }
            cmdbuf[0] = 0x08020082;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = (uint32_t)bytesRead;
            cmdbuf[3] = descriptor;
            cmdbuf[4] = bufAddr;
        } else if (commandId == 0x0804) { // FSFILE_GetSize (0x08040000)
            uint64_t fsize = 0;
            auto it = m_openFiles.find(sessionHandle);
            if (it != m_openFiles.end()) {
                fsize = it->second.size;
            }
            cmdbuf[0] = 0x080400C0;
            cmdbuf[1] = 0;
            cmdbuf[2] = (uint32_t)(fsize & 0xFFFFFFFF);
            cmdbuf[3] = (uint32_t)(fsize >> 32);
            LOGI("FSFILE_GetSize: handle 0x%X -> size %lld", sessionHandle, (long long)fsize);
        } else if (commandId == 0x0808) { // FSFILE_Close (0x08080000)
            auto it = m_openFiles.find(sessionHandle);
            if (it != m_openFiles.end()) {
                if (it->second.fp) fclose(it->second.fp);
                m_openFiles.erase(it);
            }
            cmdbuf[0] = 0x08080040;
            cmdbuf[1] = 0;
        }
    }
    // 5. File System Service (fs:USER)
    else if (service == "fs:USER" || service.rfind("fs:", 0) == 0 || service.rfind("FS:", 0) == 0) {
        if (commandId == 0x0801 || commandId == 0x0861 || commandId == 0x0862) { // Initialize / SetPriority
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x0802 || commandId == 0x0803) { // OpenFile / OpenFileDirectly
            uint32_t fileHandle = ++m_nextHandle;
            m_serviceHandles[fileHandle] = "file:romfs";

            HorizonFileRecord rec{};
            std::string possiblePaths[] = {
                m_storagePath + "/romfs.bin",
                "/sdcard/DBZ/romfs.bin",
                "/sdcard/romfs.bin",
                "/storage/emulated/0/DBZ/romfs.bin"
            };
            for (const auto& path : possiblePaths) {
                rec.fp = fopen(path.c_str(), "rb");
                if (rec.fp) {
                    rec.path = path;
                    fseeko(rec.fp, 0, SEEK_END);
                    rec.size = ftello(rec.fp);
                    fseeko(rec.fp, 0, SEEK_SET);

                    // Check if file is encapsulated in IVFC container (3DS RomFS partition)
                    char magic[4] = {0};
                    if (fread(magic, 1, 4, rec.fp) == 4 && memcmp(magic, "IVFC", 4) == 0) {
                        // Check if Level 3 RomFS header (0x00000028) is at offset 0x1000
                        fseeko(rec.fp, 0x1000, SEEK_SET);
                        uint32_t level3Hdr = 0;
                        if (fread(&level3Hdr, 4, 1, rec.fp) == 1 && level3Hdr == 0x28) {
                            rec.romfsOffset = 0x1000;
                            rec.size -= 0x1000;
                            LOGI("FS OpenFileDirectly: Detected IVFC container! Stripping 0x1000 IVFC header, exposing Level 3 RomFS (%lld bytes)", (long long)rec.size);
                        } else {
                            rec.romfsOffset = 0;
                        }
                    } else {
                        rec.romfsOffset = 0;
                    }
                    fseeko(rec.fp, (off_t)rec.romfsOffset, SEEK_SET);

                    LOGI("FS OpenFileDirectly: opened RomFS from %s (%lld bytes, baseOffset=0x%llX)",
                         path.c_str(), (long long)rec.size, (long long)rec.romfsOffset);
                    break;
                }
            }
            if (!rec.fp) {
                LOGW("FS OpenFileDirectly: romfs.bin not found in standard paths");
            }
            m_openFiles[fileHandle] = rec;

            cmdbuf[0] = (commandId << 16) | 0x0042;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 0; // Move handle descriptor
            cmdbuf[3] = fileHandle;
            LOGI("FS:USER OpenFile(Directly) returned handle 0x%X", fileHandle);
        } else if (commandId == 0x080C) { // OpenArchive
            uint32_t archHandle = ++m_nextHandle;
            m_serviceHandles[archHandle] = "archive:romfs";
            cmdbuf[0] = 0x080C00C0;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = archHandle;
            cmdbuf[3] = 0;
            LOGI("FS:USER OpenArchive returned handle 0x%X", archHandle);
        } else if (commandId == 0x080E || commandId == 0x080D) { // CloseArchive / ControlArchive
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        }
    }
    // 6. DSP Audio Service (dsp::DSP)
    else if (service.find("dsp") != std::string::npos || service.find("DSP") != std::string::npos) {
        if (commandId == 0x0011) { // LoadComponent
            uint32_t size = req[1];
            uint32_t descriptor = req[4];
            uint32_t buffer = req[5];
            cmdbuf[0] = 0x00110082;
            cmdbuf[1] = 0; // SUCCESS
            cmdbuf[2] = 1; // Component loaded
            cmdbuf[3] = descriptor;
            cmdbuf[4] = buffer;
            LOGI("DSP: LoadComponent size=%u", size);
        } else if (commandId == 0x0015) { // RegisterInterruptEvents
            uint32_t intType = req[1];
            uint32_t pipe = req[2];
            uint32_t evtHandle = req[4];
            m_dspInterruptEvents.push_back(evtHandle);
            if (m_events.count(evtHandle)) {
                m_events[evtHandle]->name = "dsp:interruptEvent";
            }
            LOGI("DSP RegisterInterruptEvents: type=%u, pipe=%u, handle=0x%X", intType, pipe, evtHandle);
            cmdbuf[0] = 0x00150040;
            cmdbuf[1] = 0; // SUCCESS
        } else if (commandId == 0x0016) { // GetSemaphoreEvent
            HorizonEvent* semEvt = new HorizonEvent();
            pthread_mutex_init(&semEvt->mutex, nullptr);
            pthread_cond_init(&semEvt->cond, nullptr);
            semEvt->signaled = false;
            semEvt->manual_reset = false;
            semEvt->name = "dsp:semEvt";
            uint32_t semHandle = ++m_nextHandle;
            m_events[semHandle] = semEvt;
            cmdbuf[0] = 0x00160042;
            cmdbuf[1] = 0;
            cmdbuf[2] = 0;
            cmdbuf[3] = semHandle;
            LOGI("DSP: GetSemaphoreEvent -> handle 0x%X", semHandle);
        } else if (commandId == 0x0007) { // SetSemaphore
            uint16_t semVal = req[1] & 0xFFFF;
            LOGI("DSP SetSemaphore: val=0x%04X -> signaling %zu registered DSP events", semVal, m_dspInterruptEvents.size());
            SignalDspInterrupts();
            cmdbuf[0] = 0x00070040;
            cmdbuf[1] = 0; // SUCCESS
        } else if (commandId == 0x0008) { // ClearSemaphore
            cmdbuf[0] = 0x00080040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x000D) { // WriteProcessPipe
            uint32_t channel = req[1];
            uint32_t size = req[2];
            LOGI("DSP WriteProcessPipe: channel=%u size=%u -> signaling DSP events", channel, size);
            SignalDspInterrupts();
            cmdbuf[0] = 0x000D0040;
            cmdbuf[1] = 0;
        } else if (commandId == 0x000C) { // ConvertAddress
            cmdbuf[0] = 0x000C0080;
            cmdbuf[1] = 0;
            cmdbuf[2] = 0x1ff40000;
        } else {
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        }
    }
    // 6. Input (hid:USER)
    else if (service.find("hid") != std::string::npos) {
        if (commandId == 0x000A) { // GetIPCHandle
            cmdbuf[0] = 0x000A0042;
            cmdbuf[1] = 0;
            cmdbuf[2] = 0;
            cmdbuf[3] = m_hidSharedMemHandle;
            LOGI("hid:USER GetIPCHandle -> returned handle 0x%X", m_hidSharedMemHandle);
        } else {
            cmdbuf[0] = (commandId << 16) | 0x0040;
            cmdbuf[1] = 0;
        }
    }
    // 7. Network / NDM
    else if (service.find("ndm") != std::string::npos) {
        cmdbuf[0] = (commandId << 16) | 0x0040;
        cmdbuf[1] = 0;
    }
    // 8. Error reporting (err:f)
    else if (service.find("err") != std::string::npos) {
        uint8_t specifier = req[1] & 0xFF;
        uint32_t resCode = req[2];
        uint32_t pcAddr = req[3];
        uint32_t pid = req[4];
        char msg[65] = {0};
        memcpy(msg, &req[9], sizeof(msg) - 1);
        LOGE("err:f ThrowFatalError: type=%u, res=0x%08X (desc=%u mod=%u sum=%u lev=%u), PC=0x%08X, PID=%u, msg='%s'",
             specifier, resCode,
             resCode & 0x3FF, (resCode >> 10) & 0xFF, (resCode >> 21) & 0x1F, (resCode >> 27) & 0x1F,
             pcAddr, pid, msg);
        for (int i = 0; i < 16; i += 4) {
            LOGE("err:f cmdbuf[%d..%d]: 0x%08X 0x%08X 0x%08X 0x%08X",
                 i, i+3, req[i], req[i+1], req[i+2], req[i+3]);
        }
        // Fatal error: halt this thread (consistent with 3DS kernel error screen)
        ctx->exit = EXIT_UNWIND;
        return;
    }

    ctx->r[0] = 0;
}

void HorizonOS::SetTouch(bool pressed, uint16_t x, uint16_t y) {
    m_touchPressed = pressed;
    m_touchX = x;
    m_touchY = y;
}

void HorizonOS::SetButtons(uint32_t padMask) {
    m_padState = padMask;
}

void HorizonOS::UpdateHID() {
    uint8_t* hidPtr = GetPointer(0x10000000);
    if (!hidPtr) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    uint64_t ticks = (ns * 268123480ULL) / 1000000000ULL;

    // 1. PAD State (Buttons) at offset 0x00
    // Index at 0x10
    uint32_t* padIndex = (uint32_t*)(hidPtr + 0x10);
    uint32_t idx = (*padIndex + 1) & 7;
    *padIndex = idx;

    // current_state at 0x1C
    uint32_t* padCurrentState = (uint32_t*)(hidPtr + 0x1C);
    *padCurrentState = m_padState;

    // Entries at 0x28 (8 entries x 16 bytes)
    uint8_t* entryPtr = hidPtr + 0x28 + idx * 16;
    uint32_t* entryCurrent = (uint32_t*)entryPtr;
    *entryCurrent = m_padState;

    // 2. Touch State at offset 0xA8
    // Index at 0xB8
    uint32_t* touchIndex = (uint32_t*)(hidPtr + 0xB8);
    uint32_t tIdx = (*touchIndex + 1) & 7;
    *touchIndex = tIdx;

    // raw_entry at 0xC0 (u16 x, u16 y, u32 valid)
    uint16_t* rawTouch = (uint16_t*)(hidPtr + 0xC0);
    uint32_t* rawValid = (uint32_t*)(hidPtr + 0xC4);
    rawTouch[0] = m_touchX;
    rawTouch[1] = m_touchY;
    *rawValid = m_touchPressed ? 1 : 0;

    // Touch entries at 0xC8 (8 entries x 8 bytes)
    uint8_t* tEntryPtr = hidPtr + 0xC8 + tIdx * 8;
    uint16_t* tEntryCoords = (uint16_t*)tEntryPtr;
    uint32_t* tEntryValid = (uint32_t*)(tEntryPtr + 4);
    tEntryCoords[0] = m_touchX;
    tEntryCoords[1] = m_touchY;
    *tEntryValid = m_touchPressed ? 1 : 0;
}

uint32_t HorizonOS::OpenFile(const std::string& path, uint32_t flags) {
    std::string fullPath = m_storagePath + "/romfs/" + path;
    FILE* fp = fopen(fullPath.c_str(), "rb");
    if (!fp) {
        std::string sdPath = "/sdcard/DBZ/romfs/" + path;
        fp = fopen(sdPath.c_str(), "rb");
    }
    if (!fp) return 0;

    uint32_t handle = ++m_nextHandle;
    HorizonFileRecord rec{};
    rec.fp = fp;
    fseeko(fp, 0, SEEK_END);
    rec.size = ftello(fp);
    fseeko(fp, 0, SEEK_SET);
    rec.path = fullPath;
    m_openFiles[handle] = rec;
    return handle;
}

uint32_t HorizonOS::ReadFile(uint32_t handle, uint64_t offset, uint32_t size, uint8_t* buffer) {
    auto it = m_openFiles.find(handle);
    if (it == m_openFiles.end() || !it->second.fp || !buffer) return 0;

    fseeko(it->second.fp, offset, SEEK_SET);
    return (uint32_t)fread(buffer, 1, size, it->second.fp);
}

uint64_t HorizonOS::GetFileSize(uint32_t handle) {
    auto it = m_openFiles.find(handle);
    if (it == m_openFiles.end() || !it->second.fp) return 0;
    return it->second.size;
}

void HorizonOS::CloseFile(uint32_t handle) {
    auto it = m_openFiles.find(handle);
    if (it != m_openFiles.end()) {
        if (it->second.fp) fclose(it->second.fp);
        m_openFiles.erase(it);
    }
}

static void func_0026D70C(Context *ctx) {
    HorizonOS* os = (HorizonOS*)ctx->user;
    uint32_t r0 = ctx->r[0];
    uint32_t r1 = 0;
    if (os) os->ReadBytes(r0 + 16u, &r1, 4);
    uint32_t r2 = r1 + 1u;
    if (os) os->WriteBytes(r0 + 16u, &r2, 4);
    uint8_t ch = 0;
    if (os) os->ReadBytes(r1, &ch, 1);
    ctx->r[0] = (uint32_t)ch;
    ctx->thumb = ctx->r[14] & 1;
    ctx->r[15] = ctx->r[14] & (ctx->thumb ? ~1u : ~3u);
}

// Host callbacks
uint8_t HorizonOS::HostRead8(Context* ctx, uint32_t addr) {
    uint8_t val = 0;
    if (ctx->user) ((HorizonOS*)ctx->user)->ReadBytes(addr, &val, 1);
    return val;
}
uint16_t HorizonOS::HostRead16(Context* ctx, uint32_t addr) {
    uint16_t val = 0;
    if (ctx->user) ((HorizonOS*)ctx->user)->ReadBytes(addr, &val, 2);
    return val;
}
uint32_t HorizonOS::HostRead32(Context* ctx, uint32_t addr) {
    uint32_t val = 0;
    if (ctx->user) ((HorizonOS*)ctx->user)->ReadBytes(addr, &val, 4);
    return val;
}
void HorizonOS::HostWrite8(Context* ctx, uint32_t addr, uint8_t val) {
    if (ctx->user) ((HorizonOS*)ctx->user)->WriteBytes(addr, &val, 1);
}
void HorizonOS::HostWrite16(Context* ctx, uint32_t addr, uint16_t val) {
    if (ctx->user) ((HorizonOS*)ctx->user)->WriteBytes(addr, &val, 2);
}
void HorizonOS::HostWrite32(Context* ctx, uint32_t addr, uint32_t val) {
    if (ctx->user) ((HorizonOS*)ctx->user)->WriteBytes(addr, &val, 4);
}
void HorizonOS::HostInterpret(Context* ctx, uint32_t addr, uint32_t opcode) {
    ctx->r[15] += 4;
}
extern "C" void f_001641E8(Context *ctx);

Code HorizonOS::HostLookup(Context* ctx, uint32_t addr) {
    if ((addr & ~1u) == 0x0026D70Cu) {
        return func_0026D70C;
    }
    if ((addr & ~1u) == 0x001641ECu) {
        return f_001641E8;
    }
    uint32_t left = 0;
    uint32_t right = recomp_entry_count;
    while (left < right) {
        uint32_t mid = left + (right - left) / 2;
        if (recomp_entries[mid].address < addr) {
            left = mid + 1;
        } else if (recomp_entries[mid].address > addr) {
            right = mid;
        } else {
            return recomp_entries[mid].code;
        }
    }
    return nullptr;
}
