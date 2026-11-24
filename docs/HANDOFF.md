# Handoff: the state of this fork on 2026-11-24

This page is for whoever picks the work up on a local machine. It says what the branch holds, what is measured and
what is not, what is left in priority order, and how to build and test on Windows. Nothing here quotes the game:
only slot numbers, call shapes, sizes and the runtime's own behaviour.

- Branch: `claude/nice-lamport-fc7qk7`, pull request #2 of simomistralii/Leonida-PC.
- Base: ItsNotRexed/Leonida up to `de4c373`, with the `sync-86a7807-repairs` branch merged. That repository stopped
  answering on 2026-11-23 and PortWatch VI reports it deleted ([related_projects.md](related_projects.md)), so this
  fork carries on alone; on the owner's instruction its continuation (ubiquitarianism/leonida) is not followed.
- Last head with every workflow green: `ecde91edd`. Everything after it is described under *Done* and *CI* below.

## 1. Progress, measured the way PortWatch VI measures it

PortWatch VI (portwatch-vi.xyz) scores each port from milestones its maintainers assess from each project's own
reports: Exe runs 25%, First frame 40%, Main menu 55%, In-game 70%, 30 FPS 85%, Stable 100%. Below Exe runs a
project earns up to 25% for groundwork (loader 0.2, shader translation 0.28, graphics layer 0.32, proven on another
game 0.2); past it, each milestone reached sets the score to its threshold and a partial one interpolates toward
the next. On 2026-11-24 the leader, VIctory, is at 67% (menu reached, in-game 0.8), XCL near 62% (in-game 0.5) and
TuxBox at 40% (first frame). The original Leonida was recorded at Exe runs.

**This fork has not been run on the game in this session**: the cloud container has no game, no GPU and no Windows.
Every score point above 25% needs a run of GTA VI on a real PC with the player's own copy. By this repository's own
boot notes the next stop on such a run is boot-chain stop 5 (DirectStorage, section 4), so an honest estimate today
is **Exe runs, 25%**, with the groundwork in good shape: the translator suite, the runtime's CPU tests and the GPU
tests on llvmpipe and MoltenVK pass, and a full translation of one copy's shaders (26,503 of 26,503) is recorded in
[retail_boot_intel_host.md](retail_boot_intel_host.md). The fastest way up is a loop on the local machine: run,
read the first `[rt]` stop in the log, fix it with a test, run again ([boot.md](boot.md) is the runbook).

## 2. Done on this branch

Kept in sync with upstream until it stopped answering, then:

- **CI made green on every platform:**
  - Windows cross-drive paths and the `gta6rt.exe` launcher name in the Python tests;
  - SPIRV-Tools optional off Linux, required where `LEONIDA_REQUIRE_SPIRV_TOOLS=1`;
  - tests that only passed as root;
  - the `loader_test` hang: Windows' Bad Image dialog, now suppressed, and every Windows ctest has a 300 s limit;
  - `root_sig`, whose fail-closed chunk walk a merge had dropped (`540912523` restores it);
  - `crt_timed_wait`, a timed wait that reported a notification when it had run out.
- **Translator:** every wave that sends `MSG_GS_ALLOC_REQ` stores the mesh counts, and an image load with tfe or
  lwe writes 0 to its fetch-status VGPR (rows in [design.md](design.md)); the tests upstream left failing pass.
- **D3D12 boundary** (`runtime/shims/d3d12_xs_device.cpp`):
  - device slots 25/26 in either argument order;
  - descriptor heap type 4, and slot 14's console tail taken only when its size matches the count;
  - the title's queues on the console queue layout (Signal 16 to ExecuteCommandListsX 35, `LEONIDA_QUEUE_LAYOUT`);
  - fence slot 12, the completed value's address, kept current by a watcher thread;
  - console format 118 and console resource flags turned into PC ones on slots 18, 21, 25, 27, 29, 30, 76 and 133.
- **GDK runtime:**
  - IXPackageImpl4;
  - text entry ending as cancelled (`E_ABORT`);
  - the registry's `ConsoleMode`, so the title knows it is on a Series X;
  - GameInput by ordinal 1;
  - the AcpHal message protocol, command acknowledgements and notification words.
- **Documentation:** the related projects and what each records past stop 13
  ([related_projects_gaps.md](related_projects_gaps.md)).

## 3. CI at handoff, and how to reproduce

- The last pushed head before this page, `4121f9731`, fixes the only failure of the previous head: `xs_proxy_mock`
  crashed (SEGFAULT) because its ExecuteCommandListsX check passed `0x1234` in a list array that the proxy reads
  entry by entry. At handoff its No Game Data, jactionlint and XMA2 decoder runs had passed and its CI and Runtime
  runs were still going; check the Runtime windows job first.
- To run one runtime test on Windows:

  ```powershell
  ctest --test-dir runtime/build -C RelWithDebInfo -R xs_proxy_mock --output-on-failure
  ```

