# dm/alarm — the Arch Linux ARM platform layer

This directory is the **only new platform code** of the nx2alarm port. It replaces the
Android runtime (`nxnative/dm/android`: NativeActivity, APK assets, AAudio, MediaCodec,
AHardwareBuffer, JNI) with a Linux layer built on **SDL3** (window, input, audio) and native
aarch64 Linux builds with gcc/glibc.

Read `docs/ALARM_PORT.md` at the repository root first: it is the M1 contract (scope,
interfaces, ownership, acceptance checks).

## Binaries

| Binary | Build | Purpose |
|---|---|---|
| `build/alarm/nx2alarm` | `python3 nxnative/dm/alarm/build.py --target shell` | Shell + doctor. Runs without game files, Android NDK or Vulkan ICD. |
| `build/alarm/nx2alarm-host` | `python3 nxnative/dm/alarm/build_host.py` | The upstream host core (Horizon HLE + NVN replay, null graphics) compiled natively for aarch64 Linux. Starts and prints the upstream usage message without a game build directory. |
| `build/alarm/nx2alarm-host-vk` | `python3 nxnative/dm/alarm/build_host.py --gfx vk` | The same host core with the **real Vulkan backend** (`gfx_vk_*.c` + `gfx_vk_alarm.c`); same usage path without game data. Links no libvulkan (the backend dlopens the loader). |
| `build/alarm/nx2alarm-vk` | `python3 nxnative/dm/alarm/build.py --target vk-smoke` | M2 hardware test: `info` (device/features/BC formats), `bc` (BC1/BC3 GPU decode), `smoke` (SDL3→VkSurface→swapchain→present, `--screenshot`, `--format bgra|rgba`). Needs `vulkan-freedreno`. |

## Layout

- `build.py` — shell build (SDL3) and `--target vk-smoke` (M2 tool; needs `glslc`).
- `build_host.py` — host-core build (gcc, glibc, `dm/android/compat` Darwin shims); `--gfx {null,vk}`.
- `vk_smoke.c` + `vk/bc_sample.comp` — M2 Vulkan tool (device/BC/present verification).
- `check_vk.sh` — compile-check the whole Vulkan backend for Linux without a device.
- `entry_alarm.c` — CLI dispatch (`shell`, `doctor`, `version`, `help`).
- `front_alarm.c` — SDL3 front end; also the Linux `front_run()`/`front_text_input()` the
  host core expects.
- `doctor.c` — prerequisite and device audit.
- `platform/` — strong implementations that replace the weak stubs: audio (SDL3), saves
  directories, LAN, haptics, performance hints, assets.
- `stubs_alarm.c` — weak stubs for the platform functions the host core references.
- `compat/` — Linux shims for Android-only headers (used only by the Vulkan backend).
- `gfx_vk_alarm.c` — (M2) Linux surface/present side of the Vulkan backend.
- `hle_table_alarm.c` — ELF replacement for `dm/host/hle_table.c` (Apple-only section).
- `host_main_alarm.c` — `main()` of the host binary; calls `dm_host_main()`.

## Conventions

- `-DDM_ALARM=1` for every host-build object.
- Platform functions are weak in `stubs_alarm.c`; strong definitions in `platform/` and
  `front_alarm.c` win at link time.
- Upstream edits are only allowed behind `#if defined(DM_ALARM)` or a glibc feature check,
  and must be reported to the Lead.

## Status

**M1 + M2 delivered (2026-11-16).** On the target machine (Huawei MateBook E 2019, aarch64 Arch
Linux ARM, gcc 16, SDL3 3.4.20, Turnip 26.2.4 / Vulkan 1.3.354 installed, no game data):

```console
$ python3 nxnative/dm/alarm/build.py --target shell   # 3 sources, 0 warnings, 75 KiB
$ ./build/alarm/nx2alarm doctor                        # 16 pass, 3 warn, 0 fail (M2)
$ ./build/alarm/nx2alarm shell --frames 3 --screenshot /tmp/nx2alarm.ppm
$ python3 nxnative/dm/alarm/build_host.py              # 182 C + 3 asm -> nx2alarm-host (7.7 MiB)
$ ./build/alarm/nx2alarm-host                          # upstream usage; no Android libs in ldd
$ bash nxnative/dm/alarm/check_vk.sh                    # 9/9 Vulkan sources compile with -DDM_ALARM
# M2:
$ python3 nxnative/dm/alarm/build.py --target vk-smoke
$ ./build/alarm/nx2alarm-vk info                       # Turnip Adreno 630, Vulkan 1.3.354
$ ./build/alarm/nx2alarm-vk bc                         # BC1/BC3 decode PASS
$ ./build/alarm/nx2alarm-vk smoke --frames 120 --format bgra --screenshot /tmp/vk.ppm
$ python3 nxnative/dm/alarm/build_host.py --gfx vk     # nx2alarm-host-vk (9.6 MiB)
```

- `nx2alarm` (75 KiB): SDL3 shell/audit; `NEEDED` = SDL3/libm/libc only.
- `nx2alarm-host` (8,066,920 B): upstream host core + null graphics + Linux platform layer
  (audio, saves, LAN, haptics, perf, assets) compiled with gcc; starts without game data and
  prints the upstream usage; the weak symbols are only the five front-end window defaults.
- M2 Vulkan: Turnip device/1.3.354 verified, BC1+BC3 GPU decode PASS, SDL surface + Wayland
  swapchain + present at ~60 fps in BGRA8 and RGBA8 with resize, `build_host.py --gfx vk`
  links the real backend. No zero-copy pools yet (`/dev/dma_heap` absent → copied pools);
  the BGRA8 conversion draw is compile-only until a game frame exists (M2.1).
- Not implemented yet: real gameplay/porter Linux target, H.264/AAC movies (graceful stub),
  on-screen gamepad/pause menu, GLES games, dma-buf zero-copy. See `docs/PORTING_PLAN.md`.

Evidence: `docs/M1_VERIFICATION.md` and `docs/M2_VERIFICATION.md` at the repository root; the
device audit is `docs/FEASIBILITY.md`.

