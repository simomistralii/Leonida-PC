// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are refused without reaching the device, the others get the real objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each).
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"

#include <d3d12.h>
#include <windows.h>

#include <crtdbg.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// ---- Mocks ----

enum class Kind { device, queue, fence, resource, allocator, command_list, heap };

constexpr unsigned kMockSlots = 128;
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    int list_level;          // command_list: 0=base (60 slots), 7=GCL7 (81 slots), 8=GCL8 (82 slots)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    bool allocator_tag;      // allocator: answers kLeonidaCommandAllocatorTag; device: the allocators it makes do
    bool create_null;        // device: creation returns create_result with no object
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    Mock* last_child;        // device: the object its last creation or open made
    std::atomic<std::uint64_t> completed;   // fence: what slot 8 returns (kCompletedValue when made)
    // device: the descriptions slots 18, 21, 25, 27, 29 and 30 last received (what the PC device would get)
    D3D12_RESOURCE_DESC desc_seen[2];
    std::uint64_t descs_seen;               // slot 25's count
    D3D12_CLEAR_VALUE clear_seen;
    bool clear_given;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_seen;
    bool srv_given;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_seen;
    bool dsv_given;
    D3D12_RESOURCE_DESC resource_desc;      // resource: what its GetDesc (slot 10) returns
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][5];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
};

Mock g_pool[256];
unsigned g_pool_used = 0;

void** mock_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = mock_vtable();
    m->kind = kind;
    return m;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::heap: return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap));
        case Kind::command_list:
            if (IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList))) return true;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
            if (m->list_level >= 7 && (IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1))))
                return true;
#endif
            return false;
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kLeonidaCommandAllocatorTag));
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    child->completed.store(kCompletedValue);
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// `words` are the call's first five words (a1..a5), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3];
    if (m->kind == Kind::device) {
        switch (slot) {
            case 8: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));    // CreateCommandQueue
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 14: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));       // CreateDescriptorHeap
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 25: {                                                      // (buffer, mask, count, descs)
                const auto* d = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
                m->descs_seen = a3;
                for (std::uint64_t i = 0; d && i < a3 && i < 2; ++i) m->desc_seen[i] = d[i];
                break;
            }
            case 27: case 29: case 30: {          // (props, flags | heap, offset, desc, state, clear) / (desc, state, clear)
                const std::uint64_t desc = slot == 30 ? a1 : a3, clear = slot == 30 ? a3 : words[4];
                if (desc) m->desc_seen[0] = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc);
                m->clear_given = clear != 0;
                if (clear) m->clear_seen = *reinterpret_cast<const D3D12_CLEAR_VALUE*>(clear);
                break;
            }
            case 18:                                                        // (resource, desc, handle)
                m->srv_given = a2 != 0;
                if (a2) m->srv_seen = *reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(a2);
                break;
            case 21:                                                        // (resource, desc, handle)
                m->dsv_given = a2 != 0;
                if (a2) m->dsv_seen = *reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(a2);
                break;
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence && slot == 8) return m->completed.load();  // GetCompletedValue
    if (m->kind == Kind::resource && slot == 10 && a1) {                  // GetDesc, through the hidden pointer
        std::memcpy(reinterpret_cast<void*>(a1), &m->resource_desc, sizeof(m->resource_desc));
        return a1;
    }
    if (m->kind == Kind::queue && slot == 10) copy_array(m, slot, a2, a1);  // ExecuteCommandLists(count, lists)
    if (m->kind == Kind::command_list && slot == 28) copy_array(m, slot, a2, a1);  // SetDescriptorHeaps(count, heaps)
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        return static_cast<std::uint64_t>(--self->refs);
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        const std::uint64_t words[] = {a1, a2, a3, a4, a5};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(Kind::command_list);
    child->list_level = self->list_level;
    self->last_child = child;
    return mock_query(child, riid, out);
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, 0, 0, 0, 0, 0,
                                                  0, 0, 0);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};

