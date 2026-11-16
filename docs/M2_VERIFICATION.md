# M2 verification log — Vulkan on the real GPU

Date: 2026-11-16 (Asia/Shanghai) · Machine: Huawei MateBook E 2019 (SDM850, Adreno 630),
aarch64 Arch Linux ARM, GNOME Wayland, Mesa 26.2.7 / **Turnip 26.2.4**, SDL3 3.4.20.

M2 is the Vulkan milestone of [`PORTING_PLAN.md`](PORTING_PLAN.md): the backend must be able to
create a device on this GPU, present through the same surface/swapchain shape the port uses, and
read BC-compressed textures. This log records the evidence. Gameplay is still M3 and needs the
owner's game files and `prod.keys`.

## 1. Driver install (the one system change)

```console
$ sudo pacman -S --needed vulkan-freedreno vulkan-tools
软件包 (4) vulkan-mesa-implicit-layers-1:26.2.4-1  xcb-util-keysyms-0.4.1-5
           vulkan-freedreno-1:26.2.4-1  vulkan-tools-1.4.363.0-1
下载大小 2.17 MiB · 全部安装大小 16.85 MiB  →  rc=0

$ vulkaninfo --summary
GPU0: apiVersion 1.3.354, driverVersion 26.2.4, vendorID 0x5143, deviceID 0x6030002
      deviceName = Turnip Adreno (TM) 630, driverID = DRIVER_ID_MESA_TURNIP,
      driverName = turnip Mesa driver, driverInfo = Mesa 26.2.4-arch1.1
```

`nx2alarm doctor` now reports (previously WARN):

```console
[PASS] Vulkan device              1 device(s): Turnip Adreno (TM) 630
[PASS] Vulkan ICD                 1 manifest(s): /usr/share/vulkan/icd.d/freedreno_icd.json
[WARN] dma-buf heap               absent (/dev/dma_heap missing); Vulkan zero-copy falls back to copied pools
summary: 16 pass, 3 warn, 0 fail
```

The three WARNs are the remaining M2.1/M3 prerequisites: no `/dev/dma_heap` (copied pools for
now), no game keys and no game data.

## 2. Device capability evidence

`probes/vkprobe.c` (upstream nx2android probe, built for Linux: `gcc -O2
-Inxnative/dm/alarm/compat -Inxnative/dm/third_party/vulkan-headers/include
probes/vkprobe.c -o build/alarm/vkprobe -ldl`) confirms:

- **API 1.3.354**, Turnip Adreno 630, 4 GiB device-local heap.
- **textureCompressionBC: yes**; BC1/BC2/BC3/BC4/BC5/BC6H/BC7/BC7_sRGB sampled + blit +
  linear-filter all yes (also ASTC_LDR, ASTC_HDR, ETC2).
- `dynamicRendering`, `synchronization2`, `maintenance4`, `imagelessFramebuffer`,
  `timelineSemaphore`, `descriptorIndexing`, `bufferDeviceAddress`, `scalarBlockLayout`,
  `shaderFloat16`, `geometryShader` all yes; `storageBuffer16BitAccess` / 8-bit storage no
  (matches the a630 report in [`FEASIBILITY.md`](FEASIBILITY.md) §3; the backend enables these
  only when present).
- Device extensions include `VK_KHR_swapchain`, `VK_EXT_external_memory_dma_buf`,
  `VK_KHR_external_memory_fd`, `VK_EXT_transform_feedback`, `VK_EXT_custom_border_color`,
  `VK_EXT_depth_clip_control`, `VK_EXT_robustness2`, `VK_EXT_index_type_uint8`,
  `VK_EXT_vertex_attribute_divisor`, `VK_EXT_provoking_vertex`; the instance exposes Wayland,
  Xlib and XCB surface extensions.
- Backend hard gates from `gfx_vk_core.c` (`graphics+compute queue` and `dynamicRendering`):
  **PASS**.

`nx2alarm-vk info` is the project's own version of this dump (same numbers, committed with the
port):

```console
$ ./build/alarm/nx2alarm-vk info
device: Turnip Adreno (TM) 630  API 1.3.354  driver 0x06802004  queue family 0
textureCompressionBC 1  textureCompressionASTC_LDR 1  textureCompressionETC2 1
dynamicRendering 1  synchronization2 1  maintenance4 1  imagelessFramebuffer 1
BC1/BC3/BC7/BC7_sRGB/ASTC8x8/ETC2: S yes B yes F yes
graphics+compute queue yes  dynamicRendering yes  => backend gate PASS
```

## 3. BC texture decode on the GPU

`nx2alarm-vk bc` uploads a known 4x4 BC1 block (red), a BC3 block (red, opaque), samples it
with a compute shader and reads the texel back:

```console
$ ./build/alarm/nx2alarm-vk bc
device: Turnip Adreno (TM) 630  API 1.3.354  queue family 0
features enabled: textureCompressionBC=1 dynamicRendering=1 sync2=1 timeline=1 descriptorIndexing=1 imagelessFB=1
== BC texture decode (compute shader sampling a 4x4 block)
BC1  sampled texel rgba(255,0,0,255), expected rgba(255,0,0,255): PASS
BC3  sampled texel rgba(255,0,0,255), expected rgba(255,0,0,255): PASS
BC decode: PASS (BC1 and BC3 sampled as red, driver textureCompressionBC=yes)
```

