// acphal.dll: Audio Control Processor HAL. Scalable Hardware Audio Processing Engine
// (SHAPE) and APU (Audio Processing Unit) physical memory management for Xbox Series.
//
// On a PC there is no SHAPE/ACP hardware and APU physical memory does not exist.
// This shim creates the APU heap from process memory and manages allocations within it.
// The SHAPE context arrays are allocated on the process heap; the ACP interface
// accepts and acknowledges commands to allow audio bring-up to proceed cleanly.
//
// Signatures from the public GDK reference (devdocs.xbox.com):
//   AcpHal, APU reference, SHAPE_CONTEXTS, APU_ADDRESS.
// Measured call-site shapes and findings from the retail executable (1.0.2.1):
//   - docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md (Stop 28)
//   - PR #6885 (call sites and parameter analysis), PR #7009 (audio/HAL bring-up).

#include <windows.h>

#include <atomic>
#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "shims.h"
#include "startup_trace.h"

namespace {

// ---- APU_ADDRESS (from the GDK) ----

// The console APU uses a 40-bit physical address space.
// On PC we translate to a 32-bit APU address offset.
struct APU_ADDRESS {
    uint64_t value;  // 40-bit physical address
};

// ---- SHAPE context structures (from the public GDK documentation at devdocs.xbox.com) ----
//
// These structures are defined here with their documented GDK field layouts so that
// sizeof() returns the correct per-context byte count. Without the real layouts the
// empty structs would be 1 byte each, and the title's writes (e.g. memset with the
// real stride) would overflow the allocation.

// UINT32 bitfields packed into 32-bit words
struct SHAPE_SRC_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t blocksToSkip : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t samplingIncrement : 21;
    uint32_t reserved0 : 1;
    uint32_t cmd : 2;
    uint32_t mixBufPeakMag2 : 4;
    uint32_t peakMag2 : 4;
    uint32_t sampleCount;
    uint32_t samplePointer : 25;
    uint32_t reserved1 : 7;
    uint32_t samplingIncrementTarget : 21;
    uint32_t reserved2 : 11;
    uint32_t reserved[3];
};
static_assert(sizeof(SHAPE_SRC_CONTEXT) == 32, "SHAPE_SRC_CONTEXT is 32 bytes per GDK");

// The EQ/Comp context: 88 UINT32s = 352 bytes per GDK definition.
#pragma pack(push, 1)
struct alignas(4) SHAPE_EQCOMP_CONTEXT {
    uint8_t data[352];
};
#pragma pack(pop)
static_assert(sizeof(SHAPE_EQCOMP_CONTEXT) == 352, "SHAPE_EQCOMP_CONTEXT is 352 bytes per GDK");

struct SHAPE_FILTVOL_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t delay1 : 30;
    uint32_t headroom : 2;
    uint32_t delay2 : 30;
    uint32_t svfMode : 2;
    uint32_t gainTarget : 16;
    uint32_t gain : 16;
    uint32_t qRecip : 24;
    uint32_t reserved1 : 8;
    uint32_t qRecipTarget : 24;
    uint32_t reserved2 : 8;
    uint32_t fc : 24;
    uint32_t reserved3 : 8;
    uint32_t fcTarget : 24;
    uint32_t reserved4 : 8;
};
static_assert(sizeof(SHAPE_FILTVOL_CONTEXT) == 32, "SHAPE_FILTVOL_CONTEXT is 32 bytes per GDK");

struct SHAPE_DMA_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 3;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t readPointer : 5;
    uint32_t reserved1 : 3;
    uint32_t writePointer : 5;
    uint32_t reserved2 : 3;
    uint32_t full : 1;
    uint32_t reserved3 : 15;
    uint32_t reserved4 : 9;
    uint32_t address : 23;
    uint32_t numFrames : 5;
    uint32_t reserved5 : 3;
    uint32_t floatConvert : 1;
    uint32_t reserved6 : 7;
    uint32_t numChannels : 3;
    uint32_t reserved7 : 5;
    uint32_t channel : 3;
    uint32_t reserved8 : 5;
};
static_assert(sizeof(SHAPE_DMA_CONTEXT) == 16, "SHAPE_DMA_CONTEXT is 16 bytes per GDK");