- The Windows runner has no GPU, so every `d3d12_*` test that needs a device exits 77 and is skipped there; they
  only really run on a local machine with a GPU. The console queue layout, fence slot 12 and format 118 work is
  therefore checked by mocks (`runtime/test/xs_proxy_mock_test.cpp`) and not yet on hardware.
- Locally on Linux, Ubuntu's SPIRV-Tools 2025.1rc `spirv-opt` crashes in `test_int32_to_f64`; that is the tool, not
  the translator, and CI's SPIRV-Tools pass it.

## 4. Remaining work, in priority order

1. **DirectStorage, boot-chain stop 5** (section 5). Without it the title asserts on a null factory right after
   the steps this fork already covers, so nothing past Exe runs can be measured.
2. **Run the title locally** with the shim and fix each stop in order ([retail_boot_chain.md](retail_boot_chain.md)
   records stops 1-13, [retail_boot_intel_host.md](retail_boot_intel_host.md) the later ones on another stack).
3. **The console device slot map:** from slot 44 the console device is not the PC's. Console 83-106 are the PC's
   44-67 and 108-112 the PC's 68-72; 57 and 67 deserialize console pipelines, 60 is the graphics pipeline, 61 a
   sampler from 21 dwords, 69 a driver hint, 70 and 71 views without a resource, 74 and 75 register and unregister a
   page pool, 86 the pipeline stream, 115 a state object, 127 a committed resource with a layout
   ([related_projects_gaps.md](related_projects_gaps.md), past stop 13, row 7). Several reach PC methods of the same
   number today (57, 69 and 74; 75 is taken for `CreateCommandQueue1`). Give the title's device a console map, as
   `QueueLayout` does for queues, and keep the routes the proxy already has (48, 62, 67, 73, 76, 78-80 and 133).
4. **Pipelines:** console stream tags (amplification 23, mesh 24, a 32-byte stencil record 26), rectangle lists
   (topology 0x11, type 0x13) and the depth format 118 inside pipeline descriptions (row 8).
5. **Frame pacing and PresentX:** device slots 78-81 on one pacer, queue slot 32 routed to `xbox_present.cpp`, the
   title's own window adopted (rows 3 and 5).
6. **Command-list console slots:** 27-31, 56-58, 71-74, 83, 85 and 139 (row 6, and this repository's own table in
   [d3d12_command_objects.md](d3d12_command_objects.md)).
7. **Audio:** `xapu.dll` (Opus) and `chad.dll` (convolution) connect at startup; a failed XDsp connect ends at a
   title breakpoint (row 9).
8. **XG texture layouts:** `XGCreateTextureComputer` and its slots answer `E_NOTIMPL` (VIctory row 1).
9. **xmem placement and hooks:** the attribute word's address range, `XMemVirtualQuery`'s extended fields, the
   log2 alignment in bits 24-28 and `__XMemAllocationHooks__` (VIctory row 5).
10. **Smaller:** CRT opens with console paths, the main module's identity, `XLaunchNewGame` relaunching, slot 48's
    queue description read as console fields, the ApuAlloc cached bit. `D3D12ConfigureVirtualMemoryX`'s outputs are
    low priority: the title only tests its result.

## 5. The DirectStorage shim (`xcrdapi.dll`)

**Why:** the title imports `DStorageGetFactory` from `xcrdapi.dll`. This fork has no shim for it, so the import
binds to the generic thunk, which answers an error `HRESULT` (`runtime/shims/registry.cpp` classifies it), and the
title asserts on the null factory: stop 5 of [retail_boot_chain.md](retail_boot_chain.md). That measurement got
past it with upstream's ItsNotRexed/Leonida#4348 merged locally; that pull request never reached `main`, its
content was not inspected here, and it went with the repository.

**Facts gathered so far** (other projects' records; facts only, the code must be this repository's own):

- Factory: slots 4 OpenFile, 5 CreateStatusArray, 8 SetStagingBufferSize and 10 CreateQueue. The title asks for a
  32 MiB staging buffer and stops if that fails (VIctory).
- Queue: slots 3 EnqueueRequest, 4 EnqueueStatus, 5 EnqueueSignal, 6 Submit, 7 CancelRequestsWithTag, 8 Close,
  9 GetErrorEvent, 10 RetrieveErrorRecord (a 0x290-byte record) and 11 Query (a 0x1C-byte result). The title ends
  its queues with Close, not Release (VIctory, G6Recomp).
- Queue description: 0x18 bytes: the source type at +0, the capacity (16 bits) at +8, the priority (8 bits) at +0xA
  and the name at +0x10. Factory slot 10 takes a 0x20-byte one, whose extra word the title sets to 1.
- Request: 0x48 bytes, with the options at +0, the destination at +8, its size at +0x18, the source at +0x20, the
  file offset at +0x28, the source size at +0x30, an intermediate size at +0x34, the cancellation tag at +0x38 and
  the name at +0x40. Option bits: 0 zlib (RFC 1950), 1-4 the BC format of BCPack data (0 for none), 5-20 swizzle,
  21 the destination is a page array, 22 the source is memory, 23 the source is pages.
