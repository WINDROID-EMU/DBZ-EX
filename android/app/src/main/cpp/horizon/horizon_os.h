#ifndef HORIZON_OS_H
#define HORIZON_OS_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <stdio.h>
#include "../recomp/recomp.h"

// Memory map constants for Nintendo 3DS Horizon OS
#define NUM_PAGES (1024 * 1024) // 4GB / 4KB
#define HORIZON_PAGE_SIZE  4096
#define HORIZON_PAGE_MASK  0xFFF
#define HORIZON_PAGE_SHIFT 12

#define CODE_BASE      0x00100000u
#define STACK_TOP      0x0FFFF000u
#define STACK_SIZE     (8 * 1024 * 1024) // 8 MiB stack
#define HEAP_BASE      0x08000000u
#define VRAM_BASE      0x18000000u
#define VRAM_SIZE      (6 * 1024 * 1024) // 6 MiB VRAM
#define MMIO_BASE      0x1EC00000u
#define MMIO_SIZE      (4 * 1024 * 1024)
#define TLS_BASE       0x1FF80000u

// Horizon OS Event object
struct HorizonEvent {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool signaled;
    bool manual_reset;
    std::string name;
    bool is_timer = false;
    uint64_t next_fire_ns = 0;
    int64_t interval_ns = 0;
};

enum ThreadWaitType {
    WAIT_NONE = 0,
    WAIT_SYNCH_1,
    WAIT_SYNCH_N,
    WAIT_SLEEP,
    WAIT_ARBITER
};

// Horizon OS Thread structure
struct HorizonThread {
    pthread_t thread = 0;
    uint32_t entry_point = 0;
    uint32_t stack_top = 0;
    uint32_t priority = 0;
    uint32_t tls_addr = 0;
    Context ctx{};
    uint32_t vfp_regs[64]{};
    uint32_t fpscr = 0;
    bool active = false;
    ThreadWaitType wait_type = WAIT_NONE;
    uint32_t wait_handle = 0;
    std::vector<uint32_t> wait_handles;
    uint32_t arbiter_addr = 0;
    int32_t arbiter_value = 0;
    uint32_t arbiter_type = 0;
    int64_t wait_timeout = 0;
    uint64_t wait_start_ms = 0;
};

struct HorizonFileRecord {
    FILE* fp = nullptr;
    uint64_t size = 0;
    uint64_t romfsOffset = 0;
    std::string path;
};

class HorizonOS {
public:
    HorizonOS();
    ~HorizonOS();

    bool Initialize(const std::string& storagePath);
    void RunMainThread(uint32_t entryPoint);
    void HandleSVC(Context* ctx);

    // Memory management
    void MapMemory(uint32_t addr, uint32_t size, bool read, bool write);
    void UnmapMemory(uint32_t addr, uint32_t size);
    uint8_t* GetPointer(uint32_t addr);

    bool ReadBytes(uint32_t addr, void* dest, size_t size);
    bool WriteBytes(uint32_t addr, const void* src, size_t size);

    // Page table arrays for recomp Context
    uint8_t** GetReadPages() { return m_readPages; }
    uint8_t** GetWritePages() { return m_writePages; }
    const Host* GetHost() { return &m_host; }

    // System Services
    void HandleSyncRequest(Context* ctx);

    // GPU & GSP methods
    void ProcessGspCommandQueue();
    void QueueGspInterrupt(uint8_t interruptId);
    void SignalEvent(uint32_t handle);
    void SignalDspInterrupts();
    void NotifyFramebufferUpdated(uint32_t address, uint32_t width, uint32_t height, uint32_t format);

    // Framebuffer access for PICA200/GSP
    uint8_t* GetTopScreenVRAM();
    uint8_t* GetBottomScreenVRAM();

    // Input state
    void UpdateHID();
    void SetTouch(bool pressed, uint16_t x, uint16_t y);
    void SetButtons(uint32_t padMask);

    // Main thread execution
    void StartGame();
    void StopGame();

    // VFS file operations
    uint32_t OpenFile(const std::string& path, uint32_t flags);
    uint32_t ReadFile(uint32_t handle, uint64_t offset, uint32_t size, uint8_t* buffer);
    uint64_t GetFileSize(uint32_t handle);
    void CloseFile(uint32_t handle);

private:
    uint8_t* m_readPages[NUM_PAGES];
    uint8_t* m_writePages[NUM_PAGES];
    Host m_host;

    std::string m_storagePath;
    std::string m_romfsPath;
    uint32_t m_currentHeapEnd;
    uint32_t m_currentLinearEnd = 0x14000000u;
    uint32_t m_nextTlsAddr;
    bool m_running;

    uint32_t m_mainVfp[64];
    uint32_t m_mainFpscr;

    // Handles map (Events, Threads, Files)
    std::unordered_map<uint32_t, HorizonEvent*> m_events;
    std::unordered_map<uint32_t, HorizonThread*> m_threads;
    std::unordered_map<uint32_t, HorizonFileRecord> m_openFiles;
    std::unordered_map<uint32_t, std::string> m_serviceHandles;
    uint32_t m_nextHandle;
    HorizonThread* m_currentThread = nullptr;

    // GSP GPU State
    uint32_t m_gspSharedMemHandle;
    uint32_t m_gspSharedMemAddr;
    uint8_t* m_gspSharedMemPtr;
    uint32_t m_gspInterruptEventHandle;
    uint32_t m_topFbAddr[2];
    uint32_t m_botFbAddr[2];
    uint32_t m_topFbFormat;
    uint32_t m_botFbFormat;
    std::vector<uint32_t> m_dspInterruptEvents;
    uint32_t m_hidSharedMemHandle = 0;
    uint32_t m_padState = 0;
    bool m_touchPressed = false;
    uint16_t m_touchX = 0;
    uint16_t m_touchY = 0;

    class PicaCommandProcessor* m_picaCmdProc = nullptr;
    bool CheckEventSignaled(HorizonEvent* evt);

    // Presentation Framebuffers (RGBA8 for GLES)
    std::vector<uint8_t> m_topFrameRGBA;
    std::vector<uint8_t> m_botFrameRGBA;
    pthread_mutex_t m_frameMutex;

    // Allocated memory tracking for cleanup
    std::vector<void*> m_allocatedBlocks;

    // Host memory fallback callbacks
    static uint8_t HostRead8(Context* ctx, uint32_t addr);
    static uint16_t HostRead16(Context* ctx, uint32_t addr);
    static uint32_t HostRead32(Context* ctx, uint32_t addr);
    static void HostWrite8(Context* ctx, uint32_t addr, uint8_t val);
    static void HostWrite16(Context* ctx, uint32_t addr, uint16_t val);
    static void HostWrite32(Context* ctx, uint32_t addr, uint32_t val);
    static void HostInterpret(Context* ctx, uint32_t addr, uint32_t opcode);
    static Code HostLookup(Context* ctx, uint32_t addr);
};

extern HorizonOS* g_Horizon;

#endif // HORIZON_OS_H