struct SHAPE_XMA_CONTEXT {
    uint32_t sizeRead0 : 12;
    uint32_t numLoops : 8;
    uint32_t validBuffer : 2;
    uint32_t sizeWrite : 5;
    uint32_t offsetWrite : 5;
    uint32_t sizeRead1 : 12;
    uint32_t sequenceNumber : 12;
    uint32_t stopSampleAddress : 8;
    uint32_t subframeOffset : 4;
    uint32_t reserved0 : 1;
    uint32_t isLooping : 1;
    uint32_t subframeSkip : 2;
    uint32_t packetMetadata : 16;
    uint32_t currentByteOffset : 26;
    uint32_t reserved1 : 6;
    uint32_t loopStartOffset : 26;
    uint32_t loopEndOffset : 6;
    // The fields above fill five words: currentByteOffset (26 bits) does not fit in the 8 bits left of the third, so
    // it starts the fourth (MSVC, GCC and Clang alike), and with reserved[10] the context was 60 bytes, not the 64
    // of the assert below (the Windows build failed on it). The context array the title shares is a stride of 64:
    // 16 words per XMA context, as VIctory records them (crates/victory-hle/src/modules/audio.rs). Which of words
    // 1-4 the bit fields above belong to is not confirmed here; only the size is used (alloc_array).
    uint32_t reserved[11];
};
static_assert(sizeof(SHAPE_XMA_CONTEXT) == 64, "SHAPE_XMA_CONTEXT is 64 bytes per GDK");

struct SHAPE_PCM_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 11;
    uint32_t loopCount : 8;
    uint32_t reserved1 : 8;
    uint32_t full : 1;
    uint32_t reserved2 : 15;
    uint32_t readPointer : 21;
    uint32_t reserved3 : 11;
    uint32_t loopStartWritePointer : 21;
    uint32_t reserved4 : 11;
    uint32_t loopEnd : 21;
    uint32_t reserved5 : 11;
    uint32_t mode : 1;
    uint32_t reserved6 : 15;
    uint32_t format : 2;
    uint32_t reserved7 : 14;
    // Six words of fields: with reserved[1] the context was 28 bytes, not the 32 of the assert below (the Windows
    // build failed on it). Only the size is used (alloc_array); the trailing words are reserved.
    uint32_t reserved[2];
};
static_assert(sizeof(SHAPE_PCM_CONTEXT) == 32, "SHAPE_PCM_CONTEXT is 32 bytes per GDK");

// The SHAPE_CONTEXTS structure (from the GDK)
struct SHAPE_CONTEXTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    SHAPE_SRC_CONTEXT* srcContextArray;
    SHAPE_EQCOMP_CONTEXT* eqCompContextArray;
    SHAPE_FILTVOL_CONTEXT* filtVolContextArray;
    SHAPE_DMA_CONTEXT* dmaContextArray;
    SHAPE_XMA_CONTEXT* xmaContextArray;
    SHAPE_PCM_CONTEXT* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(SHAPE_CONTEXTS) == 120, "SHAPE_CONTEXTS is 120 bytes per GDK");
static_assert(offsetof(SHAPE_CONTEXTS, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38 per retail binary measurement");

// ---- Startup trace helper ----

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(std::string(api) + detail).second) return;
    }
    rt::startup_trace::event("acphal",
                             std::string("\"api\":\"") + api + "\"" + detail);
}

// ---- Genuine APU Heap Implementation ----

constexpr uint32_t kApuBaseAddress = 0x40000000u;

struct ApuRegion {
    void* base = nullptr;
    uint64_t size = 0;
    std::map<uint32_t, uint32_t> blocks;  // offset -> size of active allocations