// A device proxy over a new mock device. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device)))
        return nullptr;
    return device;
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// ---- 1. Device interface levels ----

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46: ID3D12Device1
        const HRESULT r45 = hr_of(raw(device, 45, word(fences), word(values), 2, 0, 0));
        const HRESULT r46 = hr_of(raw(device, 46, 4, word(objects), word(priorities)));
        if (l.level >= 1) {
            EXPECT(r45 == S_OK && mock->calls[45] == 1 && mock->array[45][0] == real_fence &&
                   mock->array[45][1] == real_fence);
            EXPECT(r46 == S_OK && mock->calls[46] == 1 && same(mock->array[46], unwrapped, 4));
        } else {
            EXPECT(r45 == E_NOTIMPL && r46 == E_NOTIMPL && mock->calls[45] == 0 && mock->calls[46] == 0);
            void* device1 = nullptr;
            EXPECT(FAILED(hr_of(raw(device, 0, word(&kDevice1Iid), word(&device1)))) && !device1);
        }
        // 50: ID3D12Device3, with the fence argument unwrapped too
        const HRESULT r50 = hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1));
        if (l.level >= 3) {
            EXPECT(r50 == S_OK && mock->calls[50] == 1 && same(mock->array[50], unwrapped, 4) &&
                   mock->args[50][3] == real_fence);
        } else {
            EXPECT(r50 == E_NOTIMPL && mock->calls[50] == 0);
        }
        // 75: ID3D12Device9::CreateCommandQueue1
        void* queue1 = reinterpret_cast<void*>(1);  // cleared on every failure (bc59ef46's convention)
        const HRESULT r75 = hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&queue1)));
        if (l.level >= 9) {
            EXPECT(r75 == S_OK && xs::is_queue_proxy(queue1) && mock->calls[75] == 1);
            EXPECT(release(queue1) == 0 && mock->last_child->refs == 0);
        } else {
            EXPECT(r75 == E_NOTIMPL && !queue1 && mock->calls[75] == 0);
        }

        // 133: past every PC device vtable; the retail title calls it. Refused, not a jump through a missing entry.
        EXPECT(hr_of(raw(device, 133)) == E_NOTIMPL && mock->calls[127] == 0);

        // 133 with a handler (CreatePlacedResourceX): the arguments in the title's positions (va, desc, state,
        // clear, riid, out) reach the handler with the real device, the Xbox flag bits masked to the PC ones the host
        // takes (0x20004 -> 0x4, and 0x200 dropped) and the buffer's Xbox layout word made ROW_MAJOR.
        {
            static void* seen_real = nullptr;
            static void* seen_va = nullptr;
            static D3D12_RESOURCE_DESC seen_desc{};
            static GUID seen_iid{};
            static std::uint32_t seen_layout = 0;
            const xs::PlacedAtVaHandler handler = [](ID3D12Device* real_device, void* va,
                                                     const D3D12_RESOURCE_DESC& desc, std::uint32_t layout,
                                                     REFIID iid, void** out) -> HRESULT {
                seen_layout = layout;
                seen_real = real_device;
                seen_va = va;
                seen_desc = desc;
                seen_iid = iid;
                *out = reinterpret_cast<void*>(0x1330);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0x10000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the Xbox row-major word
            xbox_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20204);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000000000ull);
            const HRESULT r133 = hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0,
                                           word(&resource_iid), word(&placed)));
            xs::set_placed_at_va_handler(nullptr);
            EXPECT(r133 == S_OK && placed == reinterpret_cast<void*>(0x1330));
            EXPECT(seen_real == xs::device_proxy_real_device(device) && seen_va == va);
            EXPECT(static_cast<std::uint32_t>(seen_desc.Flags) == 0x4);
            EXPECT(seen_desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && seen_desc.Width == 0x10000);
            EXPECT(std::memcmp(&seen_iid, &resource_iid, sizeof(GUID)) == 0);
            EXPECT(seen_layout == 0xFFFFFFFFu);  // the title's third word, passed through as the barrier layout
            EXPECT(xs::device_slot_overridden(133));
        }

        // 133 with a pages hook (set_placed_at_va_pages_hook): a buffer's range [va, va + width) is handed to it
        // before the handler runs; a texture is not; a hook that fails still leaves the call to the handler.
        {
            static int order = 0;
            static int hook_at = 0;
            static int handler_at = 0;
            static const void* hook_va = nullptr;
            static std::size_t hook_bytes = 0;
            static bool hook_result = true;
            order = hook_at = handler_at = 0;
            const xs::PlacedAtVaPagesHook hook = [](const void* va, std::size_t bytes) -> bool {
                hook_at = ++order;
                hook_va = va;
                hook_bytes = bytes;
                return hook_result;
            };
            const xs::PlacedAtVaHandler handler = [](ID3D12Device*, void*, const D3D12_RESOURCE_DESC&, std::uint32_t,
                                                     REFIID, void** out) -> HRESULT {
                handler_at = ++order;
                *out = reinterpret_cast<void*>(0x1331);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            xs::set_placed_at_va_pages_hook(hook);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0xadf0000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000010000ull);
            auto place = [&]() {
                placed = nullptr;
                return hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0, word(&resource_iid),
                                 word(&placed)));
            };
            const HRESULT buffer_hr = place();
            EXPECT(buffer_hr == S_OK && placed == reinterpret_cast<void*>(0x1331));
            EXPECT(hook_at == 1 && handler_at == 2 && hook_va == va && hook_bytes == 0xadf0000);

            hook_result = false;
            order = hook_at = handler_at = 0;
            const HRESULT refused_hr = place();
            EXPECT(refused_hr == S_OK && hook_at == 1 && handler_at == 2);
            hook_result = true;

            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            xbox_desc.Width = 64;
            xbox_desc.Height = 64;
            order = hook_at = handler_at = 0;
            const HRESULT texture_hr = place();
            EXPECT(texture_hr == S_OK && hook_at == 0 && handler_at == 1);

            xs::set_placed_at_va_pages_hook(nullptr);
            xs::set_placed_at_va_handler(nullptr);
        }

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots, 45/46/50/75 refused "
                "without the interface, residency arrays and fences unwrapped otherwise\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls45 = mock->calls[45], calls46 = mock->calls[46];
    const unsigned calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)); }) ==
           E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 46, kLong, word(many), word(priorities))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)); }) == E_OUTOFMEMORY);
    EXPECT(mock->calls[34] == calls34 && mock->calls[45] == calls45 && mock->calls[46] == calls46 &&
           mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the four "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Leonida's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator; every slot past it up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        for (unsigned slot = 9; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kLeonidaCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Leonida's tag are wrapped (Reset forwarded, tail refused, residency "
                "unwrapped, references balanced); others come back unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, latency, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(event, offset_us, pEvent, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(event, timeout_ms, pEvent, flags, &token): a fresh non-zero token each time; a null token
    // pointer is E_POINTER; the call never blocks (this test would hang otherwise).
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_OK && token2 != 0 && token2 != token);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, 0)) == E_POINTER);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 3);

    // 48 CreateCommandQueueX(desc, riid, out): the queue proxy, created through the PC slot 8 path.
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);
    EXPECT(mock->args[8][0] == word(&kQueueDesc));

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list));
    EXPECT(xs::command_list_proxy_real_list(list) &&
           reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list))->kind == Kind::command_list);
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    EXPECT(mock->args[12][4] == word(&list_iid));

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);

    EXPECT(xs::device_slot_overridden(48) && xs::device_slot_overridden(73) && xs::device_slot_overridden(78) &&
           xs::device_slot_overridden(79) && xs::device_slot_overridden(80));

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78/79/80 route to a queue, a list and frame-event tokens without reaching "
                "the mock's own PC slots\n", g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 8. Environment overrides for the Xbox device slots (one process each, read once) ----

