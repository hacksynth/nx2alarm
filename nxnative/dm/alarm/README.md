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

## Layout

- `build.py` — shell build (SDL3).
- `build_host.py` — host-core build (gcc, glibc, `dm/android/compat` Darwin shims, null gfx).
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

**M1 delivered (2026-11-16).** On the target machine (Huawei MateBook E 2019, aarch64 Arch
Linux ARM, gcc 16, SDL3 3.4.20, no Vulkan ICD, no game data):

```console
$ python3 nxnative/dm/alarm/build.py --target shell   # 3 sources, 0 warnings, 75 KiB
$ ./build/alarm/nx2alarm doctor                        # 14 pass, 5 warn, 0 fail (exit 0)
$ ./build/alarm/nx2alarm shell --frames 3 --screenshot /tmp/nx2alarm.ppm
$ python3 nxnative/dm/alarm/build_host.py              # 182 C + 3 asm -> nx2alarm-host (7.7 MiB)
$ ./build/alarm/nx2alarm-host                          # upstream usage; no Android libs in ldd
$ bash nxnative/dm/alarm/check_vk.sh                    # 9/9 Vulkan sources compile with -DDM_ALARM
```

- `nx2alarm` (75 KiB): SDL3 shell/audit; `NEEDED` = SDL3/libm/libc only.
- `nx2alarm-host` (8,066,920 B): upstream host core + null graphics + Linux platform layer
  (audio, saves, LAN, haptics, perf, assets) compiled with gcc; starts without game data and
  prints the upstream usage; the weak symbols are only the five front-end window defaults.
- Vulkan backend: platform edges guarded with `DM_ALARM`, all sources compile; not runnable
  until a Vulkan ICD is installed (M2). No zero-copy pools: AHardwareBuffer allocations are
  refused so the backend uses its copied-pool fallback (`/dev/dma_heap` is absent here).
- Not implemented in M1: real gameplay/porter Linux target, H.264/AAC movies (graceful stub),
  on-screen gamepad/pause menu, GLES games, zero-copy dma-buf pools. See
  `docs/PORTING_PLAN.md` for M2–M5.

The full evidence log is `docs/M1_VERIFICATION.md` at the repository root; the device audit is
`docs/FEASIBILITY.md`.