    bool contains(const void* ptr) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        return p >= b && p < b + size;
    }

    bool is_valid_address(const void* ptr, uint32_t alignment) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        if (p < b || p >= b + size) return false;
        if (alignment == 0) alignment = 1;
        return ((p - b) & (static_cast<uintptr_t>(alignment) - 1)) == 0;
    }

    uint32_t to_offset(const void* ptr) const {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) -
                                     reinterpret_cast<uintptr_t>(base));
    }

    void* from_offset(uint32_t offset) const {
        if (offset >= size) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    void* allocate(uint32_t alloc_size, uint32_t alignment, uint32_t* out_offset) {
        if (alloc_size == 0 || alignment == 0) return nullptr;
        uint64_t at = 0;
        for (auto it = blocks.begin();; ++it) {
            at = (at + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
            const uint64_t limit = (it == blocks.end()) ? size : it->first;
            if (at + alloc_size <= limit) break;
            if (it == blocks.end()) return nullptr;  // out of memory
            at = static_cast<uint64_t>(it->first) + it->second;
        }
        const uint32_t offset = static_cast<uint32_t>(at);
        blocks.emplace(offset, alloc_size);
        uint8_t* ptr = static_cast<uint8_t*>(base) + offset;
        std::memset(ptr, 0, alloc_size);
        if (out_offset) *out_offset = offset;
        return ptr;
    }

    bool free_block(void* ptr) {
        if (!contains(ptr)) return false;
        const uint32_t offset = to_offset(ptr);
        return blocks.erase(offset) > 0;
    }

    void release() {
        if (base) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
        size = 0;
        blocks.clear();
    }
};

std::mutex g_apu_mutex;
bool g_apu_heap_created = false;
ApuRegion g_cached_heap;
ApuRegion g_noncached_heap;

// Tracked SHAPE context arrays
std::vector<void*> g_active_shape_arrays;

// ---- IAcpHal Interface Implementation ----
//
// The audio coprocessor's message protocol, as VIctory records the retail title using it (docs/related_projects_gaps.md,
// row 4; its crates/victory-hle/src/modules/audio.rs): reimplemented here from those facts, nothing copied.
// - connect(commands, messages): the second count is the size of the message queue; zero commands is refused.
// - submit(command, id, frame, data, notification) completes at once. Commands 2 and 3 register and unregister the
//   message kinds of data's first word (a mask); command 1 loads a flowgraph and posts FLOWGRAPH_COMPLETED with
//   data's second word. A command with a non-zero id is acknowledged with COMMAND_COMPLETED {command, id, frame}, and
//   a non-zero notification is the APU address of a 32-bit word that is then set to 1.
// - Only registered kinds are queued. A full queue drops a message and counts it in the next one that fits.
// - pop_message copies the oldest message (28 bytes: kind, dropped count, microseconds, 16 bytes of data) and says
//   whether there was one; get_num_messages counts them.
// Nothing is processed: no SHAPE unit runs, so commands for the XMA contexts are acknowledged and not carried out.

void* __stdcall ApuMapApuAddress(uint32_t apu);   // below, with the heap

constexpr uint32_t kAcpLoadFlowgraph = 1;
constexpr uint32_t kAcpRegisterMessages = 2;
constexpr uint32_t kAcpUnregisterMessages = 3;
constexpr uint32_t kAcpMessageFlowgraphCompleted = 0x02;
constexpr uint32_t kAcpMessageCommandCompleted = 0x10;
constexpr uint32_t kAcpMessageDisconnected = 0x80;
constexpr uint64_t kAcpFramesPerSecond = 375;   // audio frames of 128 samples at 48 kHz

#pragma pack(push, 1)
struct AcpMessage {
    uint32_t kind;
    uint32_t dropped;   // messages a full queue lost since the one before this
    uint32_t usec;      // microseconds since the object was created
    uint8_t data[16];
};
#pragma pack(pop)
static_assert(sizeof(AcpMessage) == 28, "an ACP message is 28 bytes");

struct AcpHalImpl {
    // Slot 0: Connect (called at boot with (0x1000, 0x1000), result checked)
    virtual HRESULT __stdcall connect(uint32_t commands, uint32_t messages) {
        trace_call("AcpHal::Connect", "");
        if (commands == 0) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        connected_ = true;
        capacity_ = messages;
        return S_OK;
    }

    // Slot 1: Disconnect (called at shutdown)
    virtual HRESULT __stdcall disconnect() {
        trace_call("AcpHal::Disconnect", "");
        std::lock_guard lock(mutex_);
        if (connected_) post(kAcpMessageDisconnected, nullptr, 0);
        connected_ = false;
        return S_OK;
    }

    // Slot 2: SubmitCommand. The title calls it as (2, 0xf0f0f0f0, -1, config, 0) at boot, and a negative result
    // triggers a fatal int3 (+0x43ad94e), so it never fails.
    virtual HRESULT __stdcall submit_command(uint32_t command, uint64_t id, uint32_t /*frame*/, const void* data,
                                            uint32_t notification) {
        trace_call("AcpHal::SubmitCommand", "");
        const auto word = [data](size_t i) {
            uint32_t w = 0;
            if (data) std::memcpy(&w, static_cast<const uint8_t*>(data) + 4 * i, sizeof(w));
            return w;
        };
        {
            std::lock_guard lock(mutex_);
            if (command == kAcpRegisterMessages) {
                mask_ |= word(0);
            } else if (command == kAcpUnregisterMessages) {
                mask_ &= ~word(0);
            } else if (command == kAcpLoadFlowgraph) {
                const uint32_t flowgraph = word(1);
                post(kAcpMessageFlowgraphCompleted, &flowgraph, sizeof(flowgraph));
            } else {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr, "[rt] acphal: commands are acknowledged, not carried out: no audio DSP\n");
            }
            if (id != 0) {
                uint8_t done[16] = {};
                const uint32_t frame = this->frame();
                std::memcpy(done, &command, 4);
                std::memcpy(done + 4, &id, 8);
                std::memcpy(done + 12, &frame, 4);
                post(kAcpMessageCommandCompleted, done, sizeof(done));
            }
        }
        if (notification != 0) {
            // Outside this object's lock: the heap has its own (g_apu_mutex).
            if (void* p = ApuMapApuAddress(notification))
                InterlockedExchange(static_cast<volatile LONG*>(p), 1);   // the title reads it as volatile
        }
        return S_OK;
    }

    // Slot 3: PopMessage. 1 with the oldest message copied to `out` (when not null), 0 when there is none; a full
    // 32-bit value, so a caller that reads more than AL sees 0 or 1.
    virtual uint32_t __stdcall pop_message(AcpMessage* out) {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return 0;
        if (out) std::memcpy(out, &queue_.front(), sizeof(AcpMessage));
        queue_.pop_front();
        return 1;
    }

    // Slot 4: GetNumMessages
    virtual uint32_t __stdcall get_num_messages() {
        std::lock_guard lock(mutex_);
        return static_cast<uint32_t>(queue_.size());
    }

    // Slot 5: Release
    virtual HRESULT __stdcall release() {
        trace_call("AcpHal::Release", "");
        delete this;
        return S_OK;
    }

    virtual ~AcpHalImpl() = default;

private:
    uint32_t usec() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
    }
    uint32_t frame() const {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_);
        return static_cast<uint32_t>(static_cast<uint64_t>(us.count()) * kAcpFramesPerSecond / 1000000);
    }
    // Queues a message of a registered kind (mutex_ held).
    void post(uint32_t kind, const void* bytes, size_t len) {
        if ((mask_ & kind) == 0) return;
        if (queue_.size() >= capacity_) {
            ++dropped_;
            return;
        }
        AcpMessage m{};
        m.kind = kind;
        m.dropped = dropped_;
        m.usec = usec();
        if (bytes) std::memcpy(m.data, bytes, len < sizeof(m.data) ? len : sizeof(m.data));
        dropped_ = 0;
        queue_.push_back(m);
    }

    std::mutex mutex_;
    bool connected_ = false;
    uint32_t mask_ = 0;
    size_t capacity_ = 0;
    uint32_t dropped_ = 0;
    std::deque<AcpMessage> queue_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// ---- AcpHalCreate ----

