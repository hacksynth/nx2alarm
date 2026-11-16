# nx2alarm — Arch Linux ARM port: working spec (M1)

Status: **M1 delivered** (2026-11-16); M2 is next. This file is the M1 contract for the work in
this repository; the evidence log is `docs/M1_VERIFICATION.md`. Read it before touching code.

## 1. What nx2alarm is

nx2alarm is the **Arch Linux ARM port of [nx2android](https://github.com/shirai91/nx2android)**.
nx2android turns a player's own Switch game files into a native Android APK: the game's own
arm64 code runs on the device, Switch system services are replaced by a host layer
(`nxnative/dm/host`), and NVN graphics calls are replayed on Vulkan
(`nxnative/dm/android/gfx_vk_*`). nx2alarm does the same on **aarch64 Arch Linux ARM**
(this device: Huawei MateBook E 2019, Snapdragon 850 / Adreno 630), replacing the Android
platform layer with Linux: SDL3 window/input/audio instead of NativeActivity/APK/AAudio/
MediaCodec, and native aarch64 Linux builds instead of the Android NDK.

This repository is a full fork of nx2android at tag/commit `e5a4703` (see
`docs/UPSTREAM_README_nx2android.md`, `nxnative/LICENSE`). The Android target is kept intact;
the port adds a new platform layer, `nxnative/dm/alarm/`.

## 2. Milestone M1 (this session)

Deliverables, in order of importance:

1. **`nx2alarm` shell binary** — a native aarch64 Linux program that runs on this device
   without Android, without game files and without the Android NDK:
   - opens an SDL3 window (Wayland) when a display is available;
   - `nx2alarm doctor` prints a device / prerequisite audit (CPU, GPU, RAM, SDL3 video/audio/
     input, Vulkan loader + ICD, dma-buf heaps, plugin dirs) and exits 0/1;
   - `nx2alarm shell [--headless] [--frames N] [--screenshot FILE]` runs the shell window and
     can save a frame (BMP/PPM) for verification;
   - `nx2alarm version` / `nx2alarm help`.
2. **`nx2alarm-host` binary** — the *upstream host core* (`nxnative/dm/host/*`, `dm/gfx/*`,
   `dm/gfx/nvn`, `dm/gfx/tex`, the null graphics backend `dm/android/gfx_null.c` and
   `dm/android/gfx_common.c`) compiled **natively with gcc for aarch64 Linux** and linked
   against glibc + SDL3, with weak platform stubs under `nxnative/dm/alarm/`. It must start
   and print the upstream usage message (`usage: dmhost <dir> [mod ...] [-- game arguments...]`)
   when run without a game build directory, and must not link against Android libraries.
   It does **not** have to boot a real game in M1 (no game files / prod.keys on this machine).
3. **Vulkan backend port (M2 groundwork)** — `dm/android/gfx_vk_*` adapted for Linux behind
   `DM_ALARM`: SDL3 `VkSurfaceKHR` + swapchain present instead of `ANativeWindow`, and a Linux
   memory-pool path (dma-buf heaps, with the existing copied-pool fallback) instead of
   `AHardwareBuffer`. Compiled and wired into the host build when it builds; not required for
   M1 acceptance, must never break the null backend.
4. **Docs** — `README.md` (repo root), `docs/FEASIBILITY.md` (device audit, evidence),
   `docs/ALARM_ARCHITECTURE.md` (port map: what is reused, what is replaced, file by file),
   `docs/PORTING_PLAN.md` (M1→M5 plan, risks).

### Non-goals for M1

- `porter/` / `gui/` changes (Linux packaging of ports, GUI target selector). The Android
  pipeline must keep working unchanged.
- Real gameplay validation (needs a user-owned NSP/XCI dump + `prod.keys` + a Vulkan ICD).
- Installing system packages (no `sudo`/`pacman` in this session).
- iOS/Android targets removed or broken. If a shared file must change for glibc, guard with
  `#if defined(DM_ALARM)` or a glibc feature check so the Android build is untouched.

## 2b. M1 results (2026-11-16)

All acceptance checks in §6 ran green on this machine; the full log is `docs/M1_VERIFICATION.md`.

| Deliverable | Result |
|---|---|
| `build.py` shell build | 3 sources, 0 warnings (`-Wall -Wextra`), `build/alarm/nx2alarm` 75 KiB, NEEDED = SDL3/libm/libc |
| `nx2alarm doctor` | 14 PASS / 5 WARN / 0 FAIL, exit 0; WARN = no Vulkan ICD/instance, no `/dev/dma_heap`, no keys/game data |
| `nx2alarm shell` | Wayland window 1280x720, OpenGL renderer, screenshot → `docs/images/nx2alarm-shell.png`; `--headless`/`--frames`/`--screenshot` and `SDL_VIDEODRIVER=dummy` all exit 0 |
| `build_host.py` | 182 C + 3 asm sources, clean build 56.3 s at `-j3`, `build/alarm/nx2alarm-host` 8,066,920 B, no Android libraries in `ldd`; prints upstream usage and exits via `die()` (exit 134) |
| Platform layer | 46 strong symbols (SDL3 audio, saves, LAN, haptics, perf, assets) linked; only five front-end window defaults remain weak |
| Vulkan backend | 9/9 sources compile with `-DDM_ALARM`; surface/present guarded; AHardwareBuffer refused → copied-pool fallback; no runtime test (no ICD) |
| Upstream tests | 267 run, **0 failures**, 22 errors from missing optional Python packages (`lz4`, `PIL`, …), 50 skips; the focused present-timing test that the guarded edit touches is green |

**M2 (2026-11-16, after installing `vulkan-freedreno` + `vulkan-tools`):** Turnip Adreno 630 /
Vulkan 1.3.354 detected by `doctor` (16 PASS/3 WARN/0 FAIL); `nx2alarm-vk info|bc|smoke` verified
the device, BC1+BC3 GPU decode, and SDL3→surface→Wayland swapchain→present at ~60 fps in both
BGRA8 and RGBA8 with resize handling; `build_host.py --gfx vk` links the real backend
(`nx2alarm-host-vk`, 9.6 MiB, no Android/Vulkan NEEDED). Remaining: M2.1 BGRA8 conversion draw
runtime test + dma-buf zero-copy; then M3 (`porter --target alarm`, game data, guest TLS).
Evidence: `docs/M2_VERIFICATION.md`.

Remaining for M2.1/M3: the conversion draw is compile-only until a game frame exists; dma-buf
zero-copy needs `/dev/dma_heap`; `porter --target alarm`, guest loader/TLS validation, movies
and UI. Tracked in `docs/PORTING_PLAN.md`.

## 3. Environment facts (this machine, measured)

| Fact | Value |
|---|---|
| Board | Huawei MateBook E 2019, PAK-AL09, `huawei,planck` + `qcom,sdm845` |
| SoC | Snapdragon 850 (SDM850), 8× Kryo-385, Adreno 630, Hexagon |
| OS | Arch Linux ARM, kernel `7.3.4-g7595a62d45ee` (custom), GNOME Wayland (`DISPLAY=:0`, `WAYLAND_DISPLAY=wayland-0`) |
| RAM | 7.3 GiB total / ~3.5 GiB available, no swap |
| GPU user space | Mesa 26.2.7 (OpenGL works), **no Vulkan ICD installed** (`/usr/share/vulkan` absent; only Chromium's SwiftShader ICD exists) |
| SDL | SDL3 3.4.20 (`pkg-config sdl3`), SDL2-compat 2.32.74 |
| Compiler | gcc 16.1.1 (native aarch64), no clang/llvm, no Android NDK, no .NET SDK, no Java |
| Vulkan headers | `nxnative/dm/third_party/vulkan-headers/include` (1.4.x) in the repo |
| Game data | **none** on this machine (`prod.keys`, NSP/XCI/NSZ/XCZ all absent) |
| Python | 3.14.10, stdlib only (no pip packages: no PySide6/lz4/pillow/capstone) |

Consequences for M1: build must use **gcc + system SDL3 + the repo's Vulkan headers**; the
Vulkan device may be absent, so the shell must work with an SDL renderer fallback and
`doctor` must report the missing ICD as a warning, not a crash.

## 4. Repository layout for the port

```
nxnative/dm/alarm/            new Linux/ALARM platform layer (all new files here)
  build.py                    shell build (gcc, SDL3); --target shell
  build_host.py               host-core build (gcc, glibc, null gfx)
  alarm.h                     shared shell/doctor declarations and options
  entry_alarm.c               main(): shell / doctor / version / help
  front_alarm.c               SDL3 front end: window, events, gamepad/touch/keyboard,
                              front_run() for the host binary, text input
  doctor.c                    device + prerequisite audit (`nx2alarm doctor`)
  gfx_vk_alarm.c              (M2) Linux surface/present side of the Vulkan backend
  compat/                     Linux shims for Android-only headers when compiling the
                              Vulkan backend (ANativeWindow -> SDL window, AHardwareBuffer
                              -> dma-buf heap, jni.h/media/AAudio stubs)
  platform/                   real platform implementations for the host core:
                              audio (SDL3), saves dirs, LAN, haptics, perf, assets
  stubs_alarm.c               weak stubs for every platform symbol the host core needs
  hle_table_alarm.c           Linux replacement for dm/host/hle_table.c (Apple-only section)
  host_main_alarm.c           main() wrapper: calls dm_host_main() (host/main.c compiled
                              with -Dmain=dm_host_main)
  README.md                   short guide to this directory
build/alarm/                  build outputs (git-ignored by the existing /build/ rule)
```

Everything else under `nxnative/` is upstream code. New shared-code edits are allowed only
with `#ifdef DM_ALARM` / `__linux__` guards and must be listed in the final report.

## 5. Interfaces and conventions

- Compile flag: `-DDM_ALARM=1`. Every object of the host build gets it.
- The host core must see `__APPLE__` **undefined** so `nxnative/dm/host/dm.h` selects the ELF
  branch (`section(s)` + `__start_/__stop_` tables). The Android-only Apple section in
  `dm/host/hle_table.c` cannot be assembled by GNU as; use `hle_table_alarm.c` with
  `section("swhle")` instead and exclude the upstream file from the Linux build.
- `dm/android/compat/include` is the Darwin-compat shim and is reused on Linux. glibc fixes go
  in `dm/android/compat/include/darwin_compat.h` / `dm/android/compat/darwin_compat.c` guarded
  by `!defined(__ANDROID__)` (or `defined(DM_ALARM)`) so bionic builds are unchanged.
- Platform functions are exported with **weak** definitions in `stubs_alarm.c`
  (`__attribute__((weak))`), so a strong implementation in `platform/` or `front_alarm.c`
  replaces them at link time without duplicate-symbol errors.
- Do not edit files outside your assigned write scope. If a shared fix is needed, message the
  Lead with file, line, exact patch and reason.
- Do not run `git` commands (the Lead commits). Do not touch `_recon/`.
- Build with at most `-j4`; the machine has 8 cores but ~3.5 GiB free RAM. Avoid repeated
  full rebuilds.
- C style: follow the file you are editing (4-space indentation in `dm/host`, tabs are not
  used). Keep upstream comment style (short, factual, English).
- Every agent: test your own work before reporting. Report: files changed, exact commands,
  observed output, blockers, and what remains.

## 6. Acceptance checks (Lead runs these at the end)

```sh
cd <repo>
python3 nxnative/dm/alarm/build.py --target shell      # build shell
./build/alarm/nx2alarm version
./build/alarm/nx2alarm doctor                          # exit 0 or 1, full report
./build/alarm/nx2alarm shell --headless                # no display needed
./build/alarm/nx2alarm shell --frames 2 --screenshot /tmp/nx2alarm.ppm  # window render
python3 nxnative/dm/alarm/build_host.py                # build host core
./build/alarm/nx2alarm-host                            # prints upstream usage, exit != 0
ldd ./build/alarm/nx2alarm-host | grep -E 'android|liblog' && echo BAD
```

## 7. Known risks (recorded for `docs/PORTING_PLAN.md`)

- **Guest TLS slot**: `dm/host/loader.c` stores guest thread slots in bionic's
  `TLS_SLOT_APP` (`tpidr_el0[2]`). glibc's TCB layout differs; verify the slot is unused by
  glibc (runtime check) and adapt `dm_tsd_set/get` + the rewriter's offset if not.
- **dma-buf**: the Vulkan backend's zero-copy pools use `AHardwareBuffer`; on Linux use
  `/dev/dma_heap/*` + `VK_EXT_external_memory_dma_buf`, or fall back to copied pools
  (`DM_VK_NO_ZEROCOPY=1` semantics) for correctness first.
- **BC textures**: the port needs a GPU that reads BC-compressed textures (Adreno 630 does on
  Android; Turnip/freedreno on Linux must be validated once a Vulkan ICD is installed).
- **No Vulkan ICD / no game files**: M1 stops at compile + shell + host bootstrap; gameplay is
  M3+ and needs user-owned assets.
- **Performance**: Snapdragon 850 is far slower than the Adreno 8xx devices nx2android targets;
  expect only lighter titles, low render scale, and no guarantee.
