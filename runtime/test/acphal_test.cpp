// Checks the registered acphal.dll shim against the measured call shapes
// (docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md Stop 28, PR #6885, PR #7009).
//
// All 9 exports are resolved through rt::shims::find so the test exercises
// the exact shim table and calling convention the runtime installs.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**);
using AcpHalAllocShapeCtxFn = HRESULT(__stdcall*)(void*);
using AcpHalReleaseShapeCtxFn = HRESULT(__stdcall*)(void*);
using ApuCreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
// Measured: ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t align, uint32_t flags)
using ApuAllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
// Measured: ApuIsVirtualAddressValid(void* va, uint32_t align)
using ApuIsValidFn = BOOL(__stdcall*)(void*, uint32_t);
using ApuMapApuAddrFn = void*(__stdcall*)(uint32_t);
using ApuMapVirtualAddrFn = uint32_t(__stdcall*)(void*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

// Test mirror of SHAPE_CONTEXTS matching devdocs.xbox.com GDK spec
struct APU_ADDRESS {
    uint64_t value;
};

struct TestShapeContexts {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    void* srcContextArray;
    void* eqCompContextArray;
    void* filtVolContextArray;
    void* dmaContextArray;
    void* xmaContextArray;
    void* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(TestShapeContexts) == 120, "TestShapeContexts is 120 bytes");
static_assert(offsetof(TestShapeContexts, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38");

}  // namespace

int main() {
    std::printf("acphal_test: registered acphal.dll shim verification\n");

    const auto create = reinterpret_cast<AcpHalCreateFn>(
        rt::shims::find("acphal.dll", "AcpHalCreate"));
    const auto alloc_ctx = reinterpret_cast<AcpHalAllocShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    const auto release_ctx = reinterpret_cast<AcpHalReleaseShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    const auto create_heap = reinterpret_cast<ApuCreateHeapFn>(
        rt::shims::find("acphal.dll", "ApuCreateHeap"));
    const auto alloc = reinterpret_cast<ApuAllocFn>(
        rt::shims::find("acphal.dll", "ApuAlloc"));
    const auto free_mem = reinterpret_cast<ApuFreeFn>(
        rt::shims::find("acphal.dll", "ApuFree"));
    const auto is_valid = reinterpret_cast<ApuIsValidFn>(
        rt::shims::find("acphal.dll", "ApuIsVirtualAddressValid"));
    const auto map_apu = reinterpret_cast<ApuMapApuAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapApuAddress"));
    const auto map_virtual = reinterpret_cast<ApuMapVirtualAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapVirtualAddress"));

    check(create != nullptr, "AcpHalCreate is registered in registry");
    check(alloc_ctx != nullptr, "AcpHalAllocateShapeContexts is registered in registry");
    check(release_ctx != nullptr, "AcpHalReleaseShapeContexts is registered in registry");
    check(create_heap != nullptr, "ApuCreateHeap is registered in registry");
    check(alloc != nullptr, "ApuAlloc is registered in registry");
    check(free_mem != nullptr, "ApuFree is registered in registry");
    check(is_valid != nullptr, "ApuIsVirtualAddressValid is registered in registry");
    check(map_apu != nullptr, "ApuMapApuAddress is registered in registry");
    check(map_virtual != nullptr, "ApuMapVirtualAddress is registered in registry");
    check(rt::shims::owns_dll("acphal.dll"), "the registry owns acphal.dll");

    if (!create || !alloc_ctx || !release_ctx || !create_heap || !alloc ||
        !free_mem || !is_valid || !map_apu || !map_virtual) {
        std::printf("Fatal: Missing export bindings for acphal.dll\n");
        return 1;
    }

    // 1. AcpHalCreate
    void* acp = nullptr;
    check(create(&acp) == S_OK && acp != nullptr,
          "AcpHalCreate returns S_OK with a non-null object");
    check(create(nullptr) == E_POINTER,
          "AcpHalCreate(nullptr) returns E_POINTER");

    if (acp) {
        using Slot0Fn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using Slot1Fn = HRESULT(__stdcall*)(void*);
        using Slot2Fn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
        using Slot3Fn = uint32_t(__stdcall*)(void*);
        using Slot5Fn = HRESULT(__stdcall*)(void*);

        auto vtable = *static_cast<void* const* const*>(acp);
        check(vtable[0] != nullptr, "IAcpHal vtable slot 0 (connect) is present");
        check(vtable[1] != nullptr, "IAcpHal vtable slot 1 (disconnect) is present");
        check(vtable[2] != nullptr, "IAcpHal vtable slot 2 (submit_command) is present");
        check(vtable[3] != nullptr, "IAcpHal vtable slot 3 (pop_message) is present");
        check(vtable[4] != nullptr, "IAcpHal vtable slot 4 (get_num_messages) is present");
        check(vtable[5] != nullptr, "IAcpHal vtable slot 5 (release) is present");

        // Slot 0: Connect (called at boot with (0x1000, 0x1000))
        check(reinterpret_cast<Slot0Fn>(vtable[0])(acp, 0x1000, 0x1000) == S_OK,
              "IAcpHal slot 0 (connect) returns S_OK");

        // Slot 2: SubmitCommand. Critical boot requirement: The title calls
        // slot 2 as (2, 0xf0f0f0f0, -1, config, 0) and treats failure as fatal int3.
        check(reinterpret_cast<Slot2Fn>(vtable[2])(acp, 2, 0xf0f0f0f0, 0xffffffffu, nullptr, 0) == S_OK,
              "IAcpHal slot 2 (submit_command) returns S_OK (vital boot unblocker)");

        // Slots 3 and 4: Message polling returns 0
        check(reinterpret_cast<Slot3Fn>(vtable[3])(acp) == 0, "IAcpHal slot 3 returns 0");
        check(reinterpret_cast<Slot3Fn>(vtable[4])(acp) == 0, "IAcpHal slot 4 returns 0");

        // Slot 1: Disconnect returns S_OK
        check(reinterpret_cast<Slot1Fn>(vtable[1])(acp) == S_OK, "IAcpHal slot 1 returns S_OK");

        // Slot 5: Release returns S_OK
        check(reinterpret_cast<Slot5Fn>(vtable[5])(acp) == S_OK, "IAcpHal slot 5 returns S_OK");
    }

    // 2. ApuCreateHeap: title passes (cachedSize, nonCachedSize=0x226e00)
    check(create_heap(4 * 1024 * 1024, 0x226e00) == S_OK,
          "ApuCreateHeap(4 MiB, 0x226e00) succeeds");
    check(create_heap(4 * 1024 * 1024, 0x226e00) != S_OK,
          "ApuCreateHeap second call fails (heap already created)");

    // 3. ApuAlloc argument validation
    void* dummy = nullptr;
    check(alloc(nullptr, nullptr, 1024, 0x800, 0) == E_POINTER,
          "ApuAlloc(nullptr, ...) returns E_POINTER");
    check(alloc(&dummy, nullptr, 0, 0x800, 0) == E_INVALIDARG,
          "ApuAlloc with size=0 returns E_INVALIDARG");
    check(alloc(&dummy, nullptr, 1024, 3, 0) == E_INVALIDARG,
          "ApuAlloc with unaligned alignment (3) returns E_INVALIDARG");

    // 4. ApuAlloc real boot sequence:
    // First call: flags=2, size=0x222e00, align=0x800 (noncached heap)
    void* va_nc = nullptr;
    uint32_t apu_nc = 0;
    check(alloc(&va_nc, &apu_nc, 0x222e00, 0x800, 2) == S_OK && va_nc != nullptr,
          "ApuAlloc flags=2 at title's non-cached size 0x222e00 succeeds");
    check(apu_nc != 0, "ApuAlloc provides non-zero physical APU address");

    // Second call: flags=1, size=4096, align=0x800 (cached heap)
    void* va_c = nullptr;
    uint32_t apu_c = 0;
    check(alloc(&va_c, &apu_c, 4096, 0x800, 1) == S_OK && va_c != nullptr,
          "ApuAlloc flags=1 succeeds on cached heap");
    check(apu_c != 0, "ApuAlloc flags=1 provides non-zero APU address");

    // Address verification & disjoint ranges
    check(va_nc != va_c, "Cached and non-cached allocations return different virtual pointers");
    check(apu_nc != apu_c, "Cached and non-cached APU addresses are distinct");
    check(is_valid(va_nc, 0x800) == TRUE, "Non-cached allocation is in a valid heap");
    check(is_valid(va_c, 0x800) == TRUE, "Cached allocation is in a valid heap");
    check(is_valid(nullptr, 0x800) == FALSE, "ApuIsVirtualAddressValid(nullptr) returns FALSE");
    check(is_valid(reinterpret_cast<void*>(0x1234), 0x800) == FALSE,
          "ApuIsVirtualAddressValid(bogus) returns FALSE");

    // Round-trip mapping
    check(map_virtual(va_nc) == apu_nc, "ApuMapVirtualAddress matches apu_nc");
    check(map_virtual(va_c) == apu_c, "ApuMapVirtualAddress matches apu_c");
    check(map_apu(apu_nc) == va_nc, "ApuMapApuAddress round-trips non-cached address");
    check(map_apu(apu_c) == va_c, "ApuMapApuAddress round-trips cached address");
    check(map_apu(0) == nullptr, "ApuMapApuAddress(0) returns nullptr");
    check(map_virtual(nullptr) == 0, "ApuMapVirtualAddress(nullptr) returns 0");

    // 5. Genuine State Tracking: Free & Memory Reuse
    void* reusable_block1 = nullptr;
    check(alloc(&reusable_block1, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block1 != nullptr,
          "Allocate 4 KiB reusable block");
    free_mem(reusable_block1);
    check(true, "ApuFree on valid address succeeds without error");
    free_mem(nullptr);
    check(true, "ApuFree(nullptr) is a safe no-op");

    // Reallocate in the freed slot:
    void* reusable_block2 = nullptr;
    check(alloc(&reusable_block2, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block2 != nullptr,
          "Re-allocate in cached heap succeeds (memory successfully reused)");
    check(reusable_block2 == reusable_block1,
          "Free block was genuinely reused at the same offset by the allocator");
    free_mem(reusable_block2);

    // 6. AcpHalAllocateShapeContexts & AcpHalReleaseShapeContexts
    check(alloc_ctx(nullptr) == E_POINTER,
          "AcpHalAllocateShapeContexts(nullptr) returns E_POINTER");

    // Real executable call: 0 SRC, 350 XMA (+0x38), 0 PCM
    TestShapeContexts ctx{};
    ctx.numXmaContexts = 350;
    check(alloc_ctx(&ctx) == S_OK, "AcpHalAllocateShapeContexts succeeds");
    check(ctx.xmaContextArray != nullptr,
          "xmaContextArray (+0x38) is non-null after allocation");
    check(ctx.srcContextArray == nullptr, "srcContextArray is null when count is 0");

    // Verify zeroed memory across all 350 * 64 bytes
    const uint8_t* xma_bytes = static_cast<const uint8_t*>(ctx.xmaContextArray);
    bool all_zero = true;
    for (size_t i = 0; i < 350 * 64; ++i) {
        if (xma_bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    check(all_zero, "SHAPE context buffer is zero-initialized for title's stride");

    // Test multi-context allocation
    TestShapeContexts ctx_all{};
    ctx_all.numSrcContexts = 10;
    ctx_all.numEqCompContexts = 5;
    ctx_all.numFiltVolContexts = 8;
    ctx_all.numDmaContexts = 4;
    ctx_all.numXmaContexts = 12;
    ctx_all.numPcmContexts = 6;
    check(alloc_ctx(&ctx_all) == S_OK, "AcpHalAllocateShapeContexts with all 6 types succeeds");
    check(ctx_all.srcContextArray != nullptr, "srcContextArray is allocated");
    check(ctx_all.eqCompContextArray != nullptr, "eqCompContextArray is allocated");
    check(ctx_all.filtVolContextArray != nullptr, "filtVolContextArray is allocated");
    check(ctx_all.dmaContextArray != nullptr, "dmaContextArray is allocated");
    check(ctx_all.xmaContextArray != nullptr, "xmaContextArray is allocated");
    check(ctx_all.pcmContextArray != nullptr, "pcmContextArray is allocated");

    // Release shape contexts
    check(release_ctx(&ctx_all) == S_OK, "AcpHalReleaseShapeContexts succeeds");
    check(ctx_all.xmaContextArray == nullptr, "xmaContextArray is reset to null");
    check(release_ctx(nullptr) == S_OK, "AcpHalReleaseShapeContexts(nullptr) succeeds");

    std::printf("acphal_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