int run_env_slots(bool valid) {
    const char* frame_interval = valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_LIST_X_SLOT", list_x) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    std::uint64_t token = 0;
    if (valid) {
        // Every route moved: the new slot is handled, the default slot is forwarded to the mock.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, 99, word(&kQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the five defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        EXPECT(hr_of(raw(device, 70)) == S_OK && mock->calls[70] == 1);  // not an Xbox slot: forwarded
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all five routes" : "outside 3..127 are ignored");
    return 0;
}

// ---- 9. The refused tail and the guard region ----

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // Signal (14) still reaches the real queue, with the real fence.
    EXPECT(hr_of(raw(queue, 14, word(fence), 5)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[14][1] == 5);
    for (unsigned slot = 19; slot < xs::queue_proxy_table_size(); ++slot)
        EXPECT(hr_of(raw(queue, slot)) == E_NOTIMPL && queue_mock->calls[slot] == 0);
    // 12 is served (check_fence_completion_address); the refused tail starts after it.
    for (unsigned slot = 13; slot < xs::fence_proxy_table_size(); ++slot) EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and refuse their tail as "
                    "before\n", m.flag, m.function, g_injected.load(), m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int check_console_command_list() {
    // 1. Slot mapping checks
    EXPECT(xs::console_to_pc_list_slot(14) == 14);   // Dispatch
    EXPECT(xs::console_to_pc_list_slot(26) == 26);   // ResourceBarrier
    EXPECT(xs::console_to_pc_list_slot(31) == 27);   // ExecuteBundle
    EXPECT(xs::console_to_pc_list_slot(32) == 28);   // SetDescriptorHeaps
    EXPECT(xs::console_to_pc_list_slot(33) == 29);   // SetComputeRootSignature
    EXPECT(xs::console_to_pc_list_slot(35) == 31);   // SetComputeRootDescriptorTable
    EXPECT(xs::console_to_pc_list_slot(60) == 58);   // EndEvent
    EXPECT(xs::console_to_pc_list_slot(61) == 59);   // ExecuteIndirect
    EXPECT(xs::console_to_pc_list_slot(85) == 16);   // CopyTextureRegion
    EXPECT(xs::console_to_pc_list_slot(146) == 80);  // GCL7 Barrier
    EXPECT(xs::console_to_pc_list_slot(148) == xs::kListNotForwarded); // AllocateUploadMemory

    Mock* mock_device = nullptr;
    void* device = make_device(mock_device, 11, true);
    EXPECT(device != nullptr);

    // 2. PC Slot 12 vs Xbox Slot 73
    // PC Slot 12 returns unwrapped list directly, so PC tests like tiny_frame can call slot 48 (ClearRenderTargetView)
    void* pc_list = nullptr;
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&list_iid), word(&pc_list))) == S_OK);
    EXPECT(pc_list != nullptr);
    EXPECT(!xs::is_command_list_proxy(pc_list));
    Mock* real_pc_list = reinterpret_cast<Mock*>(pc_list);
    EXPECT(real_pc_list->kind == Kind::command_list);
    // In PC ABI, slot 48 is ClearRenderTargetView
    raw(pc_list, 48, 0x1111, 0x2222, 0, 0);
    EXPECT(real_pc_list->calls[48] == 1);
    EXPECT(release(pc_list) == 0);

    // Xbox Slot 73 returns wrapped console command list proxy
    void* allocator = nullptr;
    const IID alloc_iid = __uuidof(ID3D12CommandAllocator);
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));

    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, {0, 0}};

    void* list_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list_proxy));
    EXPECT(xs::command_list_proxy_table_size() == 160);

    // 3. Inline cursor pinning: +0x18, +0x28, +0x30
    const auto base = reinterpret_cast<std::uintptr_t>(list_proxy);
    std::uint64_t c18 = 0, c28 = 0, c30 = 0;
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    std::memcpy(&c28, reinterpret_cast<const void*>(base + 0x28), sizeof(c28));
    std::memcpy(&c30, reinterpret_cast<const void*>(base + 0x30), sizeof(c30));
    const std::uint64_t expected_limit = static_cast<std::uint64_t>(base + 0x18 + 0x500);
    EXPECT(c18 == expected_limit);
    EXPECT(c28 == expected_limit);
    EXPECT(c30 == expected_limit);

    // 4. Console slot routing on ListProxy
    Mock* real_list = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list_proxy));
    EXPECT(real_list != nullptr && real_list->kind == Kind::command_list);

    // Console slot 32 (SetDescriptorHeaps) -> PC slot 28
    // Pass descriptor heap proxy to ensure UnwrappedArray unwrap_object works
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* heap_proxy = nullptr;
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    EXPECT(hr_of(raw(device, 14, word(&heap_desc), word(&heap_iid), word(&heap_proxy))) == S_OK);
    EXPECT(heap_proxy != nullptr && xs::is_heap_proxy(heap_proxy));
    ID3D12DescriptorHeap* real_heap = xs::heap_proxy_real_heap(heap_proxy);
    void* heap_ptrs[2] = {heap_proxy, real_heap};
    raw(list_proxy, 32, 2, word(heap_ptrs), 0, 0);
    EXPECT(real_list->calls[28] == 1);
    EXPECT(real_list->args[28][0] == 2);
    // The heap proxy should have been unwrapped in the array passed to PC slot 28:
    EXPECT(real_list->array[28][0] == word(real_heap));
    EXPECT(real_list->array[28][1] == word(real_heap));
    EXPECT(xs::list_slot_hits(32) == 1);
    EXPECT(xs::list_slot_first_call_seen(32));

    // Console slot 33 (SetComputeRootSignature) -> PC slot 29
    raw(list_proxy, 33, 0x5555, 0, 0, 0);
    EXPECT(real_list->calls[29] == 1);
    EXPECT(real_list->args[29][0] == 0x5555);
    EXPECT(xs::list_slot_hits(33) == 1);
    EXPECT(xs::list_slot_first_call_seen(33));

    // Console slot 60 (EndEvent) -> PC slot 58
    raw(list_proxy, 60, 0, 0, 0, 0);
    EXPECT(real_list->calls[58] == 1);

    // Console slot 61 (ExecuteIndirect) -> PC slot 59
    raw(list_proxy, 61, 0x10, 0x20, 0x30, 0x40);
    EXPECT(real_list->calls[59] == 1);
    EXPECT(real_list->args[59][0] == 0x10);

    // Console slot 148 (AllocateUploadMemory stub) -> returns 0 without faulting
    EXPECT(raw(list_proxy, 148, 2048, 512, 0, 0) == 0);
    EXPECT(xs::list_slot_hits(148) == 1);
    EXPECT(xs::list_slot_first_call_seen(148));

    // Console slot 146 (Barrier) on base 60-slot list returns E_NOTIMPL (PC slot 80 >= 60)
    EXPECT(hr_of(raw(list_proxy, 146, 0, 0, 0, 0)) == E_NOTIMPL);

    // Console slot 10 (Reset) unwraps allocator proxy and re-pins inline cursor
    std::uint64_t zero = 0;
    std::memcpy(reinterpret_cast<void*>(base + 0x18), &zero, sizeof(zero));
    EXPECT(hr_of(raw(list_proxy, 10, word(allocator), 0, 0, 0)) == S_OK);
    EXPECT(real_list->calls[10] == 1);
    EXPECT(real_list->args[10][0] == word(real_allocator));
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    EXPECT(c18 == expected_limit);

    // Console slot 7 (GetDevice) returns device proxy
    void* list_dev = nullptr;
    const IID dev_iid = __uuidof(ID3D12Device);
    const auto dev_refs_before = xs::device_proxy_ref_count(device);
    EXPECT(hr_of(raw(list_proxy, 7, word(&dev_iid), word(&list_dev))) == S_OK);
    EXPECT(list_dev == device);
    EXPECT(xs::device_proxy_ref_count(device) == dev_refs_before + 1);
    EXPECT(release(list_dev) == dev_refs_before);

    // Console slot 0 (QueryInterface)
    void* list_qi = nullptr;
    EXPECT(hr_of(raw(list_proxy, 0, word(&list_iid), word(&list_qi))) == S_OK);
    EXPECT(list_qi == list_proxy);
    EXPECT(release(list_qi) == 1);

    // 5. Queue slot 10 (ExecuteCommandLists) unwraps ListProxy in array
    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    const IID queue_iid = __uuidof(ID3D12CommandQueue);
    EXPECT(hr_of(raw(device, 8, word(&qdesc), word(&queue_iid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue));
    Mock* real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));

    Mock* unwrapped_list = new_mock(Kind::command_list);
    void* lists_to_exec[2] = {unwrapped_list, list_proxy};
    raw(queue, 10, 2, word(lists_to_exec), 0, 0);
    EXPECT(real_queue->calls[10] == 1);
    EXPECT(real_queue->args[10][0] == 2);
    EXPECT(real_queue->array[10][0] == word(unwrapped_list));
    EXPECT(real_queue->array[10][1] == word(real_list));

    // 6. Test with list_level = 7 (supporting ID3D12GraphicsCommandList7, 81 slots)
    mock_device->list_level = 7;
    void* list7_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list7_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list7_proxy));
    Mock* real_list7 = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list7_proxy));
    EXPECT(real_list7 != nullptr);
    // Console slot 146 -> PC slot 80 (since 80 < 81 slots)
    raw(list7_proxy, 146, 0x9999, 0, 0, 0);
    EXPECT(real_list7->calls[80] == 1);
    EXPECT(real_list7->args[80][0] == 0x9999);
    EXPECT(release(list7_proxy) == 0 && real_list7->refs == 0);

    // Clean up
    EXPECT(release(list_proxy) == 0 && real_list->refs == 0);
    EXPECT(release(heap_proxy) == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0 && real_queue->refs == 0);
    EXPECT(release(device) == 0 && mock_device->refs == 0);

    // 7. Verify stats reset
    EXPECT(xs::list_slot_hits(32) > 0);
    xs::reset_device_proxy_stats();
    EXPECT(xs::list_slot_hits(32) == 0);
    EXPECT(!xs::list_slot_first_call_seen(32));

    std::printf("%s: console command list routing, cursor pinning, queue unwrapping, and slot mapping passed\n", g_label);
    return 0;
}