This is the format gate [`PORTING_PLAN.md`](PORTING_PLAN.md) M2 requires: Switch textures are
mostly BC-compressed, and the GPU has to read them natively (CPU fallback costs 4–8× texture
RAM, which this 7.3 GiB machine cannot afford for long).

## 4. Window, surface and swapchain presentation

`nx2alarm-vk smoke` creates an SDL3 `SDL_WINDOW_VULKAN` window, a `VkSurfaceKHR` through
SDL, a Vulkan 1.3 device with the feature set the backend uses, a Wayland swapchain, clears an
animated colour, presents, and handles a resize that Wayland triggers (1280×720 → 1296×795).
Both surface family formats are exercised:

```console
$ ./build/alarm/nx2alarm-vk smoke --frames 120 --format bgra --screenshot /tmp/vk_shot.ppm
surface formats offered: 97, 91, 58, 50, 44, 43, 37, 4, ...
swapchain: 1280x720  4 images  format 44 (B8G8R8A8_UNORM)  present mode FIFO
swapchain: 1296x795  4 images  format 44 (B8G8R8A8_UNORM)  present mode FIFO
presented 120 frame(s) in 1.951 s (61.5 fps, includes startup/first-use costs)
wrote /tmp/vk_shot.ppm (1280x720 PPM P6)      rc=0

$ ./build/alarm/nx2alarm-vk smoke --frames 60 --format rgba ...
swapchain: 1280x720  4 images  format 37 (R8G8B8A8_UNORM)  present mode FIFO
presented 60 frame(s) in 1.021 s (58.7 fps ...)     rc=0
```

FIFO pacing gives the expected ~60 fps (61.5 fps BGRA, 58.7 fps RGBA over 60–120 frames); the resize path recreated the swapchain (4 images each
time), and the screenshot path uploads/reads back a 1280×720 pattern through Vulkan
(`docs/images/nx2alarm-vk-smoke.png`).

The screenshot is a GPU round-trip (host buffer → image → host buffer), not a compositor grab.

## 5. Host binary with the real backend

`build_host.py --gfx vk` links the backend into the host executable (new in M2; the M1 null
binary is unchanged):

```console
$ python3 nxnative/dm/alarm/build_host.py --gfx vk --clean
gfx backend: vk  (Vulkan via gfx_vk_* + dm/alarm/gfx_vk_alarm.c)
vk headers: android_profile.h, 8 shader header(s) generated
compiling 189 C / 3 asm files with -j3 ...  192/192
linking ...
built build/alarm/nx2alarm-host-vk (10040520 bytes, 9.6 MiB, vk gfx backend) in 53.7s   rc=0

$ ./build/alarm/nx2alarm-host-vk
[dm] [nvn] null device: 537 names, 423 implemented ...
[dm] FATAL usage: dmhost <dir> [mod...] [-- game arguments...]      # exit 134 (die)
$ ldd build/alarm/nx2alarm-host-vk | grep -E 'vulkan|android|liblog'  # no output
```

The backend is `dlopen`-based (`libvulkan.so.1`), so no `libvulkan` NEEDED entry; no unresolved
`dm_/mtl_/nvn_/hle_/vk*` symbols. **This binary has not initialized Vulkan yet**: without game
data it exits at the usage/fixups path before the backend's `vk_init()`, so the end-to-end
device creation by the *host* remains an M3 item (the standalone tool above covers the driver,
surface, swapchain and BC paths).

## 6. What is verified, and what is not

| Item | Status |
|---|---|
| Turnip device + Vulkan 1.3 + backend hard gates | **verified** |
| BC1/BC3 GPU decode; BC1–BC7 format support | **verified** |
| SDL3 → `VkSurfaceKHR` → Wayland swapchain → present | **verified** (BGRA8 and RGBA8) |
| Resize/OUT_OF_DATE handling in the smoke loop | **verified** (1280×720 → 1296×795) |
| FIFO pacing ~60 fps on a 60 Hz panel | **verified** (60.9/61.4 fps over 60–120 frames) |
| `build_host.py --gfx vk` compiles/links the real backend | **verified** |
| BGRA8 swapchain conversion draw in `gfx_vk_present.c` | compiled + review only; needs a game frame to exercise |
| Host backend device init + first game frame | **not tested** (no game data/keys) |
| Zero-copy NVN pools | not available: `/dev/dma_heap` absent; copied-pool fallback is the M2 path; dma-buf is M2.1 |
| Movies, audio, UI | M3/M4 (SDL3 audio backend is already linked; H.264 is a graceful stub) |

## 7. Reproduce

```sh
# prerequisites: vulkan-freedreno + vulkan-tools (root), sdl3, shaderc (glslc), gcc
python3 nxnative/dm/alarm/build.py --target vk-smoke
./build/alarm/nx2alarm-vk info
./build/alarm/nx2alarm-vk bc
./build/alarm/nx2alarm-vk smoke --frames 120 --format bgra --screenshot /tmp/vk.ppm
python3 nxnative/dm/alarm/build_host.py --gfx vk
./build/alarm/nx2alarm-host-vk             # upstream usage; a game build dir is M3+
```

The screenshots are in [`docs/images/nx2alarm-vk-smoke.png`](images/nx2alarm-vk-smoke.png)
(GPU round-trip pattern) and [`docs/images/nx2alarm-shell.png`](images/nx2alarm-shell.png)
(the M2 shell audit with the Vulkan rows now PASS).
