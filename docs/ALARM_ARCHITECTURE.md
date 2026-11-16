# Architecture: how nx2alarm reuses nx2android

nx2alarm is a **platform re-target** of nx2android, not a rewrite. The upstream engine is
kept: the game's own arm64 code runs natively, Horizon services are replaced by the host HLE,
NVN calls are replayed on a Vulkan backend, and the same game profiles / rewriter / shader
pipeline are used. Only the **host platform layer** changes: Android → Arch Linux ARM.

```
porter/ (Python pipeline)                 reused: inputs → extract → rewrite → shaders
  │                                        replaced (M3): host build (NDK → gcc) and
  │                                        apk/data pack (APK → Linux app dir)
  ▼
nxnative/dm/games/<title ID>/             reused: game profiles (game.tsv, shader tables,
  │                                        patches.py, android.tsv metadata)
  ▼
nxnative/dm/host/   (Horizon HLE)         reused: loader (NSO/NRO), hle_*.c services,
  │                                        memory, threads, IPC, fs, audio, movie, hid
  ├── nxnative/dm/gfx/                   reused: NVN layer, texture decoders, null backend
  ├── nxnative/dm/android/gfx_vk_*.c      ported: Vulkan backend (surface/memory/present)
  └── nxnative/dm/alarm/                  NEW: Linux/ALARM platform layer
```

## 1. What is reused unchanged

| Layer | Path | Role |
|---|---|---|
| Horizon HLE | `nxnative/dm/host/hle_*.c` | Switch OS services (fs, hid, audio, threads, svc, socket, ldn, …) on top of the host. Platform-independent C; compiles for Linux with the Darwin-compat shim. |
| Guest loader | `nxnative/dm/host/loader.c`, `entry.S` | Maps the rewritten NSO/NRO images, relocations, symbol tables, gate dispatch, guest TLS. Arm64 Linux executes the same code. |
| Rewriter + shader tools | `nxnative/dm/tools/`, `dm/gfx/tools/`, `dm/games/*/patches.py` | Python, runs on the PC (the porter). Unchanged: the same patched NSOs are produced for ALARM. |
| NVN layer + textures | `nxnative/dm/gfx/nvn/`, `dm/gfx/tex/`, `hle_nv.c`, `hle_vi.c` | NVN API implementation over the replay backend; BC/deswizzle decoders. |
| Null graphics backend | `dm/android/gfx_null.c`, `gfx_common.c` | CPU-side replay used for bring-up; keeps game logic correct with no GPU. |
| Game profiles | `nxnative/dm/games/<id>/` | Per-build constants, shader tables, rewriter cuts, plus `android.tsv` (status, `requires bcn`, `sysdata`, settings, overlay layout, env). All still meaningful for ALARM; `android.tsv` is read as generic "port metadata" for now. |
| Third-party | `nxnative/dm/third_party/` | Vulkan headers, zstd, libvpx, stb_vorbis, BC decoders, Ryujinx shader translator, fonts. |
| Vulkan backend core | `nxnative/dm/android/gfx_vk_{exec,tex,shader,dq}.c` | The NVN→Vulkan replay itself (command execution, textures, shader bundle, descriptor cache). Ported only at its platform edges. |

## 2. What is replaced (new `nxnative/dm/alarm/`)

