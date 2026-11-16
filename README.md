# nx2alarm

**Native Nintendo Switch game ports for Arch Linux ARM** — a compatibility layer that runs
the game's own arm64 code on an aarch64 Linux device, replaces the Switch's system services
with a host layer, and replays NVN graphics on Vulkan.

nx2alarm is a fork of **[nx2android](https://github.com/shirai91/nx2android)** (the Android
port of **[nxnative](https://github.com/seedds/nxnative)**, which does the same on Apple
devices). The engine — guest loader, Horizon HLE, NVN replay, rewriter, shader pipeline, game
profiles — is reused. The **platform layer is new**: SDL3 window/input/audio instead of
Android's NativeActivity/APK/AAudio/MediaCodec, and native aarch64 builds with gcc instead of
the Android NDK. The target device is a Huawei MateBook E 2019 (Snapdragon 850 / Adreno 630)
running Arch Linux ARM.

> **Status: M1 + M2 — shell, native host compile, and Vulkan on the real GPU (2026-11-16).**
> The device now has `vulkan-freedreno` (Turnip 26.2.4, Vulkan 1.3.354), and the Vulkan path is
> verified up to a presented frame and BC texture decode. This checkout still does **not** play
> games: that is M3 and needs the owner's game files + `prod.keys`.
>
> - `nx2alarm doctor` — device/prerequisite audit on Arch Linux ARM: **16 PASS / 3 WARN / 0 FAIL**;
>   the WARNs are the remaining next steps (no `/dev/dma_heap` → copied pools, no game keys,
>   no game data);
> - `nx2alarm shell` — native SDL3 window shell with the audit on screen (Wayland, screenshot
>   and headless modes), 75 KiB, no Android tooling;
> - `nx2alarm-vk` — M2 hardware test: `info` (device/features/BC formats), `bc` (BC1+BC3 GPU
>   decode, PASS), `smoke` (SDL3 → `VkSurfaceKHR` → Wayland swapchain → present; ~60 fps FIFO
>   in both BGRA8 and RGBA8, resize handled, GPU round-trip screenshot);
> - `nx2alarm-host` / `nx2alarm-host-vk` — the upstream nxnative host core compiled natively with
>   gcc (Horizon HLE + NVN layer), 7.7 MiB with the null backend and 9.6 MiB with the real
>   Vulkan backend (`--gfx vk`); both start and print the upstream usage message without game
>   data, and link no Android libraries;
> - the Linux platform layer (SDL3 audio, XDG saves, LAN, haptics, perf, assets) and the Vulkan
>   backend port (`#ifdef DM_ALARM`: SDL surface/present, copied-pool fallback, BGRA8 conversion
>   draw compiled).
>
> ![nx2alarm shell on the MateBook E 2019](docs/images/nx2alarm-shell.png)
>
> ![nx2alarm-vk smoke: GPU round-trip pattern](docs/images/nx2alarm-vk-smoke.png)
>
> Not yet: `porter` producing Linux ports, real gameplay, movies, on-screen gamepad; the host
> backend's own `vk_init()`/first game frame needs game data (M3). The path is in
> [`docs/PORTING_PLAN.md`](docs/PORTING_PLAN.md); evidence in
> [`docs/M2_VERIFICATION.md`](docs/M2_VERIFICATION.md).

Read the plan: [`docs/PORTING_PLAN.md`](docs/PORTING_PLAN.md) ·
[`docs/ALARM_PORT.md`](docs/ALARM_PORT.md) (M1 contract) ·
[`docs/M1_VERIFICATION.md`](docs/M1_VERIFICATION.md) (M1 evidence) ·
[`docs/M2_VERIFICATION.md`](docs/M2_VERIFICATION.md) (M2 Vulkan evidence) ·
[`docs/ALARM_ARCHITECTURE.md`](docs/ALARM_ARCHITECTURE.md) ·
[`docs/FEASIBILITY.md`](docs/FEASIBILITY.md) (device audit) ·
[`nxnative/dm/alarm/GFX_VK_PORT.md`](nxnative/dm/alarm/GFX_VK_PORT.md) (M2 Vulkan map).

## Quick start (on this device)

```sh
# The shell and the device audit (gcc + SDL3; no Android tooling):
python3 nxnative/dm/alarm/build.py --target shell
./build/alarm/nx2alarm version
./build/alarm/nx2alarm doctor
./build/alarm/nx2alarm shell --headless
./build/alarm/nx2alarm shell --frames 2 --screenshot /tmp/nx2alarm.ppm

# The host core, compiled natively for aarch64 Linux (null graphics backend):
python3 nxnative/dm/alarm/build_host.py
./build/alarm/nx2alarm-host          # prints the upstream usage: a game build dir is required

# M2 Vulkan hardware test (needs vulkan-freedreno; M1 shell does not):
python3 nxnative/dm/alarm/build.py --target vk-smoke
./build/alarm/nx2alarm-vk info
./build/alarm/nx2alarm-vk bc
./build/alarm/nx2alarm-vk smoke --frames 120 --format bgra --screenshot /tmp/vk.ppm

# Host binary with the real Vulkan backend instead of the null one:
python3 nxnative/dm/alarm/build_host.py --gfx vk
./build/alarm/nx2alarm-host-vk
```

Requirements for M1: Arch Linux ARM, gcc (16 works), SDL3 (`sdl3`), pkg-config, Python 3.
For real play (M2/M3) you also need a Vulkan ICD for Adreno 630 (e.g. `vulkan-freedreno` from
the Arch Linux ARM repos, checked with `vulkaninfo`) and the game data you own.

## Why "ALARM"?

*Arch Linux ARM* is commonly abbreviated **ALARM**; `nx2alarm` is the counterpart of
`nx2android` for that platform.

## What the port reuses

The port is deliberate about what it keeps and what it replaces:

| Kept from nx2android | Replaced for Linux |
|---|---|
| Guest loader (arm64 NSO/NRO), Horizon HLE (`dm/host`), NVN layer and texture decoders (`dm/gfx`), rewriter and shader tools, game profiles, third-party libraries | Android NativeActivity/JNI/ANativeWindow front end → SDL3; AAudio → SDL3 audio; MediaCodec movies → V4L2 Venus / ffmpeg (M3+); AHardwareBuffer pools → dma-buf heaps or copy fallback; NDK build → gcc; APK/data pack → Linux app dir |

Details and a file-by-file map: [`docs/ALARM_ARCHITECTURE.md`](docs/ALARM_ARCHITECTURE.md).

## Repository layout

```
nxnative/dm/            the engine (fork of nxnative)
  dm/alarm/             NEW: the Arch Linux ARM platform layer (shell, doctor, build scripts,
                        native platform backends, Vulkan surface glue)
  dm/host/, dm/gfx/     reused host core (Horizon HLE, NVN, textures)
  dm/android/           the Android runtime (kept working; source of the port)
  dm/games/             game profiles
porter/, gui/           the porting pipeline and desktop GUI (Android target unchanged)
docs/                   upstream docs + nx2alarm docs (ALARM_PORT, ALARM_ARCHITECTURE,
                        PORTING_PLAN, FEASIBILITY)
```

## Licensing, keys and game files

- nx2alarm is **MIT**, as nx2native/nx2android are: see [`LICENSE`](LICENSE)
  (MIT, © 2026 seedds) and [`nxnative/LICENSE`](nxnative/LICENSE). Third-party code keeps its
  own license (Ryujinx, zstd, libvpx, Vulkan headers, fonts, …).
- Like nx2android, this repository contains **no keys, no game files, and nothing extracted
  from them**. You need your own dumped `prod.keys` and your own game files; the tool never
  downloads or ships them. Do not share the keys or ports built from them.
- Not affiliated with Nintendo. Whether decrypting your own games for your own use is allowed
  depends on where you live.

## Credits

- [nx2android](https://github.com/shirai91/nx2android) (MIT) — the Android port this project
  forks; engine, pipeline, profiles and docs.
- [nxnative](https://github.com/seedds/nxnative) (MIT) — the original ntnative host engine
  for Apple devices.
- [Ryujinx](https://github.com/Ryujinx/Ryujinx) (MIT) — shader translator and common code.
- The upstream `README` for nx2android is preserved at
  [`docs/UPSTREAM_README_nx2android.md`](docs/UPSTREAM_README_nx2android.md); the upstream
  architecture and development docs remain in `docs/`.
