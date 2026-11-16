# nx2alarm — Vulkan backend port map (M2)

Status: **M2 compile + device probe done, M2b conversion path implemented** (2026-11-16). The whole
backend (`dm/android/gfx_vk_*.c`, `gfx_common.c`) compiles for Linux/glibc with gcc 16 and
`-DDM_ALARM=1`; `build_host.py --gfx vk` links it as `build/alarm/nx2alarm-host-vk`, and the Android
code path is unchanged when `DM_ALARM` is undefined. The device is real and measured (§1.1: Turnip
Adreno 630, Vulkan 1.3.354, BC1–BC7). Surface/swapchain/present and the copied-pool path are
**not yet exercised end-to-end**: there is no game data on this machine (the host dies at the
usage/fixups stage before creating a device), and the agent shell this document was written in runs
inside a sandbox whose private `/dev` hides `/dev/dri` (the probe binary there prints "no physical
device"). `nx2alarm-vk smoke` (Lead's standalone tool) proves the same surface/swapchain shape on
this device with its own present loop; it does **not** call `gfx_vk_present.c`, so the backend's
present/conversion path is first exercised by a game boot (M3) or a targeted harness. §5b lists what
that run's log should show.

Contract: `docs/ALARM_PORT.md` §2.3. This file is the implementation map for
`nxnative/dm/alarm/` (new files) plus the `#if defined(DM_ALARM)` edits in
`nxnative/dm/android/`.

## 1. What the backend is

| File | Lines | Role |
|---|---|---|
| `dm/android/gfx_vk_core.c` | 1827 | device/instance init, memory pools (zero-copy / copied), submission slots, completion |
| `dm/android/gfx_vk_present.c` | 1238 | window, VkSurfaceKHR, swapchain create/recreate, present thread, overlay, frame pacing |
| `dm/android/gfx_vk_exec.c` | 2871 | command replay: passes, pipelines, descriptors, draws, copies |
| `dm/android/gfx_vk_tex.c` | 1071 | formats, textures, samplers, registration tables |
| `dm/android/gfx_vk_shader.c` | 594 | shader bundle, modules, layouts |
| `dm/android/gfx_vk_dq.c` | 323 | optional replay-recorder thread (`DM_VK_MT_REPLAY`) |
| `dm/android/gfx_vk_subgroup.c` | 143 | SPIR-V subgroup rewriting (no platform deps) |
| `dm/android/gfx_common.c` | 44 | null-backend statistics stubs (no platform deps) |

Only **core.c** and **present.c** touch Android APIs. exec/tex/shader/dq/subgroup/common are
portable C + POSIX and compile unmodified on glibc.

## 1.1 Measured device facts — VERIFIED (probe `build/alarm/vkprobe`, 2026-11-16)

| Fact | Measured value | Consequence for the port |
|---|---|---|
| GPU / driver | Turnip Adreno (TM) 630, vendor 0x5143 device 0x6030002, **Vulkan 1.3.354**, Mesa 26.2.4 | the backend's `VK_API_VERSION_1_3` request is met; no promoted-extension fallback needed |
| Required 1.3 features | `dynamicRendering`, `synchronization2`, `timelineSemaphore`, `descriptorIndexing`, `imagelessFramebuffer` = **yes** | `vk_init`'s feature chain and `rot_setup()`'s dynamic-rendering pipeline are satisfied |
| Texture compression | **BC1–BC7: sampled + blit + linear filter = yes**; ASTC_LDR yes; ETC2 yes | NVN BC textures replay natively; the CPU `bc_decode` fallback (`DM_VK_X_BC_CPU`) is not needed — the BC risk in `docs/FEASIBILITY.md` is resolved for this GPU/driver |
| Subgroups | size 128, `VK_EXT_subgroup_size_control` yes | `spv_one_lane` / `DM_VK_SUBGROUP_PIN` paths available |
| Memory | heap 0: 4096 MB device-local; types DEVICE_LOCAL+HOST_VISIBLE (HOST_COHERENT and HOST_CACHED) and DEVICE_LOCAL LAZY | copied pools (§3) get host-visible memory; no host import of guest pages |
| External memory | `VK_EXT_external_memory_dma_buf` **yes**, `VK_KHR_external_memory_fd` **yes**; `VK_ANDROID_external_memory_android_hardware_buffer` **no** (expected on Linux); `VK_EXT_external_memory_host` **no** | AHB import stays compiled out (§3); dma-buf import is possible once a dma-buf **source** exists (§4) |
| dma-buf heaps | **no `/dev/dma_heap`** on this kernel | M2.1's dma-buf-heap path cannot run here: copied pools remain the M2 path (§4 lists the alternatives) |
| Instance surface extensions | **wayland yes, xlib yes, xcb yes** (`nx2alarm-vk info`) | `vk_alarm_instance_exts()` (SDL) picks the window system's at run time; Wayland is what this GNOME session uses |
| Surface formats | not enumerated by the probe (it has no window): Wayland WSI through Turnip is expected to offer the **B8G8R8A8** family (UNORM and/or SRGB), **not** R8G8B8A8 | exactly what §5b's conversion present path handles; `swap_make` logs `swapchain %ux%u format %d` and whether the conversion pipeline is used — read those first on the first backend run |
| Limits | maxMemoryAllocationCount 4294967295, maxStorageBufferRange 128 MB, maxPushConstants 256 | no backend limit hit |

## 2. Android dependency → Linux replacement

Line numbers are post-port (`gfx_vk_core.c`/`present.c` as of this commit); upstream Android-only
files keep upstream numbers.

| # | Android dependency | Where | Linux replacement | Status / phase |
|---|---|---|---|---|
| 1 | `ANativeWindow` handle + `dm_front_window()` | `gfx_vk_present.c:20` (weak NULL), `:168`; Android impl `front_android.c:59` | `dm_front_window()` strong in `dm/alarm/gfx_vk_alarm.c:59` returning the shell's `SDL_Window*`; `compat/android/native_window.h` aliases `ANativeWindow` to `struct SDL_Window` | **done**, compile |
| 2 | `ANativeWindow_getWidth/Height` | `gfx_vk_present.c:171,198,207` (via compat) | `SDL_GetWindowSizeInPixels` in `gfx_vk_alarm.c:57-58`; 0 for minimized/hidden | **done**, compile |
| 3 | `VkAndroidSurfaceCreateInfoKHR` + `vkCreateAndroidSurfaceKHR` | `gfx_vk_present.c:177-184`; declared in `VK_FNS` at `gfx_vk.h:31`; loaded in `core.c:199` | `#if defined(DM_ALARM)` branch calls `vk_alarm_create_surface()` → `SDL_Vulkan_CreateSurface` (`gfx_vk_alarm.c:103`); the Android struct/pointer stay declared for the Android build and are unused on Linux (`compat/android/vulkan_android_compat.h`) | **done**, compile; first runtime = vk-smoke (§5b) |
| 4 | `VK_KHR_android_surface` instance extension | `gfx_vk_core.c:181` | `vk_alarm_instance_exts()` → `SDL_Vulkan_GetInstanceExtensions` (adds `VK_KHR_surface` + Wayland/X11/… as SDL picks), `gfx_vk_alarm.c:76` | **done**, compile; needs SDL video initialized before `vk_init` |
| 5 | loader load `libvulkan.so` | `gfx_vk_core.c:167` (upstream line) | `#if DM_ALARM`: `dlopen("libvulkan.so.1")` (`core.c:138-148`); custom driver via `VK_ICD_FILENAMES` | **done**, compile |
| 6 | `DM_VK_DRIVER` / `libadrenotools` / `adrenotools_open_libvulkan` | `gfx_vk_core.c:118-171` | guarded out whole under `DM_ALARM` (`#if !defined(DM_ALARM)`), warning logged if the env var is set; Linux picks drivers through the loader's `VK_ICD_FILENAMES` | **done**, compile |
| 7 | `VK_ANDROID_external_memory_android_hardware_buffer`, `VK_EXT_queue_family_foreign` device extensions | `gfx_vk_core.c:213-220` | not requested under `DM_ALARM` (unused without AHB import) | **done**, compile |
| 8 | `AHardwareBuffer_allocate` / `_release` / `_sendHandleToUnixSocket` / `_Desc` | `core.c:499,502,521,632-665,688,785-797,969`; `present.c:1110+` (`vk_image_from_ahb`) | `compat/android/hardware_buffer.h`: **allocate always returns -1** → backend's copied-pool fallback (see §3); the Vulkan AHB import structs are compile-only shims in `compat/android/vulkan_android_compat.h` | **done (copied pools)**; dma-buf import extensions VERIFIED present, no heap device — M2.1 (§4) |
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

**Measured on this device (§1.1):** the import side is ready — `VK_EXT_external_memory_dma_buf` and
`VK_KHR_external_memory_fd` are both advertised — but the **allocation side is missing**:
`/dev/dma_heap` does not exist on this kernel, so there is no DMA_HEAP_IOCTL_ALLOC to call. The copied
pools of §3 stay the M2 path until a dma-buf source exists. Options, in order of effort: (a) a kernel
with `CONFIG_DMABUF_HEAPS` (then steps 1–8 below run unchanged); (b) `udmabuf` (`/dev/udmabuf`) over a
memfd, if enabled; (c) export a dma-buf from another allocator (DRM/GBM or a Vulkan allocation whose
memory supports export) and import it with the same `VkImportMemoryFdInfoKHR` path; (d) accept the
copied pools (correct, slower). Only (a)–(c) make `pool_zero_copy()` reach its zero-copy branch.

1. **Device availability**: check `/dev/dma_heap/system` and `/dev/dma_heap/system-uncached`
   (`doctor.c` already audits `/dev/dma_heap/*`). On this kernel they are absent; keep the copied
   pools (M2) — a hard environment limit, not a port bug.
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
  minimized/hidden swapchain suppression and `maxImageExtent` clamp under `DM_ALARM`; swapchain format
  selection that keeps the window texture's sRGB-ness in the B8G8R8A8 family; the **conversion present
  path** (§5b); SDL display rate in `vk_refresh_ms`/`mtl_display_hz`.
- `nxnative/dm/android/gfx_common.c` — **no change needed** (compiles as is).
- New files: `nxnative/dm/alarm/gfx_vk_alarm.c/.h`, `compat/android/{hardware_buffer,native_window,
  log,vulkan_android_compat}.h`, `compat/vulkan/vulkan.h`.

## 5b. Conversion present path (M2b, implemented; compile-verified, runtime-untested)

The identity present used to blit the game's `R8G8B8A8_*` window texture into the swapchain image with
`vkCmdBlitImage`, which requires **format-compatible** images; Linux WSI (Wayland through Turnip)
offers the `B8G8R8A8_*` family, so RGBA8→BGRA8 is not a legal blit. Behind `#if defined(DM_ALARM)`
the backend now draws the picture instead, with the pipeline it already had for Android pre-rotation
(`shaders/rot.vert` + `rot.frag`: a fullscreen triangle with an affine uv mapping, no new shaders and
no new header list).

- **Format selection** (`present.c:197-210`, `DM_ALARM` only): prefer the exact `g_want` (the window
  texture's sRGB-ness, `R8G8B8A8_SRGB`/`_UNORM`), else the same sRGB-ness in `B8G8R8A8_*`; otherwise
  `sf[0]` and one `[RED]` line. Choosing the same sRGB-ness keeps the draw's sample-decode /
  render-target-encode round trip neutral, so the game's bytes reach the display as the Android blit
  copied them. Component order is irrelevant to a sampling draw.
- **Setup** (`swap_make`, `present.c:230-236`): `g_conv.on = <format not R8G8B8A8> && V.has_dynrender
  && rot_setup()`. `rot_setup()` creates the descriptor layout/pool/sampler and the pipeline that
  renders into `g_sfmt` (it also creates its pre-rotation image; unused here). If the pipeline cannot
  be made, `[RED]` 96 and the old (incompatible) blit is attempted. `swap_destroy()` clears `g_conv.on`
  (`present.c:113`) so a re-created swapchain decides again.
- **Draw** (`present_window`, `present.c:938-981`): after the black clear in `TRANSFER_DST_OPTIMAL`
  (the window's borders), `barrier(tgt, TRANSFER_DST -> COLOR_ATTACHMENT_OPTIMAL)`, one dynamic-rendering
  pass (`LOAD`/`STORE`) at the picture's rect, `vkCmdBindPipeline(rot)`, viewport = `(dx,dy,dw,dh)` (the
  aspect-fit rect, so the borders stay black), push constants
  `m = (cw/im->w, 0, ±ch/im->h, 0)`, `t = (cx/im->w, (cy or cy+ch)/im->h)` — the `±` and the `t.y` pick
  are the `vk_window_bottom_up()` flip that the blit did with `srcOffsets`; then `vkCmdDraw(3)`.
  The target stays in `COLOR_ATTACHMENT_OPTIMAL` for the existing overlay pass and goes to
  `PRESENT_SRC_KHR` at the end exactly as the blit path did.
- **Descriptors**: 8 sets (one per present slot) from a pool created on the first conversion present
  (`present.c:941-953`), sampling `im->full` in `VK_IMAGE_LAYOUT_GENERAL` (version 1 keeps images in
  GENERAL) with a `VK_FILTER_LINEAR` sampler of its own (the pre-rotation sampler is nearest because
  that path copies 1:1; the conversion scales). The set for slot `k` is re-written before recording;
  its previous use was the submit whose fence is waited for at the top of `present_window`, so this is
  safe for the 8-deep ring.
- **Android path**: every changed line is inside `#if defined(DM_ALARM)`; when `DM_ALARM` is undefined
  the same operations run in the same order (the blit block is only wrapped in a block, `bu` replaces
  the direct `vk_window_bottom_up()` call).

**What the first backend run should show** (game boot in M3, or a harness that drives
`mtl_present`): no `[RED]` lines 95/96/97; `swapchain WxH format 44/50 …` for
B8G8R8A8_UNORM(44)/B8G8R8A8_SRGB(50) together with *"the picture is drawn through the conversion
pipeline"* when the format is not R8G8B8A8; a correctly scaled, correctly coloured (and, for a
bottom-up frame, correctly oriented) picture with black borders; `DM_VK_SHOT` PPM output (`shot()`,
which reads the game image directly) unaffected.
**Not verified here**: `nx2alarm-vk smoke` has its own swapchain loop and does not call
`present_window`; this agent shell cannot open `/dev/dri` (private `/dev`), and there is no game data
— so the conversion path is compile- and review-verified, not runtime-verified.

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

Artifact script (committed, not part of the build): `nxnative/dm/alarm/check_vk.sh`; per-game generated headers in
`build/alarm/vkcheck/gen/` (`game_profile.h`, `android_profile.h`, the 8 shader `.spv.h` files
generated with the system `glslc`). It runs:
`gcc -c <src> -O2 -DDM_ALARM=1 -D_GNU_SOURCE -Wall -Wno-unused-function -Wno-unused-variable
-include nxnative/dm/android/compat/include/darwin_compat.h -Inxnative/dm/android/compat/include
-Inxnative/dm/alarm/compat -Inxnative/dm/alarm/compat/include -Inxnative/dm/host -Inxnative/dm/gfx/nvn
-Inxnative/dm/gfx/nvn/include -Inxnative/dm/gfx/tex -Inxnative/dm/third_party/vulkan-headers/include
-Inxnative/dm/third_party/stb_vorbis -Inxnative/dm/third_party/zstd -Inxnative/dm/third_party/libvpx
-Inxnative/dm/android -Inxnative/dm/android/shaders -I<gen>`.

Result after M2b: **9/9 OK** — `gfx_common.c`, `gfx_vk_core.c`, `gfx_vk_present.c`,
`gfx_vk_exec.c`, `gfx_vk_tex.c`, `gfx_vk_shader.c`, `gfx_vk_dq.c`, `gfx_vk_subgroup.c`,
`gfx_vk_alarm.c` (only upstream-style warnings; no errors). The Android branch (`DM_ALARM` undefined)
was syntax-checked separately against the compat headers plus a throwaway `jni.h` stub:
`gfx_vk_core.c` and `gfx_vk_present.c` both rc=0, so the guarded branches still parse.
`build_host.py --gfx vk` (§6, owner alarm-host) now compiles and links the same slice as
`build/alarm/nx2alarm-host-vk`: no `NEEDED` libvulkan (the loader is `dlopen`ed at run time), no
Android libraries, no unresolved `dm_/mtl_/nvn_/hle_/vk*` symbols.
**Runtime remains unexercised in this environment**: the host binary stops at its usage/fixups path
without game data, and this document's agent shell runs under a sandbox whose private `/dev` hides
`/dev/dri` (re-running `build/alarm/vkprobe` there prints "no physical device"). `nx2alarm-vk
smoke` (Lead's standalone tool, its own swapchain loop) proves the surface/swapchain shape on this
device but does not call `gfx_vk_present.c`; the first run that does is a game boot (M3) or a
targeted harness (§5b).

## 8. Runtime risks (honest list)

The facts in §1.1 are verified on the real GPU (BC1–BC7 replay, the Vulkan 1.3 features the backend
needs, dma-buf import extensions present, no dma-buf heap). Everything below is **not yet exercised
end-to-end**: there is no game data on this machine, and the agent shell that wrote this cannot
open `/dev/dri`; `nx2alarm-vk smoke` (own swapchain, not this backend) is the closest runtime
evidence so far.

1. **Conversion present path (§5b) — new, compile-verified only.** The rot pipeline was previously
   only used by the Android pre-rotation path. To watch on the first backend run: the push-constant uv mapping
   (aspect-fit rect, bottom-up flip), the black borders (the clear runs before the draw), the
   sample-decode/write-encode round trip for the chosen sRGB-ness (when the surface offers only the
   *opposite* sRGB-ness, `[RED]` 95 logs and colours are off — the shader cannot fix that), and the
   per-slot descriptor update (safe only because `present_window` waits for slot `k`'s fence at the
   top; keep that invariant if the ring depth changes).
2. **Surface creation** (`SDL_Vulkan_CreateSurface`) is still untested here: it needs an SDL window
   created with `SDL_WINDOW_VULKAN` and SDL video initialized before `vk_init`; a shell reusing its
   GL/`SDL_Renderer` window fails (logged once, backend stays headless).
3. **Instance extensions**: `SDL_Vulkan_GetInstanceExtensions` needs SDL video / `SDL_Vulkan_LoadLibrary`;
   if called too early only `VK_KHR_surface` is enabled and no surface can be created (headless).
4. **Resize**: `swap_make` re-creates the swapchain when SDL's pixel size changes; `OUT_OF_DATE` /
   `SUBOPTIMAL` destroy it and the next present re-makes it; a minimized/hidden window yields "no
   swapchain" instead of a 0×0 one. A smoke run at one window size does not exercise this.
5. **Present pacing**: there is no `VK_GOOGLE_display_timing` on Linux WSI, so desired-present-time
   pacing is off (FIFO + the game's frame step); `vk_refresh_ms`/`mtl_display_hz` come from the SDL
   display mode. Frame-time correctness is untested.
6. **Pre-rotation**: `VK_SURFACE_TRANSFORM_ROTATE_90/270` is Android-panel specific, so Linux WSI is
   identity and that path stays inactive — but §5b now reuses its pipeline and descriptor-layout
   objects. Conversion keeps its own descriptor sets, `rot` keeps `g_rot.set`; if a pre-rotated Linux
   surface ever appears, re-check that split.
7. **Overlay**: no APK assets → no theme atlas, Latin-1-only font (procedural gamepad). The overlay
   draws into the same `COLOR_ATTACHMENT_OPTIMAL` target right after the conversion draw, so it should
   compose; only a real run can confirm.
8. **Memory pools**: copied pools are correct but slow (full copy per pool + a baseline copy, dirty
   64 KB block sync per submission). Watch RSS with a real game on this 8 GB device; a failed
   `pool_copied_alloc` aborts through the caller's `VKCHK`.
9. **Surface teardown**: `present.c` uses `vkDestroySurfaceKHR` (SDL recommends `SDL_Vulkan_DestroySurface`
   before `SDL_DestroyWindow`); legal per spec, but validate on window close.
10. **Device/queue selection**: device 0 and the first graphics+compute queue; unverified if a software
    ICD (llvmpipe) is also installed.
11. **dma-buf heaps**: the import extensions are present (§1.1) but `/dev/dma_heap` is not, so §4's
    M2.1 path cannot be validated on this kernel; copied pools remain the only path.