HRESULT __stdcall AcpHalCreate(void** ppAcpHal) {
    if (!ppAcpHal) return E_POINTER;
    *ppAcpHal = new AcpHalImpl();
    trace_call("AcpHalCreate", "");
    return S_OK;
}

// ---- AcpHalAllocateShapeContexts ----

HRESULT __stdcall AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;

    std::lock_guard lock(g_apu_mutex);

    auto alloc_array = [](uint32_t count, size_t elem_size) -> void* {
        if (count == 0) return nullptr;
        void* ptr = std::calloc(count, elem_size);
        if (ptr) g_active_shape_arrays.push_back(ptr);
        return ptr;
    };

    ctx->srcContextArray = static_cast<SHAPE_SRC_CONTEXT*>(
        alloc_array(ctx->numSrcContexts, sizeof(SHAPE_SRC_CONTEXT)));
    ctx->eqCompContextArray = static_cast<SHAPE_EQCOMP_CONTEXT*>(
        alloc_array(ctx->numEqCompContexts, sizeof(SHAPE_EQCOMP_CONTEXT)));
    ctx->filtVolContextArray = static_cast<SHAPE_FILTVOL_CONTEXT*>(
        alloc_array(ctx->numFiltVolContexts, sizeof(SHAPE_FILTVOL_CONTEXT)));
    ctx->dmaContextArray = static_cast<SHAPE_DMA_CONTEXT*>(
        alloc_array(ctx->numDmaContexts, sizeof(SHAPE_DMA_CONTEXT)));
    ctx->xmaContextArray = static_cast<SHAPE_XMA_CONTEXT*>(
        alloc_array(ctx->numXmaContexts, sizeof(SHAPE_XMA_CONTEXT)));
    ctx->pcmContextArray = static_cast<SHAPE_PCM_CONTEXT*>(
        alloc_array(ctx->numPcmContexts, sizeof(SHAPE_PCM_CONTEXT)));

    // On PC there is no physical APU address translation for host context arrays;
    // assign unique synthetic addresses from the APU base.
    ctx->apuSrcContextArray = {ctx->srcContextArray ? kApuBaseAddress : 0};
    ctx->apuEqCompContextArray = {ctx->eqCompContextArray ? kApuBaseAddress + 0x1000 : 0};
    ctx->apuFiltVolContextArray = {ctx->filtVolContextArray ? kApuBaseAddress + 0x2000 : 0};
    ctx->apuDmaContextArray = {ctx->dmaContextArray ? kApuBaseAddress + 0x3000 : 0};
    ctx->apuXmaContextArray = {ctx->xmaContextArray ? kApuBaseAddress + 0x4000 : 0};
    ctx->apuPcmContextArray = {ctx->pcmContextArray ? kApuBaseAddress + 0x5000 : 0};

    trace_call("AcpHalAllocateShapeContexts",
               ",\"src\":" + std::to_string(ctx->numSrcContexts) +
               ",\"xma\":" + std::to_string(ctx->numXmaContexts) +
               ",\"pcm\":" + std::to_string(ctx->numPcmContexts));
    return S_OK;
}