| Android runtime piece | Replaced by | Milestone |
|---|---|---|
| `dm/android/front_android.c`: NativeActivity, JNI callbacks, ANativeWindow, sensors, IME, activity lifecycle | `dm/alarm/front_alarm.c`: SDL3 window + event loop, gamepad/touch/keyboard, `front_run()`/`front_text_input()`/`front_save_failed()`; XDG paths | M1 shell, M3 game window |
| `dm/android/gfx_vk_present.c` ANativeWindow surface + `VkAndroidSurfaceKHR` swapchain | `dm/alarm/gfx_vk_alarm.c` + `#ifdef DM_ALARM` present path: `SDL_Vulkan_CreateSurface`, `VkSwapchainKHR`, SDL resize/fullscreen | M2 |
| `dm/android/gfx_vk_core.c` `AHardwareBuffer` zero-copy NVN pools | copy-pool fallback first (allocator refusal path); later `/dev/dma_heap` + `VK_EXT_external_memory_dma_buf` | M2 / M2.1 |
| `dm/android/compat/audiotoolbox_aaudio.c` (AudioUnit on AAudio) | `dm/alarm/platform/audio_sdl.c` (AudioToolbox subset on SDL3 audio) | M1 (compile), M3 (tune) |
| `dm/android/compat/audioconverter_mediacodec.c` (AudioConverter on MediaCodec) | SDL3/CPU converter or stub; AAC movie audio only | M3 |
| `dm/android/movie_avc.c`, `dm/host/hle_movie_avc.c` (MediaCodec H.264) | `dm/alarm/platform/movie_alarm.c` graceful stub; later V4L2 Venus (H.264/HEVC decode is working on this device) or ffmpeg | M3+ |
| `dm/android/gl_android.c` (EGL/GLES for GL games) | `dm/alarm/gl_alarm.c` on desktop EGL/Mesa (OpenGL works on Adreno 630) | M4 |
| `dm/android/saves_android.c` (JNI storage) | `dm/alarm/platform/saves_alarm.c` (XDG `$XDG_DATA_HOME/nx2alarm/<title ID>/`) | M1/M3 |
| `dm/android/lan_android.c` (Wi-Fi multicast lock) | `dm/alarm/platform/lan_alarm.c` (plain UDP multicast) | M1 |
| `dm/android/haptics_android.c` (Vibrator) | `dm/alarm/platform/haptics_alarm.c` (SDL3 rumble) | M1 |
| `dm/android/perf_android.c` (ADPF/game mode/GPU turbo) | `dm/alarm/platform/perf_alarm.c` (no-op; governor/thermals stay with the OS) | M1 |
| `dm/android/asset_manager` (APK assets: gamepad theme, fonts) | `dm/alarm/platform/assets_alarm.c` (filesystem `$DM_ALARM_DATA`) | M1 |
| `dm/android/build.py` (NDK clang, `-landroid -llog -lvulkan -laaudio …`) | `dm/alarm/build_host.py` (gcc 16, glibc, SDL3; no Android libs) | M1 |
| `dm/android/apk/mkapk.py` + `porter` install stage (adb) | Linux app dir + launcher script/desktop file (M3); porter `--target alarm` | M3 |
| `dm/android/java`, `external_input_event.h` (external display) | not ported (out of scope) | — |

The Android target stays buildable: platform-specific edits are guarded with
`#if defined(DM_ALARM)` or `#if !defined(__ANDROID__)`, and `dm/android/` keeps its own
`build.py`/NDK path.

## 3. Platform contract between the host core and the ALARM layer

The host core already defines the interface (`nxnative/dm/host/front.h`, `dm.h`, the
`nvn_mtl.h` backend interface, `dm_input.h`):

- `front_run(guest)` — run the guest (M1: synchronously; M3: guest thread + SDL main loop).
- `front_text_input(...)` — software keyboard (M3: on-screen SDL text entry or a native dialog).
- `front_save_failed(err)`, `dm_touch_set(...)`, `front_screen_max_fps()`,
  `front_screen_match(fps)` — save failure prompt, touch points, display refresh.
- `dm_front_window()` — the presentable window the Vulkan backend renders to (M2: SDL window).
- `dm_overlay_*`, `dm_menu_*`, `dm_save_note`, `dm_android_temps`, audio route, haptics, perf —
  front-end features. M1 provides weak stubs (`dm/alarm/stubs_alarm.c`); M3/M4 implement them.

## 4. Build layout

```
python3 nxnative/dm/alarm/build.py --target shell   → build/alarm/nx2alarm        (SDL3, shell+doctor)
python3 nxnative/dm/alarm/build_host.py             → build/alarm/nx2alarm-host   (host core, null gfx)
```

The host binary needs a generated `game_profile.h` (from `dm/games/<id>/game.tsv`, via
`dm/games/game.py header`); the build script picks a profile at build time exactly as the
Android host build does. The profile is compiled in, so a port is built per game — same as
Android.

## 5. Runtime data layout (M3 target)

```
<port dir>/
  nx2alarm-host            (or an app dir with this + libSDL3)
  mods/                    rewritten NSO/NRO modules (porter stage "host")
  RomFS/  aoc/             game data
  sysdata/                 firmware system titles, if the profile needs them
  shaders.dmsv             Vulkan shader bundle
  env.txt settings.txt     DM_* switches and front-end settings
  saves/                   XDG data dir fallback
```

This mirrors the Android data pack (`README.md` "data" stage) with `saves/` moved to the
XDG data dir. The porter does not produce it yet (M3).

## 6. Status / limits of this checkout

- M1 (this session): shell + doctor run on the device; the host core compiles and links
  natively but cannot boot a game without rewritten game data; the Vulkan backend is ported
  at the platform edges and compile-checked, but cannot be run (no Vulkan ICD installed and no
  Vulkan device test).
- No game files, `prod.keys`, Android NDK or .NET SDK are present; extraction/rewriting and
  real gameplay are out of scope until the owner supplies those.