// ---- Console resource formats and flags (format 118 and the flag bits above the PC's) ----

D3D12_RESOURCE_DESC texture2d(UINT64 width, UINT height, DXGI_FORMAT format, std::uint32_t flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return d;
}

int check_console_formats() {
    g_label = "xs_proxy_mock console formats";
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    constexpr auto kD16S8 = static_cast<DXGI_FORMAT>(118), kR16X8 = static_cast<DXGI_FORMAT>(119),
                   kX16G8 = static_cast<DXGI_FORMAT>(120);
    // TuxBox's recorded stop: a 2560x1440 depth texture of format 118 with flags 0x80300006 (depth, UAV, console bits).
    const D3D12_RESOURCE_DESC depth = texture2d(2560, 1440, kD16S8, 0x80300006u);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 65536;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x80000004u);   // UAV and a console bit
    const auto pc_depth = [](const D3D12_RESOURCE_DESC& d) {
        return d.Format == DXGI_FORMAT_R24G8_TYPELESS && static_cast<std::uint32_t>(d.Flags) == 0x2 &&
               d.Width == 2560 && d.Height == 1440;
    };

    // 25 GetResourceAllocationInfo, PC order and with the return buffer first: the PC device gets PC descriptions.
    const D3D12_RESOURCE_DESC both[2] = {depth, buffer};
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    raw(device, 25, word(&info), 0, 2, word(both));
    EXPECT(mock->calls[25] == 1 && mock->descs_seen == 2 && pc_depth(mock->desc_seen[0]));
    EXPECT(mock->desc_seen[1].Format == DXGI_FORMAT_UNKNOWN && static_cast<std::uint32_t>(mock->desc_seen[1].Flags) == 0x4);
    std::memset(mock->desc_seen, 0, sizeof(mock->desc_seen));
    void* const* const vtable = *reinterpret_cast<void* const* const*>(device);
    reinterpret_cast<SlotFn>(vtable[25])(&info, word(device), 0, 1, word(&depth), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT(mock->calls[25] == 2 && mock->descs_seen == 1 && pc_depth(mock->desc_seen[0]));
    EXPECT(depth.Format == kD16S8 && static_cast<std::uint32_t>(depth.Flags) == 0x80300006u);   // the title's own

    // 27 CreateCommittedResource, 29 CreatePlacedResource, 30 CreateReservedResource: the description and the clear
    // value (format 118 -> D24_UNORM_S8_UINT, its depth and stencil kept).
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kD16S8;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 3;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto pc_clear = [&] {
        return mock->clear_given && mock->clear_seen.Format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
               mock->clear_seen.DepthStencil.Depth == 1.0f && mock->clear_seen.DepthStencil.Stencil == 3;
    };
    raw(device, 27, word(&heap), 0, word(&depth), 0, word(&clear));
    EXPECT(mock->calls[27] == 1 && pc_depth(mock->desc_seen[0]) && pc_clear());
    raw(device, 29, 0x1234, 0, word(&depth), 0, word(&clear));
    EXPECT(mock->calls[29] == 1 && pc_depth(mock->desc_seen[0]) && pc_clear());
    raw(device, 30, word(&depth), 0, word(&clear));
    EXPECT(mock->calls[30] == 1 && pc_depth(mock->desc_seen[0]) && pc_clear());
    raw(device, 27, word(&heap), 0, word(&depth), 0, 0);
    EXPECT(mock->calls[27] == 2 && !mock->clear_given);   // no clear value stays none
    // 76, the title's committed resource: the same, after its layout word.
    {
        D3D12_RESOURCE_DESC tiled = depth;
        tiled.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
        void* made = nullptr;
        const IID iid = kResourceIid;
        reinterpret_cast<SlotFn>(vtable[76])(device, word(&heap), 0, word(&tiled), 0, word(&clear), word(&iid),
                                             word(&made), 0, 0, 0, 0, 0, 0, 0);
        EXPECT(mock->calls[27] == 3 && pc_depth(mock->desc_seen[0]) &&
               mock->desc_seen[0].Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && pc_clear());
    }
    // An ordinary PC description goes through unchanged.
    const D3D12_RESOURCE_DESC colour = texture2d(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 0x5);   // render target and UAV
    raw(device, 27, word(&heap), 0, word(&colour), 0, 0);
    EXPECT(std::memcmp(&mock->desc_seen[0], &colour, sizeof(colour)) == 0);

    // 18 CreateShaderResourceView: 118 and 119 view the depth plane, 120 the stencil plane; other formats as they are.
    struct { DXGI_FORMAT in, out; } const views[] = {{kD16S8, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
                                                     {kR16X8, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
                                                     {kX16G8, DXGI_FORMAT_X24_TYPELESS_G8_UINT},
                                                     {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}};
    for (const auto& v : views) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = v.in;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        raw(device, 18, 0x5000, word(&srv), 0x6000);
        EXPECT(mock->srv_given && mock->srv_seen.Format == v.out && mock->srv_seen.Texture2D.MipLevels == 1);
    }
    raw(device, 18, 0x5000, 0, 0x6000);
    EXPECT(!mock->srv_given);

    // 21 CreateDepthStencilView: 118 -> D24_UNORM_S8_UINT; no description on a typeless D24 texture (what a format-118
    // texture became) -> a typed view over the whole resource; no description on anything else stays none.
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kD16S8;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    raw(device, 21, 0x5000, word(&dsv), 0x6000);
    EXPECT(mock->dsv_given && mock->dsv_seen.Format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
           mock->dsv_seen.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D);
    Mock* resource = new_mock(Kind::resource);
    resource->resource_desc = texture2d(256, 256, DXGI_FORMAT_R24G8_TYPELESS, 0x2);
    raw(device, 21, word(resource), 0, 0x6000);
    EXPECT(resource->calls[10] == 1 && mock->dsv_given && mock->dsv_seen.Format == DXGI_FORMAT_D24_UNORM_S8_UINT &&
           mock->dsv_seen.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D && mock->dsv_seen.Texture2D.MipSlice == 0);
    resource->resource_desc.DepthOrArraySize = 6;
    raw(device, 21, word(resource), 0, 0x6000);
    EXPECT(mock->dsv_seen.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY &&
           mock->dsv_seen.Texture2DArray.ArraySize == 6);
    resource->resource_desc.DepthOrArraySize = 1;
    resource->resource_desc.SampleDesc.Count = 4;
    raw(device, 21, word(resource), 0, 0x6000);
    EXPECT(mock->dsv_seen.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS);
    resource->resource_desc = texture2d(256, 256, DXGI_FORMAT_D32_FLOAT, 0x2);
    raw(device, 21, word(resource), 0, 0x6000);
    EXPECT(!mock->dsv_given);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: passed\n", g_label);
    g_label = "xs_proxy_mock";
    return 0;
}

// ---- Fence slot 12: the completed value at a stable address, kept current ----

// True when *word reads `want` within two seconds (the watcher thread's safety poll is 16 ms).
bool word_reaches(const volatile std::uint64_t* word, std::uint64_t want) {
    for (int i = 0; i < 2000; ++i) {
        if (*word == want) return true;
        Sleep(1);
    }
    return *word == want;
}

int check_fence_completion_address() {
    g_label = "xs_proxy_mock fence slot 12";
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        void* fence = make_fence(device);
        EXPECT(device && fence);
        Mock* const real = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));

        // The word holds GetCompletedValue's answer, on a cache line of its own, and every call returns it.
        const auto* word = reinterpret_cast<const volatile std::uint64_t*>(raw(fence, 12));
        EXPECT(word && reinterpret_cast<std::uintptr_t>(word) % 64 == 0 && *word == kCompletedValue);
        EXPECT(reinterpret_cast<const volatile std::uint64_t*>(raw(fence, 12)) == word);

        // The GPU completes more work: the watcher thread stores the new value without a call from the title.
        real->completed.store(42);
        EXPECT(word_reaches(word, 42));
        // A CPU Signal through the proxy shows at once, as does what GetCompletedValue reads.
        real->completed.store(55);
        EXPECT(hr_of(raw(fence, 10, 55)) == S_OK && *word == 55);
        real->completed.store(56);
        EXPECT(raw(fence, 8) == 56 && *word == 56);

        // Past it the table is still refused.
        EXPECT(hr_of(raw(fence, 13)) == E_NOTIMPL);

        // Released, the fence is no longer read; the word stays readable and keeps its last value.
        EXPECT(release(fence) == 0 && release(device) == 0 && mock->refs == 0 && real->refs == 0);
        real->completed.store(99);
        Sleep(50);
        EXPECT(*word == 56);
    }
    std::printf("%s: passed\n", g_label);
    g_label = "xs_proxy_mock";
    return 0;
}

