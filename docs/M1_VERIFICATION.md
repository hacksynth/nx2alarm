# M1 verification log

Date: 2026-11-16 (Asia/Shanghai) · Machine: Huawei MateBook E 2019 (SDM850, Adreno 630),
aarch64 Arch Linux ARM, GNOME Wayland, gcc 16.1.1, SDL3 3.4.20, no Vulkan ICD, no game data.

Every result below was produced on the target machine in this checkout, from a clean build or an
incremental rebuild of the mentioned command. Nothing here needs an Android NDK, .NET SDK or
game data.

## 1. Shell build and CLI

```console
$ python3 nxnative/dm/alarm/build.py --target shell
  output   : .../build/alarm/nx2alarm
build.py: compiled 3 of 3 source(s), 0 warning(s)
build.py: built build/alarm/nx2alarm (75 KiB)

$ ./build/alarm/nx2alarm version
nx2alarm 0.1.0-m1
native aarch64 Linux shell for nx2android
  compiler : gcc 16.1.1
  SDL3     : compiled 3.4.20, runtime 3.4.20
  targets  : shell window, doctor; no Android NDK, no .NET, no game data needed
```

Build details: `build.py` compiles `entry_alarm.c`, `front_alarm.c`, `doctor.c` with
`-std=c11 -O2 -Wall -Wextra -DDM_ALARM=1`, links `pkg-config --libs sdl3`; `NEEDED` is
`libSDL3.so.0, libm, libc` only (no `libvulkan`, no Android libraries). Incremental rebuild
reports `compiled 0 of 3 source(s)`.

## 2. `doctor` on this machine

```console
$ ./build/alarm/nx2alarm doctor ; echo $?
nx2alarm doctor -- device and prerequisite audit
nx2alarm 0.1.0-m1
== device ==
[PASS] Board        Huawei MateBook E 2019
[PASS] SoC          huawei,planck, qcom,sdm845 (Snapdragon 845/850 class)
[PASS] Kernel       Linux 7.3.4-g7595a62d45ee aarch64
[PASS] CPU          8 cores (4 x 0x803, 4 x 0x802); up to 2.84 GHz; implementer 0x51
[PASS] RAM          7499 MiB (7.3 GiB) total
[PASS] SDL3         3.4.20 (runtime), compiled 3.4.20
[PASS] Video        wayland (6 driver(s) compiled in)
[PASS] Display      1296x864 @ 60.0 Hz (display 2)         # panel is 2160x1440, logical 2x scale
[PASS] Audio        pipewire driver, default device: 内置音频 Speaker playback
[PASS] Gamepads     0 connected
== prerequisites ==
[PASS] Vulkan loader              libvulkan.so.1 (1.4.363)
[WARN] Vulkan device              no instance: -9 (VK_ERROR_INCOMPATIBLE_DRIVER)
[WARN] Vulkan ICD                 no manifests (/usr/share/vulkan/icd.d, /etc/vulkan/icd.d, /usr/local/share/vulkan/icd.d); Vulkan stays unavailable until a driver is installed
[WARN] dma-buf heap               absent (/dev/dma_heap missing); Vulkan zero-copy falls back to copied pools
[PASS] Toolchain                  gcc (GCC) 16.1.1 20260430; pkg-config sdl3 3.4.20
[PASS] Build outputs              shell: nx2alarm; host: build/alarm/nx2alarm-host
[WARN] Game keys                  not installed (no prod.keys; user-owned keys are needed only for gameplay)
[WARN] Game data                  not installed (no game dumps; M1 shell and doctor do not need them)
[PASS] Mesa                       26.2.7 (libgallium present; OpenGL works)
summary: 14 pass, 5 warn, 0 fail, 0 not checked
result: OK (warnings only; they do not fail doctor)
0
```

The four warnings are the real M2/M3 prerequisites: install a Vulkan ICD, enable dma-buf heaps
(optional), and supply owned game data + `prod.keys`. `doctor` exits 0 in this state; a missing
toolchain (e.g. empty `PATH`) is the kind of condition that makes it exit 1 (verified).

## 3. Shell render (Wayland) and headless modes

```console
$ ./build/alarm/nx2alarm shell --frames 3 --screenshot /tmp/nx2alarm_shell.ppm
nx2alarm shell: window 1280x720 (resizable), renderer opengl
nx2alarm shell: wrote screenshot /tmp/nx2alarm_shell.ppm (1280x720 PPM P6)
nx2alarm shell: rendered 3 frame(s)
$ echo $?
0

$ ./build/alarm/nx2alarm shell --headless ; echo $?
... full audit ...
  no window created (headless)
0

$ SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/alarm/nx2alarm shell --frames 2 --screenshot /tmp/x.ppm
nx2alarm shell: window 1280x720 (resizable), renderer software
nx2alarm shell: wrote screenshot /tmp/x.ppm (1280x720 PPM P6)
nx2alarm shell: rendered 2 frame(s)
0
```

The captured frame is committed as [`docs/images/nx2alarm-shell.png`](images/nx2alarm-shell.png)
(converted from the PPM). It shows the full audit: board, SoC, kernel, CPU, RAM, SDL3, video
driver, display, audio, gamepads and the prerequisite checklist, with the four WARN rows.