// ---- AcpHalReleaseShapeContexts ----

HRESULT __stdcall AcpHalReleaseShapeContexts(SHAPE_CONTEXTS* ctx = nullptr) {
    std::lock_guard lock(g_apu_mutex);

    // Free all recorded context allocations
    for (void* p : g_active_shape_arrays) {
        if (p) std::free(p);
    }
    g_active_shape_arrays.clear();

    if (ctx) {
        ctx->srcContextArray = nullptr;
        ctx->eqCompContextArray = nullptr;
        ctx->filtVolContextArray = nullptr;
        ctx->dmaContextArray = nullptr;
        ctx->xmaContextArray = nullptr;
        ctx->pcmContextArray = nullptr;
    }

    trace_call("AcpHalReleaseShapeContexts", "");
    return S_OK;
}

// ---- ApuCreateHeap ----

HRESULT __stdcall ApuCreateHeap(uint32_t cachedSizeInBytes,
                                uint32_t nonCachedSizeInBytes) {
    std::lock_guard lock(g_apu_mutex);
    if (g_apu_heap_created) return E_FAIL;

    if (cachedSizeInBytes > 0) {
        g_cached_heap.base = VirtualAlloc(
            nullptr, cachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_cached_heap.base) return E_OUTOFMEMORY;
        g_cached_heap.size = cachedSizeInBytes;
    }

    if (nonCachedSizeInBytes > 0) {
        g_noncached_heap.base = VirtualAlloc(
            nullptr, nonCachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_noncached_heap.base) {
            g_cached_heap.release();
            return E_OUTOFMEMORY;
        }
        g_noncached_heap.size = nonCachedSizeInBytes;
    }

    g_apu_heap_created = true;
    trace_call("ApuCreateHeap",
               ",\"cachedSize\":" + std::to_string(cachedSizeInBytes) +
               ",\"nonCachedSize\":" + std::to_string(nonCachedSizeInBytes));
    return S_OK;
}