- Page destinations: an array of 64 KiB page numbers; the start inside the first page is in the low 16 bits
  (VIctory; check which word holds them before relying on it).
- Status array: slot 3 IsComplete, slot 4 GetHResult. Entries start as `E_PENDING`. An entry is written once every
  request enqueued before it on its queue has finished, with the first failure since the previous entry, or
  success. The title polls entry 0 in a `Sleep(1)` loop, so a request that never completes is a hang, not a crash
  (VIctory, G6Recomp).
- File: slot 7 is GetId (G6Recomp). Destinations must be committed before the read.
- Errors: a source that ends early answers `HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)`, a bad compressed stream
  `HRESULT_FROM_WIN32(ERROR_INVALID_DATA)` (VIctory).

The layouts above come from G6Recomp, TuxBox and OpenScarlett, and VIctory agrees on the slots it uses. Before
writing the shim, check each offset in at least two of them: VIctory's `crates/victory-hle/src/modules/dstorage.rs`
(GPL-3.0-or-later), TuxBox's xcrdapi crate (Apache-2.0), OpenScarlett's DirectStorage module (MIT) and G6Recomp's
DirectStorage header with its `docs/io.md` (GPL-3.0). Take facts only ([related_projects.md](related_projects.md),
*The line*).

**Plan:**

- A new shim file, `xcrdapi.cpp` in `runtime/shims` (not written yet), registered as `xcrdapi.dll!DStorageGetFactory`.
- Open files through the file system shim's own `CreateFileW` (`rt::shims::find("kernel32.dll", "CreateFileW")`),
  so `GAME:`, `G:` and `T:` paths, the overlay and the `..` refusal behave as for every other title open
  (`runtime/shims/file_system.cpp`).
- Inflate zlib with this repository's `leonida::zlib_inflate` (`runtime/src/shader_library.h`). Answer BCPack with
  an error that names it until a runtime decoder exists.
- Write page destinations through a new helper in `runtime/shims/xmem.h` that writes 64 KiB pages by page number.
- Run requests on one worker thread, write status entries in queue order, and signal fences through the fence
  proxy's own Signal.
- Wire it like `acphal.cpp`: the `gta6rt` sources and a test target in `runtime/CMakeLists.txt`, its `add_test`,
  the file list in `scripts/build_runtime.ps1`, and `xcrdapi.dll` in `classify_import`
  (`runtime/shims/registry.cpp`).
- Test it with synthetic files only: a read, an early end, a zlib stream, status order across two queues, Close
  waiting for running reads.

## 6. Console depth format 118

Done at the Xbox boundary in `dfbca8517` and checked by mocks only. The fork drops console flag bits above the
PC's, makes 118-120 textures `R24G8_TYPELESS` without unordered access on depth, and gives clear values and
depth-stencil views `D24_UNORM_S8_UINT` and shader views the R24 and X24 plane formats
([d3d12_vulkan.md](d3d12_vulkan.md)). Still open:

- the depth-stencil format inside graphics pipeline descriptions and streams;
- B4G4R4A4 render targets, which another port backs with RGBA8;
- sizing the console texture layouts 0x100-0x109 from their own layout instead of the PC's;
- an unordered-access view on a depth texture, which now fails since that flag is dropped; no run has shown
  whether the title makes one;
- a check on hardware: TuxBox's first run stopped on device slot 25 for a 2560x1440 texture of this format with
  flags 0x80300006.

## 7. Building and testing on Windows

From the repository root, in PowerShell (the same commands as [AGENTS.md](../AGENTS.md)):

```powershell
python -m compileall -q make_game.py tools tests
python -I make_game.py --help
python -I scripts/run_semantic_tests.py tests/semantic
cmake -S runtime -B runtime/build
cmake --build runtime/build --config RelWithDebInfo
ctest --test-dir runtime/build -C RelWithDebInfo --output-on-failure --timeout 300
```

A run against the player's own copy, logging every D3D12 call ([boot.md](boot.md) has the full runbook):

```powershell
$env:LEONIDA_D3D12 = 'vulkan'
.\runtime\build\gta6rt.exe --run $exe --game-dir $pkg --trace $trace --trace-d3d12 2>&1 |
    Tee-Object -FilePath $log
```

Keep the log on the local machine: a log that quotes the executable is game data, and none of it may be committed
([AGENTS.md](../AGENTS.md), hard rules). Report stops the way the boot notes do, with slot numbers, call shapes and
counts only.

## 8. Where the comparisons are

- [related_projects.md](related_projects.md): what each related project is, its licence, and what must not be
  taken from it.
- [related_projects_gaps.md](related_projects_gaps.md): the VIctory rows, xcl's facts, and the twelve rows past
  stop 13 from TuxBox, OpenScarlett and G6Recomp, each marked done in this fork or still open.
- [xcl_compare.md](xcl_compare.md): the run against XCL's test title.