Robustness: `SDL_VIDEODRIVER=dummy` (headless/software renderer), a bogus video driver (clear
message + headless fallback, exit 0), a bogus audio driver (WARN, exit 0) and bad arguments
(usage, exit 2) were all checked.

## 4. Host core (`nx2alarm-host`)

```console
$ python3 nxnative/dm/alarm/build_host.py --clean
compiling 182 C / 3 asm files with -j3 ...
  185/185
linking ...
built build/alarm/nx2alarm-host (8066920 bytes, 7.7 MiB) in 56.3s        # exit 0

$ ./build/alarm/nx2alarm-host
[dm] [nvn] null device: 537 names, 423 implemented (409 semantic + 14 empty, 293 on the large host stack), the other 114 go to the red-reporting stub
[dm] FATAL usage: dmhost <dir> [mod...] [-- game arguments...]
# exit 134, upstream die() -> abort (no SIGSEGV/SIGILL)

$ ldd build/alarm/nx2alarm-host
        linux-vdso.so.1
        libSDL3.so.0
        libm.so.6
        libz.so.1
        libc.so.6
        ld-linux-aarch64.so.1
$ ldd build/alarm/nx2alarm-host | grep -E 'android|liblog'   # no output
```

The binary is an ELF64 aarch64 PIE. Its link set is the upstream host core (`dm/host` minus the
VideoToolbox movie backend and Apple-only `hle_table.c`, `dm/gfx`, NVN, texture decoders), the
null graphics backend, the Darwin-compat shim, the Linux platform layer and vendored libvpx /
zstd. Platform symbols are strong (`AudioComponent*`, `dm_haptics_*`, saves/LAN/perf/assets);
only the five front-end defaults are weak because that binary has no window front end yet. With
an empty game directory it reports `FATAL open .../main.fixups.tsv` plus a glibc-symbolized crash
report — the upstream behavior for missing game data.

## 5. Vulkan backend compile check (M2 groundwork)

```console
$ bash build/alarm/vkcheck/cc.sh 2>/dev/null | grep -E '^(OK|FAIL)'
OK   nxnative/dm/android/gfx_common.c
OK   nxnative/dm/android/gfx_vk_core.c
OK   nxnative/dm/android/gfx_vk_present.c
OK   nxnative/dm/android/gfx_vk_exec.c
OK   nxnative/dm/android/gfx_vk_tex.c
OK   nxnative/dm/android/gfx_vk_shader.c
OK   nxnative/dm/android/gfx_vk_dq.c
OK   nxnative/dm/android/gfx_vk_subgroup.c
OK   nxnative/dm/alarm/gfx_vk_alarm.c
```

`-DDM_ALARM=1` with the Linux shims under `nxnative/dm/alarm/compat/`; warnings only (upstream
style `-Wmisleading-indentation` in untouched code). Runtime testing is impossible here: no
Vulkan ICD is installed. The port map and the memory-path decision are in
[`../nxnative/dm/alarm/GFX_VK_PORT.md`](../nxnative/dm/alarm/GFX_VK_PORT.md) (M2 deliverable).

## 6. Upstream tests

```console
$ python3 -m unittest discover -s tests -p 'test_*.py'
Ran 267 tests in 34.8s
FAILED (errors=22, skipped=50)
```

- **0 failures.** The focused presentation test that our guarded present-path edit could have
  broken is green: `python3 -m unittest tests.test_vk_present_timing -v` → 2/2 OK.
  `tests/test_architecture.py` and `tests/test_commit_msg.py` also pass.
- The 22 errors are all missing optional Python packages on this machine (17 × `lz4`, 1 × `PIL`,
  the rest importer errors from `porter` needing `lz4`/`PySide6`), not code regressions; the
  50 skips are tests that need toolchains/NDKs or platform features. Install
  `requirements.txt` (plus pip) to run those.

## 7. Upstream edits for the port (all guarded)

| File | Change | Guard |
|---|---|---|
| `nxnative/dm/android/compat/include/darwin_compat.h` | `pthread_from_mach_thread_np` helper for glibc crash dumps | `#if !defined(__ANDROID__) && defined(__GLIBC__)` |
| `nxnative/dm/android/gfx_vk.h` | Linux platform include (Android Vulkan shims + `gfx_vk_alarm.h`) | `#if defined(DM_ALARM)` |
| `nxnative/dm/android/gfx_vk_core.c` | Linux loader (system `libvulkan.so.1`), instance extensions from SDL, no Android HAB extensions | `#if defined(DM_ALARM)` |
| `nxnative/dm/android/gfx_vk_present.c` | SDL3 surface creation, Linux window size/refresh, no JNI entry points; Android struct declaration kept before the branch so `tests/test_vk_present_timing.py` still extracts the resize logic | `#if defined(DM_ALARM)` / `#if !defined(DM_ALARM)` |

The Android build path is unchanged when `DM_ALARM` is undefined. `dm/android/build.py` still
builds the APK host with the NDK (not exercised here: no NDK on this machine).