// ---- The console queue layout (QueueLayout::console): Signal 16 ... ExecuteCommandListsX 35 ----

int check_console_queue_layout() {
    g_label = "xs_proxy_mock console queue layout";
    Mock* mock = new_mock(Kind::device);
    mock->device_level = 9;
    mock->fence1 = true;
    mock->allocator_tag = true;
    mock->refs = 1;
    void* device = nullptr;
    EXPECT(SUCCEEDED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device,
                                             xs::QueueLayout::console)));
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence && xs::queue_proxy_layout(queue) == xs::QueueLayout::console);
    EXPECT(xs::queue_proxy_layout(fence) == xs::QueueLayout::pc &&
           xs::queue_proxy_layout(nullptr) == xs::QueueLayout::pc);
    Mock* const q = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    const std::uint64_t real_fence = word(xs::fence_proxy_real_fence(fence));

    // 16 Signal, 17 Wait and 25 SignalX reach the PC Signal (14) and Wait (15) with the real fence; SignalX takes a
    // flush word of 0 or 0x08000000 only.
    EXPECT(hr_of(raw(queue, 16, word(fence), 5)) == S_OK && q->calls[14] == 1 && q->args[14][0] == real_fence &&
           q->args[14][1] == 5);
    EXPECT(hr_of(raw(queue, 17, word(fence), 6)) == S_OK && q->calls[15] == 1 && q->args[15][0] == real_fence &&
           q->args[15][1] == 6);
    EXPECT(hr_of(raw(queue, 25, word(fence), 7, 0)) == S_OK && q->calls[14] == 2 && q->args[14][1] == 7);
    EXPECT(hr_of(raw(queue, 25, word(fence), 8, 0x08000000)) == S_OK && q->calls[14] == 3 && q->args[14][1] == 8);
    EXPECT(hr_of(raw(queue, 25, word(fence), 9, 1)) == E_NOTIMPL && q->calls[14] == 3);
    // Without a fence proxy first they are refused before the real queue sees them.
    std::uint64_t frequency = 0;
    EXPECT(hr_of(raw(queue, 16, word(&frequency))) == E_INVALIDARG && q->calls[14] == 3 && q->calls[16] == 0);
    EXPECT(hr_of(raw(queue, 17, 0, 1)) == E_INVALIDARG && q->calls[15] == 1);
    EXPECT(hr_of(raw(queue, 25, 0, 1, 0)) == E_INVALIDARG && q->calls[14] == 3);

    // 18 and 19 are GetTimestampFrequency (16) and GetClockCalibration (17), with the caller's pointers.
    std::uint64_t gpu = 0, cpu = 0;
    EXPECT(hr_of(raw(queue, 18, word(&frequency))) == S_OK && q->calls[16] == 1 && q->args[16][0] == word(&frequency));
    EXPECT(hr_of(raw(queue, 19, word(&gpu), word(&cpu))) == S_OK && q->calls[17] == 1 &&
           q->args[17][0] == word(&gpu) && q->args[17][1] == word(&cpu));

    // 20 is GetDesc (18): the buffer goes to the PC method as its hidden pointer and comes back in RAX, whichever
    // order the title passes the buffer and `this` in, a buffer holding a stale copy of the vtable pointer included.
    void* const* const vtable = *reinterpret_cast<void* const* const*>(queue);
    const auto slot20 = reinterpret_cast<SlotFn>(vtable[20]);
    D3D12_COMMAND_QUEUE_DESC desc{};
    EXPECT(raw(queue, 20, word(&desc)) == word(&desc) && q->calls[18] == 1 && q->args[18][0] == word(&desc));
    EXPECT(slot20(&desc, word(queue), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == word(&desc) && q->calls[18] == 2 &&
           q->args[18][0] == word(&desc));
    D3D12_COMMAND_QUEUE_DESC stale{};
    std::memcpy(&stale, &vtable, sizeof(vtable));   // the first 8 bytes hold the queue proxy's vtable pointer
    EXPECT(slot20(&stale, word(queue), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == word(&stale) &&
           q->calls[18] == 3 && q->args[18][0] == word(&stale));
    EXPECT(raw(queue, 20, word(&stale)) == word(&stale) && q->calls[18] == 4 && q->args[18][0] == word(&stale));

    // 23 SuspendX and 24 ResumeX: there is nothing to suspend; S_OK, and the real queue is not called.
    EXPECT(hr_of(raw(queue, 23)) == S_OK && hr_of(raw(queue, 24)) == S_OK);

    // 35 ExecuteCommandListsX(count, lists, frame token, flags): ExecuteCommandLists (10) with every proxy in the
    // array replaced by its real object; flags past bits 0-1 submit nothing.
    // Any proxy in the array is unwrapped and any other object goes as it is. Every entry is read to tell which, as
    // for slot 10, so the second one is a real address that is no proxy (a list the title made elsewhere).
    std::uint64_t not_a_proxy = 0;
    const std::uint64_t lists[2] = {word(fence), word(&not_a_proxy)};
    EXPECT(raw(queue, 35, 2, word(lists), 0x1111, 0) == 0 && q->calls[10] == 1 &&
           static_cast<std::uint32_t>(q->args[10][0]) == 2 && q->array[10][0] == real_fence &&
           q->array[10][1] == word(&not_a_proxy));
    EXPECT(raw(queue, 35, 2, word(lists), 0x1112, 3) == 0 && q->calls[10] == 2);
    EXPECT(raw(queue, 35, 2, word(lists), 0x1113, 4) == 0 && q->calls[10] == 2);
    // 10 itself is the PC's ExecuteCommandLists, as in the PC layout.
    EXPECT(raw(queue, 10, 1, word(lists)) == 0 && q->calls[10] == 3 && q->array[10][0] == real_fence);

    // 14 and 15 with a fence proxy are a PC-style caller's Signal and Wait; anything else there is a console slot
    // that is not modelled.
    EXPECT(hr_of(raw(queue, 14, word(fence), 11)) == S_OK && q->calls[14] == 4 && q->args[14][1] == 11);
    EXPECT(hr_of(raw(queue, 15, word(fence), 12)) == S_OK && q->calls[15] == 2 && q->args[15][1] == 12);
    EXPECT(hr_of(raw(queue, 14, 0x1000)) == E_NOTIMPL && hr_of(raw(queue, 15)) == E_NOTIMPL &&
           q->calls[14] == 4 && q->calls[15] == 2);

    // 31 (page mappings), 32 (PresentX, no presenter yet) and the slots around them: refused, nothing forwarded.
    for (const unsigned slot : {21u, 22u, 26u, 30u, 31u, 32u, 33u, 34u, 36u, 127u})
        EXPECT(hr_of(raw(queue, slot)) == E_NOTIMPL);
    for (unsigned slot = 19; slot < kMockSlots; ++slot) EXPECT(q->calls[slot] == 0);
    EXPECT(xs::queue_table_overruns() == 0);

    // A PC-layout device next to it is unchanged: 16 is GetTimestampFrequency there.
    Mock* pc_mock = nullptr;
    void* pc_device = make_device(pc_mock, 9, true);
    void* pc_queue = make_queue(pc_device);
    EXPECT(pc_queue && xs::queue_proxy_layout(pc_queue) == xs::QueueLayout::pc);
    Mock* const pq = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(pc_queue));
    EXPECT(hr_of(raw(pc_queue, 16, word(&frequency))) == S_OK && pq->calls[16] == 1 && pq->calls[14] == 0);
    EXPECT(hr_of(raw(pc_queue, 20, word(&desc))) == E_NOTIMPL && pq->calls[18] == 0);

    EXPECT(release(pc_queue) == 0 && release(pc_device) == 0 && pc_mock->refs == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    std::printf("%s: passed\n", g_label);
    g_label = "xs_proxy_mock";
    return 0;
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_tail(true) || check_levels() || check_fence1() || check_create_failures() || check_open_shared() ||
        check_alloc_failures() || check_allocators() || check_xbox_slots() || check_console_command_list() ||
        check_console_queue_layout() || check_fence_completion_address() || check_console_formats())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