// ---- ApuAlloc ----

HRESULT __stdcall ApuAlloc(void** virtualAddress, uint32_t* physicalAddress,
                            uint32_t sizeInBytes, uint32_t alignmentInBytes,
                            uint32_t flags) {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = 0;

    if (sizeInBytes == 0) return E_INVALIDARG;
    if (alignmentInBytes == 0) alignmentInBytes = 1;
    if ((alignmentInBytes & (alignmentInBytes - 1)) != 0) return E_INVALIDARG;

    std::lock_guard lock(g_apu_mutex);

    // Routing: flags=2 routes to non-cached heap; flags=0 or 1 routes to cached heap.
    // If non-cached heap wasn't created or has no room, fall back to cached heap
    // because PC memory is unified and cache-coherent.
    const bool prefer_noncached = (flags == 2);
    ApuRegion* target = prefer_noncached ? &g_noncached_heap : &g_cached_heap;

    if (!target->base) {
        if (!prefer_noncached) {
            // Lazy default cached heap if ApuCreateHeap wasn't called yet
            constexpr uint32_t kDefaultHeapSize = 64 * 1024 * 1024;
            target->base = VirtualAlloc(nullptr, kDefaultHeapSize,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!target->base) return E_OUTOFMEMORY;
            target->size = kDefaultHeapSize;
            g_apu_heap_created = true;
        } else {
            // Fall back to cached heap
            target = &g_cached_heap;
            if (!target->base) return E_OUTOFMEMORY;
        }
    }

    uint32_t offset = 0;
    void* ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    if (!ptr && prefer_noncached && g_cached_heap.base) {
        // Fall back to cached heap if non-cached heap is full
        target = &g_cached_heap;
        ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    }

    if (!ptr) return E_OUTOFMEMORY;

    *virtualAddress = ptr;
    if (physicalAddress) {
        // Disjoint APU address ranges:
        //   cached:     0x40000000 .. 0x40000000 + cached_size - 1
        //   non-cached: 0x40000000 + cached_size .. 0x40000000 + cached_size + noncached_size - 1
        uint32_t apu = kApuBaseAddress + offset;
        if (target == &g_noncached_heap) {
            apu += static_cast<uint32_t>(g_cached_heap.size);
        }
        *physicalAddress = apu;
    }

    return S_OK;
}

// ---- ApuFree ----

void __stdcall ApuFree(void* virtualAddress) {
    if (!virtualAddress) return;
    std::lock_guard lock(g_apu_mutex);
    if (!g_cached_heap.free_block(virtualAddress)) {
        g_noncached_heap.free_block(virtualAddress);
    }
    trace_call("ApuFree", "");
}

// ---- ApuIsVirtualAddressValid ----

BOOL __stdcall ApuIsVirtualAddressValid(void* virtualAddress, uint32_t alignment) {
    if (!virtualAddress) return FALSE;
    if (alignment == 0) alignment = 1;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    if (g_noncached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    return FALSE;
}

// ---- ApuMapApuAddress ----

void* __stdcall ApuMapApuAddress(uint32_t apu) {
    if (apu == 0 || apu < kApuBaseAddress) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    const uint32_t offset = apu - kApuBaseAddress;
    if (g_cached_heap.base && offset < g_cached_heap.size) {
        return g_cached_heap.from_offset(offset);
    }
    if (g_noncached_heap.base) {
        const uint32_t cached_span = static_cast<uint32_t>(g_cached_heap.size);
        if (offset >= cached_span && offset - cached_span < g_noncached_heap.size) {
            return g_noncached_heap.from_offset(offset - cached_span);
        }
    }
    return nullptr;
}

// ---- ApuMapVirtualAddress ----

uint32_t __stdcall ApuMapVirtualAddress(void* virtualAddress) {
    if (!virtualAddress) return 0;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + g_cached_heap.to_offset(virtualAddress);
    }
    if (g_noncached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + static_cast<uint32_t>(g_cached_heap.size) +
               g_noncached_heap.to_offset(virtualAddress);
    }
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);
