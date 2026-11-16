# nx2alarm — Vulkan backend port map (M2)

Status: **M2 compile slice done** (2026-11-16). The whole backend (`dm/android/gfx_vk_*.c`,
`gfx_common.c`) compiles for Linux/glibc with gcc 16 and `-DDM_ALARM=1`; the Android code path is
unchanged when `DM_ALARM` is undefined. **No runtime test has been possible**: this machine has no
Vulkan ICD (`/usr/share/vulkan` absent, only Chromium's SwiftShader ICD exists), so surface creation,
swapchain and the copied-pool path below are compile- and review-verified only.

Contract: `docs/ALARM_PORT.md` §2.3. This file is the implementation map for
`nxnative/dm/alarm/` (new files) plus the `#if defined(DM_ALARM)` edits in
`nxnative/dm/android/`.

## 1. What the backend is

| File | Lines | Role |
|---|---|---|
| `dm/android/gfx_vk_core.c` | 1827 | device/instance init, memory pools (zero-copy / copied), submission slots, completion |
| `dm/android/gfx_vk_present.c` | 1176 | window, VkSurfaceKHR, swapchain create/recreate, present thread, overlay, frame pacing |
| `dm/android/gfx_vk_exec.c` | 2871 | command replay: passes, pipelines, descriptors, draws, copies |
| `dm/android/gfx_vk_tex.c` | 1071 | formats, textures, samplers, registration tables |
| `dm/android/gfx_vk_shader.c` | 594 | shader bundle, modules, layouts |
| `dm/android/gfx_vk_dq.c` | 323 | optional replay-recorder thread (`DM_VK_MT_REPLAY`) |
| `dm/android/gfx_vk_subgroup.c` | 143 | SPIR-V subgroup rewriting (no platform deps) |
| `dm/android/gfx_common.c` | 44 | null-backend statistics stubs (no platform deps) |

Only **core.c** and **present.c** touch Android APIs. exec/tex/shader/dq/subgroup/common are
portable C + POSIX and compile unmodified on glibc.

## 2. Android dependency → Linux replacement

Line numbers are post-port (`gfx_vk_core.c`/`present.c` as of this commit); upstream Android-only
files keep upstream numbers.

| # | Android dependency | Where | Linux replacement | Status / phase |
|---|---|---|---|---|
| 1 | `ANativeWindow` handle + `dm_front_window()` | `gfx_vk_present.c:20` (weak NULL), `:168`; Android impl `front_android.c:59` | `dm_front_window()` strong in `dm/alarm/gfx_vk_alarm.c:59` returning the shell's `SDL_Window*`; `compat/android/native_window.h` aliases `ANativeWindow` to `struct SDL_Window` | **done**, compile |
| 2 | `ANativeWindow_getWidth/Height` | `gfx_vk_present.c:171,198,207` (via compat) | `SDL_GetWindowSizeInPixels` in `gfx_vk_alarm.c:57-58`; 0 for minimized/hidden | **done**, compile |
| 3 | `VkAndroidSurfaceCreateInfoKHR` + `vkCreateAndroidSurfaceKHR` | `gfx_vk_present.c:177-184`; declared in `VK_FNS` at `gfx_vk.h:31`; loaded in `core.c:199` | `#if defined(DM_ALARM)` branch calls `vk_alarm_create_surface()` → `SDL_Vulkan_CreateSurface` (`gfx_vk_alarm.c:103`); the Android struct/pointer stay declared for the Android build and are unused on Linux (`compat/android/vulkan_android_compat.h`) | **done**, compile; runtime untested (no ICD) |
| 4 | `VK_KHR_android_surface` instance extension | `gfx_vk_core.c:181` | `vk_alarm_instance_exts()` → `SDL_Vulkan_GetInstanceExtensions` (adds `VK_KHR_surface` + Wayland/X11/… as SDL picks), `gfx_vk_alarm.c:76` | **done**, compile; needs SDL video initialized before `vk_init` |
| 5 | loader load `libvulkan.so` | `gfx_vk_core.c:167` (upstream line) | `#if DM_ALARM`: `dlopen("libvulkan.so.1")` (`core.c:138-148`); custom driver via `VK_ICD_FILENAMES` | **done**, compile |
| 6 | `DM_VK_DRIVER` / `libadrenotools` / `adrenotools_open_libvulkan` | `gfx_vk_core.c:118-171` | guarded out whole under `DM_ALARM` (`#if !defined(DM_ALARM)`), warning logged if the env var is set; Linux picks drivers through the loader's `VK_ICD_FILENAMES` | **done**, compile |
| 7 | `VK_ANDROID_external_memory_android_hardware_buffer`, `VK_EXT_queue_family_foreign` device extensions | `gfx_vk_core.c:213-220` | not requested under `DM_ALARM` (unused without AHB import) | **done**, compile |
| 8 | `AHardwareBuffer_allocate` / `_release` / `_sendHandleToUnixSocket` / `_Desc` | `core.c:499,502,521,632-665,688,785-797,969`; `present.c:1110+` (`vk_image_from_ahb`) | `compat/android/hardware_buffer.h`: **allocate always returns -1** → backend's copied-pool fallback (see §3); the Vulkan AHB import structs are compile-only shims in `compat/android/vulkan_android_compat.h` | **done (copied pools)**, dma-buf = M2.1 |
| 9 | `vkGetAndroidHardwareBufferPropertiesANDROID`, `VkImportAndroidHardwareBufferInfoANDROID` | `core.c:510-513,654-659`; `present.c:1113,1119` | compile-only (paths unreachable: no AHB allocation); `PFN_…` typedefs in `compat/android/vulkan_android_compat.h` | compile-only |
| 10 | `jni.h` + `JNIEXPORT Java_org_nx2a_ExternalActivity_*` menu raster entry points | `present.c:7-9` include, `:527-551` block; `front_android.c:65,222-224` | the two present.c entry points are `#if !defined(DM_ALARM)` (no jvm); front_android.c is not built (SDL front end replaces it) | **done**, compile |
| 11 | `dm_perf_window(ANativeWindow*, fps)` (Android display-rate request) | `present.c:1046,1065`; Android impl `perf_android.c:370` | strong `dm_perf_window` in `gfx_vk_alarm.c:129`: log the request, do nothing (compositor owns the mode) | **done**, compile |
| 12 | `dm_perf_frame` / `dm_perf_frame_cost` (ADPF pacing) | `present.c:1052,1023`; Android impl `perf_android.c:263,349` | weak no-ops in `gfx_vk_alarm.c:136-137`; strong versions may come from `dm/alarm/platform/perf_alarm.c` | **done**, compile |
| 13 | display refresh `VK_GOOGLE_display_timing` (absent on Linux WSI) | `present.c:1147-1155` (`vk_refresh_ms`), `:1158-1164` (`mtl_display_hz`) | `vk_alarm_refresh_hz()` → `SDL_GetCurrentDisplayMode` rate; FIFO present mode does the pacing, desired-present-time pacing is skipped when the extension is absent | **done**, compile; pacing untested |
| 14 | `android/log.h` / `__android_log_print` | `front_android.c:21`, `compat/audiotoolbox_aaudio.c:8`; backend itself logs via `dm.h` `LOG` → stderr | `compat/android/log.h` (stderr shim, compile-only); backend needs no change | **done** |
| 15 | APK assets `AAssetManager` for the overlay theme/font | `overlay_theme.c:8,14-17`, `dm_overlay_font` `:26`, `dm_overlay_theme` `:77`, `dm_overlay_theme_done` `:115`; used by `present.c:726,741` | weak `dm_overlay_font` (built-in Latin-1 `PF_LATIN1`), `dm_overlay_theme` = NULL, `dm_overlay_theme_done` = no-op, in `gfx_vk_alarm.c:141-143`. Overlay draws procedurally, Latin-1 only | **done**, compile; no CJK font without assets |
| 16 | MediaCodec H.264 movie decoding | `movie_avc.c:12,19-94` (`AMediaCodec_*`) | out of the gfx backend: M1 platform layer (`dm/alarm/platform/movie_alarm.c`) fails movies gracefully; ffmpeg is not installed | M3+ |
| 17 | AAudio audio output | `compat/audiotoolbox_aaudio.c:1-8` (`aaudio/AAudio.h`) | M1 platform layer replaces it with SDL3 audio (`dm/alarm/platform/audio_sdl.c`) | done in M1 |
| 18 | EGL/GLES games (`gl_android.c`): `vk_image_from_ahb` imports GL frame buffers | `gl_android.c:31-32,316-360`; `present.c:1110-1129` | no Linux GL path in M2. `vk_image_from_ahb` stays compiled (AHB import unreachable); GL games are a later milestone (needs EGL/GL context on a second window) | M3+ |
| 19 | `ANativeActivity` lifecycle / `onNativeWindowCreated/Destroyed` | `front_android.c:64-69,1266` | SDL front end: `vk_alarm_set_window()`, `vk_alarm_window_lost()` (both `gfx_vk_alarm.c:22,42`); `vk_window_lost()` in `present.c:999` is unchanged and drops swapchain+surface | **done**, compile |
| 20 | `android/hardware_buffer.h`/`native_window.h` includes | `gfx_vk.h:8-9`, `core.c:13`, `present.c:6` | `-Inxnative/dm/alarm/compat` puts `compat/android/*.h` first; Android build (no `-I compat`) keeps the NDK headers | **done** |

Not part of the vk set but listed by the task: MediaCodec/AAudio/EGL above, and `adrenotools`
(row 6) which the Android APK uses to load a bundled Turnip; the Linux equivalent is the distro
ICD (`vulkan-freedreno`/`vulkan-radeon` or `VK_ICD_FILENAMES`).

## 3. Memory pools: decision

**Decision: ship the copied-pool fallback for M2; implement dma-buf heaps as M2.1.** Correctness
first, and it is the smallest change that keeps the Android code untouched.

How the existing fallback works (verified in code):

1. `mtl_pool_add()` (`core.c:804`) is called by the host for every NVN memory pool.
2. `pool_zero_copy()` (`core.c:614`) tries slab → kept buffer → new `AHardwareBuffer_allocate`
   (`core.c:632`). With the shim this returns -1 and the function returns -1 (`core.c:633`); no
   guest pages are touched, no `mmap MAP_FIXED` happens.
3. `pool_add_one()` logs once
   `"[RED] memory pool … could not be zero-copy: a GPU copy is refreshed before each submission (slow)"`
   (`core.c:866`) and calls `pool_copied_alloc()` (`core.c:408`): an ordinary `VkBuffer` with
   host-visible memory and a persistent mapping (`p->shadow`).
4. `vk_pools_sync()` (`core.c:1036`, called from `mtl_sub_begin`, i.e. once per submission) protects
   the guest pages (`nvn_dirty_protect`), copies the 64 KB blocks written since the last sync into
   the buffer, and flushes non-coherent memory. Completion merges GPU-side writes back against a
   baseline (`pools_copy_back`). This is the path the comment in the header describes
   (`core.c:4-5`).
5. `zc_uncached()` (`core.c:781`) also calls `AHardwareBuffer_allocate`; it returns "not uncached"
   and only affects whether pools would have been lazy — irrelevant here.

Consequences / knobs:
- Every guest pool is copied: one full copy per pool on its first sync, then dirty-block copies per
  submission, plus a baseline copy (2× pool memory for baselines) and a GPU-side copy per
  submission. Expect this to dominate performance; it is **correct** and the same code path the
  Android app uses when gralloc refuses a buffer (`DM_VK_ZEROCOPY_MAX_MB`, `DM_VK_NO_ZEROCOPY=1`).
- Because `AHardwareBuffer_allocate` fails, the lazy-zero-copy path (`DM_VK_ZEROCOPY_LAZY`) and the
  slab/reuse caches never engage. Diagnostic env vars still work (`DM_VK_MEM_DIAG=1` reports the
  copied-pool costs).
- `DM_VK_NO_ZEROCOPY=1` is therefore redundant on Linux but harmless; keep using it to make intent
  explicit in scripts.
- The struct fields `ahb`, `slab`, `slot`, `lazy` stay in `vkpool_t` (`gfx_vk.h:69`) and are simply
  unused/zero. No ABI or restructure change; the Android build is unaffected.

Cost estimate (from the Android port's comments, same code): a copied pool is a host-visible
`VkBuffer`; sync copies only dirty 64 KB blocks after the first full copy, so steady-state cost is
proportional to per-frame guest write volume, not pool size — but pool creation costs a full copy and
`baseline` allocates a second copy of every pool (`core.c:1050`).

## 4. M2.1 — dma-buf heap zero-copy path (concrete next steps)

Replace the failing `AHardwareBuffer_allocate` with real dma-buf heaps and import the fd into Vulkan.

1. **Device availability**: check `/dev/dma_heap/system` and `/dev/dma_heap/system-uncached`
   (`doctor.c` already audits `/dev/dma_heap/*`). On this kernel they may be absent; if so keep the
   copied pools (M2) — that is a hard environment limit, not a port bug.
2. **Allocate**: `open("/dev/dma_heap/system", O_RDWR|O_CLOEXEC)`; `struct dma_heap_allocation_data
   { .len, .fd_flags = O_RDWR|O_CLOEXEC, .heap_flags = 0 }`; `ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &d)`
   (`<linux/dma-heap.h>`, `DMA_HEAP_IOCTL_ALLOC`). Keep the returned dma-buf fd as the compat
   "AHardwareBuffer" object (make `AHardwareBuffer` a struct holding `{ int fd; uint64_t size; }`).
3. **Import into Vulkan** (needs these device extensions, checked in `core.c` like the current list):
   `VK_EXT_external_memory_dma_buf`, `VK_KHR_external_memory_fd`, and for buffers
   `VK_KHR_external_memory` (core 1.1). The imported memory is a **buffer** in this backend, so:
   `VkExternalMemoryBufferCreateInfo{handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT}`,
   then `VkImportMemoryFdInfoKHR{handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd}` chained
   under `VkMemoryAllocateInfo` (`vkAllocateMemory`); `vkBindBufferMemory`.
4. **Map over the guest pages**: `mmap(mem, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, dmabuf_fd, 0)`
   after copying the current contents in (the existing `zc_fill`/`zc_place`/`nvn_dirty_swap` machinery
   does exactly this: `core.c:560-640`). This is the only place that needs a new `mmap` source; the
   rest of `pool_zero_copy` already works for a supplied fd (`slab_new` path is the model).
5. **Emit the import**: the cleanest shape is to keep `AHardwareBuffer_*` as the internal interface:
   implement `AHardwareBuffer_allocate` to allocate a dma-buf (step 2) and add two shim functions the
   backend lacks on Android: `dm_ahb_fd(ahb)` and the `VK_EXT_external_memory_dma_buf` import replacing
   `vkGetAndroidHardwareBufferPropertiesANDROID` in `pool_zero_copy` — guard those two call sites
   (`core.c:654-665`) with `#if defined(DM_ALARM)` and use `VkImportMemoryFdInfoKHR` there. Keep the
   Android branch byte-identical.
6. **Also adapt** `pool_fd()` (`core.c:683-693`) to return the dma-buf fd directly, and
   `slab_new()` (`core.c:489-521`) to allocate one dma-buf per slab and `mmap` it at a slot offset.
7. **Sync**: dma-buf heap memory is CPU-coherent with the GPU on the same device; check
   `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` / use `vkFlushMappedMemoryRanges`, and keep
   `vkGetAndroidHardwareBufferPropertiesANDROID`'s two outputs (`allocationSize`, `memoryTypeBits`)
   replaced by `VkMemoryFdPropertiesKHR` (`vkGetMemoryFdPropertiesKHR`, needs
   `VK_KHR_external_memory_fd`).
8. **Validate**: with a Vulkan ICD installed, run with `DM_VK_MEM_DIAG=1` and compare
   "copied-pool baseline allocated" / "could not be zero-copy" counts going to zero; then check
   `pools_copy_back` merge correctness with a renderer test. Do not enable by default until the
   import/mmd path is proven: a wrong `MAP_FIXED` destroys the guest's own memory.

Simplest M2.1 landing order: (a) extension probe + `doctor` reporting, (b) allocate+import+map a single
pool behind `DM_VK_ZC_DMABUF=1`, (c) slabs/reuse, (d) default-on after validation.

## 5. Guarded edits (only these; Android code unchanged when `DM_ALARM` is undefined)

- `nxnative/dm/android/gfx_vk.h` — `VK_USE_PLATFORM_ANDROID_KHR` only when `!DM_ALARM`; under
  `DM_ALARM` also include `compat/android/vulkan_android_compat.h` and `../alarm/gfx_vk_alarm.h`.
- `nxnative/dm/android/gfx_vk_core.c` — `#if !defined(DM_ALARM)` around `drv_has_gpu` and the
  adrenotools/`DM_VK_DRIVER` block; `#if defined(DM_ALARM)` linux loader (`libvulkan.so.1`), instance
  extension list from SDL, and the device-extension `want[]` list without the two Android entries.
  `niext` replaces the hard-coded 3/2 counts so the non-alarm branch is numerically identical.
- `nxnative/dm/android/gfx_vk_present.c` — `jni.h` and the two JNI entry points only when
  `!DM_ALARM`; surface creation via `vk_alarm_create_surface` under `DM_ALARM`; SDL pixel extent,
  minimized/hidden swapchain suppression and `maxImageExtent` clamp under `DM_ALARM`; B8G8R8A8
  surface-format warning; SDL display rate in `vk_refresh_ms`/`mtl_display_hz`.
- `nxnative/dm/android/gfx_common.c` — **no change needed** (compiles as is).
- New files: `nxnative/dm/alarm/gfx_vk_alarm.c/.h`, `compat/android/{hardware_buffer,native_window,
  log,vulkan_android_compat}.h`, `compat/vulkan/vulkan.h`.

## 6. Build integration (`--gfx vk` in `build_host.py`; owner: alarm-host)

Replace `dm/android/gfx_null.c` with the vk set and add the glue + generated headers:

- sources: `dm/android/gfx_vk_core.c gfx_vk_present.c gfx_vk_exec.c gfx_vk_tex.c gfx_vk_shader.c
  gfx_vk_dq.c gfx_vk_subgroup.c dm/alarm/gfx_vk_alarm.c` instead of `dm/android/gfx_null.c`
  (`gfx_common.c` stays).
- include dirs: add `-Inxnative/dm/alarm/compat` (must come first among the compat dirs) so
  `<android/hardware_buffer.h>`, `<android/native_window.h>`, `<vulkan/vulkan.h>` resolve to the shims.
- SDL: `<SDL3/SDL.h>` and `<SDL3/SDL_vulkan.h>` from the system (`pkg-config sdl3` gives no extra
  `-I` on this machine); link `-lSDL3`.
- generated per-game headers: `game_profile.h` (existing) **plus** `android_profile.h`
  (`python3 dm/android/tools/android_profile.py header <game> <gen>/android_profile.h`) **plus** the
  shader SPIR-V headers `dm/android/shaders/*` → `<gen>/<name>_<stage>.spv.h` (`gfx_vk_present.c:14-17`,
  `gfx_vk_exec.c:814-816,2511`). The system `glslc` can generate them exactly as the Android
  `build.py:48-58` does (`glslc --target-env=vulkan1.1 -O <sh> -o …`); the repo ships no prebuilt
  `.spv.h`. Report: if `glslc` is missing on a machine, `--gfx vk` must fail with a clear message
  rather than compile with empty shaders.
- link: no extra libraries beyond the existing `-lSDL3 -lpthread -ldl -lm -lz`; the vk objects resolve
  against the existing host core (checked with `nm`: no unresolved `dm_/mtl_/nvn_/hle_` symbols).

## 7. Compile-check (this session)

Artifact script (not part of the build): `build/alarm/vkcheck/cc.sh`; per-game generated headers in
`build/alarm/vkcheck/gen/` (`game_profile.h`, `android_profile.h`, the 8 shader `.spv.h` files
generated with the system `glslc`). It runs:
`gcc -c <src> -O2 -DDM_ALARM=1 -D_GNU_SOURCE -Wall -Wno-unused-function -Wno-unused-variable
-include nxnative/dm/android/compat/include/darwin_compat.h -Inxnative/dm/android/compat/include
-Inxnative/dm/alarm/compat -Inxnative/dm/alarm/compat/include -Inxnative/dm/host -Inxnative/dm/gfx/nvn
-Inxnative/dm/gfx/nvn/include -Inxnative/dm/gfx/tex -Inxnative/dm/third_party/vulkan-headers/include
-Inxnative/dm/third_party/stb_vorbis -Inxnative/dm/third_party/zstd -Inxnative/dm/third_party/libvpx
-Inxnative/dm/android -Inxnative/dm/android/shaders -I<gen>`.

Result: **9/9 OK** — `gfx_common.c`, `gfx_vk_core.c`, `gfx_vk_present.c`, `gfx_vk_exec.c`,
`gfx_vk_tex.c`, `gfx_vk_shader.c`, `gfx_vk_dq.c`, `gfx_vk_subgroup.c`, `gfx_vk_alarm.c`
(only upstream-style `-Wmisleading-indentation`/`-Wformat` warnings; no errors). The Android branch
(`DM_ALARM` undefined) was syntax-checked separately against the compat headers plus a throwaway
`jni.h` stub: `gfx_vk_core.c` and `gfx_vk_present.c` both rc=0, so the guarded branches still parse.
**Not linked into `nx2alarm-host` this session** — that needs `--gfx vk` in `build_host.py` (owner
alarm-host; the exact source/flag list is §6). Runtime is impossible here (no ICD).

## 8. Untested runtime risks (honest list)

1. **Swapchain format conversion (highest).** Linux WSI usually offers `B8G8R8A8_*` first, not
   `R8G8B8A8_*`. `present_window` blits the game's RGBA8 window texture into the swapchain image with
   `vkCmdBlitImage`, which requires format-compatible images; RGBA8→BGRA8 is **not** compatible.
   The backend logs `[RED] … not format-compatible` (`present.c:190-196`) but the picture may be
   wrong/rejected. Fix (M2.1): route the identity path through a fullscreen-triangle draw (the
   `rot`/`overlay` pipeline already renders into `g_sfmt`) when `g_sfmt` is not RGBA8.
2. **Surface creation** (`SDL_Vulkan_CreateSurface`) is never exercised: requires an SDL window with
   `SDL_WINDOW_VULKAN` and SDL video initialized before `vk_init`; a shell that reuses its GL/
   `SDL_Renderer` window will fail (logged once, backend stays headless).
3. **Instance extensions**: `SDL_Vulkan_GetInstanceExtensions` needs SDL video/`SDL_Vulkan_LoadLibrary`;
   if called too early only `VK_KHR_surface` is enabled and no surface can be created (headless).
4. **Resize**: `swap_make` re-creates the swapchain when SDL's pixel size changes; `OUT_OF_DATE`/
   `SUBOPTIMAL` from acquire/present destroy it and the next present re-makes it. Untested without an
   ICD; window minimize/hidden now yields "no swapchain" instead of a 0×0 swapchain.
5. **Present pacing**: `VK_GOOGLE_display_timing` does not exist on Linux WSI, so `pace_desired`
   returns 0 (no desired-present-times); pacing is FIFO + the game's own frame step. `vk_refresh_ms`/
   `mtl_display_hz` come from the SDL display mode. Untested for frame-time correctness.
6. **Pre-rotation**: `VK_SURFACE_TRANSFORM_ROTATE_90/270` is Android-panel specific; Linux WSI is
   identity, so the rot path is inactive (harmless). Also its blit has the same format issue (risk 1).
7. **Overlay**: no APK assets → no theme atlas and Latin-1-only font; gamepad menu uses the
   procedural path. Icons/text for CJK missing until a Linux asset source exists.
8. **Memory pools**: copied pools are correct but slow; the first-sync full copy + baseline double
   memory can push memory pressure on 8 GB machines (informational log at every 600 submissions).
   A pool larger than RAM would fail allocation (`pool_copied_alloc` VKCHK via caller).
9. **`SDL_Vulkan_DestroySurface`**: `present.c` destroys surfaces with `vkDestroySurfaceKHR`
   (Android path); SDL recommends `SDL_Vulkan_DestroySurface` before `SDL_DestroyWindow`. Works per
   spec, but is a difference to validate when a window is torn down.
10. **No swapchain/queue-family handling for multiple GPUs**: the code uses device index 0 and the
    first graphics+compute queue; on a system with several ICDs/devices this is unchanged from
    Android but untested on Linux (e.g. llvmpipe + freedreno both present).
