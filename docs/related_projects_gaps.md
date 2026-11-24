# What the related ports do that this runtime does not yet

**Status: a reading of other projects' source, 2026-11-24. Nothing here is measured in this repository yet.** Each
row is a fact another project records about what the title asks of a console API, the file it is recorded in, and
the place in this runtime that does something else. A row is a lead: check it with a test of your own, a measured
trace or a public specification before relying on it. The licence boundary for each project is in
[related_projects.md](related_projects.md); in short, facts only, never code.

Read at VIctory `97c78d1` (GPL-3.0-or-later). In VIctory's column, a path is under
`crates/victory-hle/src/modules/` unless it says otherwise.

## The five most likely next stops of the retail boot

Ordered by where they would fall after stop 13 of [retail_boot_chain.md](retail_boot_chain.md): by then the title
has created its device, queues, command lists, one placed resource and its first descriptor heap. The order is a
hypothesis.

| # | Area | What VIctory records | This runtime |
|---|---|---|---|
| 1 | Texture layouts (`xg.dll`) | `XGSuggestSwizzleMode` and `XGCreateTextureComputer` describe the resource with a `D3D12_RESOURCE_DESC` whose layout field holds `0x100 \| mode`. The computer fills a 0x1b40-byte layout: 15 mips of 0x70 bytes, 4 planes; the title reads the mip offsets at 0x00, 0x08, 0x10 and 0x1c. Slot 6 returns a 64-bit element offset that the title reads texels through (`xg.rs`) | Both exports and every computer slot from 2 up return `E_NOTIMPL` and write nothing, so slot 6 hands back 0x80004001 where an offset is read (`runtime/shims/xg_xs.cpp`) |
| 2 | DirectStorage (`xcrdapi.dll`) | Factory slots 4 (OpenFile), 5 (CreateStatusArray), 8 (SetStagingBufferSize: the title asks for 32 MiB and stops if it fails) and 10 (CreateQueue). A request is 0x48 bytes. The title polls status entry 0 in a `Sleep(1)` loop, so a request that never completes is a hang, not a crash. It closes queues with Close, not Release (`dstorage.rs`) | No `xcrdapi.dll` shim; stop 5 of the boot chain was cleared by an open upstream pull request (#4348) |
| 3 | XPackage interface id | The title asks `QueryApiImpl` for XPackageImpl as IXPackageImpl4 (`xpackage.rs`). Its chunk manager calls chunk availability, install, uninstall and enumeration | **Id done in this fork:** IXPackageImpl4 is accepted, with its one extra slot (41, `XPackageGetPackageKind`) answering `E_NOTIMPL` as the layout in `tools/gdk_runtime_classes.json` gives it (`runtime/shims/xgameruntime.cpp`). Still open: the chunk-manager slots answer `E_NOTIMPL` |
| 4 | Audio coprocessor | `AcpHal.dll` answers a command that carries an id with `COMMAND_COMPLETED` and sets the notification word to 1. The SHAPE context arrays come from the APU heap so their device addresses map back to them; the title uses only XMA contexts, and an XMA context is 16 words (`audio.rs`). `xapu.dll` and `chad.dll` are implemented (`apu.rs`, `dsp.rs`) | `runtime/shims/acphal.cpp` accepts commands and drops them, writes no notification word, and gives the context arrays synthetic APU addresses in the range its first `ApuAlloc` blocks also use. No `xapu.dll` or `chad.dll` shim; [audio_hal_exe_calls.md](audio_hal_exe_calls.md) records that the title reads 0 from `XDspConnect` as success |
| 5 | Title memory placement (`xmem.dll`) | The attribute word picks the address range: one flag bit places memory below 4 GiB, otherwise it lands between 4 GiB and 2^40, and the title's BVH builder refuses addresses above that. `XMemVirtualQuery` reports committed pages as graphics memory, which the BVH builder requires. `XMemAlloc` and `XMemFree` go through the title's exported `__XMemAllocationHooks__`, and the attribute's bits 24-28 give the log2 alignment (`xmem.rs`) | Page modes and placeholders are handled, but no address range is requested, the query leaves the extended fields zero, and `XMemAlloc` uses a fixed 64-byte alignment and ignores the hooks (`runtime/shims/xmem.cpp`) |

## Other differences

| Area | What VIctory records | This runtime |
|---|---|---|
| Console or PC | The title does not ask the runtime what device it is on: code linked into it reads `HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM`, value `ConsoleMode`, and takes a missing value for a PC. VIctory answers 0x64 for Series X and 0x60 for Series S (`xsystem.rs`) | No registry shim, so the host answers and the title sees a PC. `XSystemGetDeviceType` (`runtime/shims/xgameruntime.cpp`) is not what the title calls |
| GameInput by ordinal | The title imports `GameInputCreate` by ordinal 1 only (`gameinput.rs`) | **Done in this fork:** `gameinput.dll!#1` is registered too (`runtime/shims/gameinput.cpp`); before, an ordinal import fell back to the host's DLL or an unimplemented thunk (`runtime/src/pe_loader.cpp`). Still open: the keyboard as a device of its own ([gameinput.md](gameinput.md), VERIFY) |
| Relaunch | `XLaunchNewGame` does not return; the title relaunches itself with its path from `GetModuleFileNameA` (`xgame.rs`) | `XLaunchNewGame` logs and returns (`runtime/shims/xgameruntime.cpp`) |
| Main module identity | `GetModuleHandle(NULL)` and `GetModuleFileName*` answer with the title's image and path (`kernel.rs`) | They answer with `gta6rt.exe` ([xcl_compare.md](xcl_compare.md) lists the same difference) |
| Virtual memory range | `D3D12ConfigureVirtualMemoryX` writes the user-space range to its two outputs (`crates/victory-gpu/src/device.rs`) | It returns `S_OK` and writes nothing (`runtime/shims/d3d12_xs.cpp`) |
| Text entry | The title takes `E_ABORT` for "cancelled" and carries on; any other failure is an error (`xgameui.rs`) | Text entry fails with `E_FAIL` (`runtime/shims/xgameruntime_offline.cpp`) |
| CRT file opens | `fopen`, `_wfopen`, `_open`, `_wstat64`, `std::_Fiopen` and the others take virtual-drive paths too, as do `GetDiskFreeSpace(Ex)W` and `SetCurrentDirectoryA` (`files.rs`) | Only the kernel32 family maps `G:\` and `T:\` (`runtime/shims/file_system.cpp`) |

## Small fixes found on the way

- An unknown interface id of a served class got `E_NOINTERFACE` without a log line (`runtime/shims/xgameruntime.cpp`,
  `QueryApiImpl`), which hid row 3. **Done:** it is logged now, as a class that is not served is.
- [runtime_imports.md](runtime_imports.md) listed XAccessibilityImpl and XLauncherImpl as not served; the runtime
  serves both. **Done:** corrected there; only XAppCaptureImpl is still missing.
