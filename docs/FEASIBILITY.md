# Device feasibility audit — nx2alarm on the Huawei MateBook E 2019

Status: **M1d deliverable, 2026-11-16.** Question: can nx2alarm run on *this* machine — an
Arch Linux ARM tablet with a 2018 Snapdragon 850 / Adreno 630 — and what must be fixed first?

Evidence is from this machine unless marked otherwise: `/proc`, `/sys`, `/etc`,
`pacman -Q`/`-Si` (read-only; nothing was installed), `wayland-info`, `pactl`, `journalctl -k`.
All paths in §2 are relative to the repository root.

**Inspection caveat:** the agent shell runs inside a `bwrap` sandbox whose `/dev` is a minimal
tmpfs (`mount`: `tmpfs on /dev`), so `/dev/dri`, `/dev/dma_heap` and `/dev/input` were not
readable here. GPU, display, audio and input facts below come from `/sys`, `/proc`, the kernel
journal and the running GNOME session; device-node *probes* (vulkaninfo, SDL3) were not run. This
does not change the conclusions, but it is why several verdicts say "likely / verify at M2".

## 1. Device profile (verified)

| Fact | Verified value | Command / source |
|---|---|---|
| Model | HUAWEI MateBook E 2019, DMI `PAK-AL09` / `M1010` / `PAK-AL09-PCB`, BIOS 1.15 | `cat /sys/class/dmi/id/{sys_vendor,product_name,product_version,board_name,bios_version}` |
| Device tree | `Huawei MateBook E 2019`, `huawei,planck` + `qcom,sdm845` | `cat /proc/device-tree/model /proc/device-tree/compatible` |
| SoC / CPU | Snapdragon 850; 4× Kryo 385 Gold (Cortex-A75, part `0x803`) max **2,841,600 kHz**, 4× Kryo 385 Silver (A55, `0x802`) max **1,766,400 kHz**; 8 cores, `schedutil` | `/proc/cpuinfo`, `/sys/.../cpufreq/cpuinfo_max_freq`, `cpu*/cpufreq/scaling_governor` |
| GPU | Adreno 630 (`qcom,adreno-630.2`), bound to the in-kernel `adreno` driver (part of the `msm` module); firmware `a630_sqe.fw`/`a630_gmu.bin` loaded; devfreq **257–710 MHz**, `simple_ondemand`; DRM render node `renderD128` | `readlink /sys/devices/platform/soc@0/5000000.gpu/driver`, `journalctl -k`, `/sys/class/devfreq/5000000.gpu/*` |
| Display | eDP-1 `CMN P120ZDG-BF1`, **2160×1440 @ 60.000 Hz** (scale 2, logical 1296×864) | `/sys/class/drm/card0-eDP-1/modes`, `wayland-info` |
| Kernel | **7.3.4-g7595a62d45ee**, custom build (`gcc 13.3.0`, `#1 SMP PREEMPT`) | `uname -a`, `journalctl -k` |
| OS / desktop | Arch Linux ARM (rolling), GNOME Shell 51.1, **Wayland** (`wayland-0`, Xwayland `DISPLAY=:0`) | `/etc/os-release`, `gnome-shell --version`, session env |
| RAM | **7,679,296 kB = 7.32 GiB**, **no swap**; `MemAvailable` 2.7–3.4 GiB with GNOME running | `/proc/meminfo`, `/proc/swaps` |
| Storage | UFS 225 GiB root (`sda`, **203 GiB free**); microSD 58.2 GiB (`mmcblk0`, 51 GiB free) | `df -h`, `lsblk -o NAME,SIZE,ROTA,MODEL` |
| Audio | sdm845 card `HUAWEI-PAK_AL09-M1040`; PipeWire 1.6.11, default sink `HiFi__Speaker__sink` | `/proc/asound/cards`, `pactl info` |
| Input | Goodix `27C6:0118` touchscreen + stylus + HID keyboard, power/volume keys; **no gamepad, no touchpad** | `/proc/bus/input/devices` |
| Bluetooth / USB | `hci0` present but **rfkill soft=1**; 4 xHCI root hubs, no USB devices attached | `/sys/class/rfkill/*`, `/sys/bus/usb/devices` |
| Power / USB-C | `/sys/class/power_supply` **empty** (no battery); `mem_sleep` = `[s2idle]` only; no `/sys/class/typec` | `/sys/class/power_supply`, `/sys/power/mem_sleep` |
| Vulkan user space | loader `vulkan-icd-loader 1.4.363.0-1` installed; **no system ICD** (`/usr/share/vulkan` absent); only Chromium's bundled SwiftShader ICD JSON exists (not in the loader's search path); no `vulkaninfo` | `pacman -Q`, `ls /usr/share/vulkan`, `find / -name '*_icd*.json'` |
| Mesa / SDL | Mesa **26.2.7** (freedreno GL works — GNOME has a "high priority EGL context"); SDL3 **3.4.20** with wayland/pipewire/pulseaudio/alsa/evdev backends compiled in | `pacman -Q mesa sdl3`, `journalctl --user`, `strings /usr/lib/libSDL3.so` |
| Toolchain | gcc 16.1.1, make, ninja, meson, python 3.14.10; **no clang, cmake, .NET SDK or Java** | `command -v`, `pacman -Q gcc` |
| DMA-BUF heaps | kernel `CONFIG_DMABUF_HEAPS=y`, `SYSTEM=m`, `CMA=m`; `system_heap.ko` / `cma_heap.ko` exist but are **not loaded**, `CONFIG_UDMABUF=n`; no `/dev/dma_heap` (the node appears only when the heap module registers) | `zcat /proc/config.gz`, `find /lib/modules/... -name '*heap*'`, `/proc/modules` |

## 2. What the runtime requires (from nx2alarm/nx2android code and docs)

| Requirement | Needed? | Evidence |
|---|---|---|
| Native aarch64 execution of the game's own NSO/NRO code, ELF loader, arm64 relocations | **required** | `nxnative/dm/host/loader.c:1-2,299-365`; `docs/ALARM_ARCHITECTURE.md:28` |
| Guest TLS slots in bionic's `TLS_SLOT_APP` (`tpidr_el0[2]`) | **required, Linux-sensitive** | `loader.c:47-70` — on glibc this slot is not "app-reserved"; a collision aborts with a diagnostic (`:68`) |
| Guest address space: one reserved block, typically **4 GiB or 5 GiB** (`mem_reserve`), partitioned alias/stack/heap | **required** | `hle_svc.c:13-18,48-56`; game profiles: 42×`0x100000000` (4 GiB), 34×`0x140000000` (5 GiB), long tail 1–29 GiB (a few VA-heavy profiles) (`dm/games/*/game.tsv`) — mostly **virtual** space (`PROT_NONE`, `hle_svc.c:13`) |
| Application memory pool **3285 MiB** (`app_pool 0xCD500000`) — this is real RAM the game's allocator will use | **required** | `hle_svc.c:67`; `dm/games/profile_guess.py:29`; keys documented in `dm/games/README.md:46` |
| Vulkan instance requested at **`VK_API_VERSION_1_3`**; loader `libvulkan.so` | **required** | `gfx_vk_core.c:121,128,157-162` (`apiVersion 1.3`; the `DM_VK_DRIVER` path even requires a device reporting ≥1.3) |
| Instance extensions: `VK_KHR_surface` + a platform surface (Android now; **Wayland on Linux**) + optional `VK_EXT_debug_utils` | **required** | `gfx_vk_core.c:161`; Linux replacement `VK_KHR_wayland_surface` per `docs/PORTING_PLAN.md:56-58` |
| Device extensions wanted: `VK_KHR_swapchain`, `VK_ANDROID_external_memory_android_hardware_buffer` (Android-only), `VK_EXT_queue_family_foreign`, `VK_EXT_custom_border_color`, `VK_EXT_robustness2`, `VK_EXT_depth_clip_control`, `VK_EXT_provoking_vertex`, `VK_EXT_index_type_uint8`, `VK_EXT_transform_feedback`, `VK_GOOGLE_display_timing`, vertex-attribute-divisor (EXT or KHR) | mostly optional; **swapchain required** | `gfx_vk_core.c:192-200` — each missing one is logged, not fatal |
| **Hard device gates:** graphics+compute queue; **dynamic rendering OR imageless framebuffers + render pass 2** | **required** | `gfx_vk_core.c:186-189,251` (`die()` if neither) |
| Timeline semaphores | optional (fence ring fallback) | `gfx_vk_core.c:241-245`; also guards against a reported-but-unloadable entry point (`:319-322`) |
| Feature chain enabled if present: 1.1/1.2/1.3 core features (16/8-bit storage, multiview, variable pointers, timeline, **descriptor indexing**, scalar block layout, uniformity, memory model, buffer device address, sync2, dynamic rendering, maintenance4, …) | enabled when supported | `gfx_vk_core.c:217-239,296-311` |
| BC-compressed textures read natively (upstream device requirement) | **recommended**, not fatal | `docs/UPSTREAM_README_nx2android.md:14` (Adreno/Xclipse yes; Mali/PowerVR no); fallback decodes BC on the CPU at **4–8× memory** (`gfx_vk_tex.c:120-122`) |
| Memory pools: Android `AHardwareBuffer` zero-copy → Linux `/dev/dma_heap` + `VK_EXT_external_memory_dma_buf`, or existing **copied-pool fallback** | zero-copy optional | `gfx_vk_core.c:2-4,599-630`; replace per `docs/ALARM_ARCHITECTURE.md:42`; copy fallback `DM_VK_NO_ZEROCOPY` (`gfx_vk_core.c:771-774`) and costs one extra byte of RAM per copied pool byte (`docs/ARCHITECTURE.md:94`) |
| Presentation: SDL3 window → `SDL_Vulkan_CreateSurface`, swapchain (FIFO), resize | **required** | `docs/ALARM_ARCHITECTURE.md:41`; `docs/PORTING_PLAN.md:56-58`; present path today `gfx_vk_present.c:2,166-203` |
| Audio: `hle_audio.c` unchanged, SDL3 audio backend instead of AAudio | required for audio | `docs/ALARM_ARCHITECTURE.md:43` |
| Input: SDL3 gamepad/touch/keyboard, `dm_input.h`, `front_run()` | required for play | `docs/ALARM_ARCHITECTURE.md:40,65-68` |
| OS services replaced by HLE: `nn::fs` → host FS with transactional saves | required | `hle_fs.c:1-5`; save commits use macOS `clonefile` + `RENAME_SWAP` (`hle_fs.c:34-37`) — must be reimplemented on Linux |
| Data layout: `mods/`, `RomFS/`, `aoc/`, `sysdata/`, `shaders.dmsv`, `env.txt` | required at M3 | `docs/ALARM_ARCHITECTURE.md:85-96`; compressed RomFS can be ~30 GB (`docs/ARCHITECTURE.md:267`) |

## 3. Feasibility matrix

Verdict scale: **works now** / **works after installing packages** / **likely** / **unknown** /
**blocker**.

| Requirement | This device | Verdict | Evidence |
|---|---|---|---|
| aarch64 Linux userspace, glibc, gcc build | 8× arm64, gcc 16.1.1, glibc 2.43 | **works now** (M1 build) | `uname -m`, `pacman -Q gcc glibc` |
| GPU kernel driver + DRM render node | `msm`/`adreno` loaded, a630 firmware, `renderD128`, GL works | **works now** | `journalctl -k` (msm 1.13.0, a630 fw), GNOME EGL |
| Vulkan loader | `vulkan-icd-loader 1.4.363` | **works now** | `pacman -Q` |
| Vulkan ICD (Turnip/freedreno) | none installed; `vulkan-freedreno 1:26.2.4-1` is in `extra` (aarch64) | **works after installing packages** | §4; `pacman -Si vulkan-freedreno` |
| Vulkan 1.3 + graphics/compute queue | Turnip is "a Vulkan 1.3 driver for Adreno 6xx"; a same-class a630 report lists **1.3.296** and 117 extensions | **likely** (after ICD install; the backend requests 1.3 and accepts 1.1/1.2 via extensions) | Mesa docs; `gfx_vk_core.c:121,186-189` |
| Dynamic rendering / imageless framebuffer + render pass 2 (hard gate) | a630 report: `imagelessFramebuffer` yes + `VK_KHR_create_renderpass2` present, so the non-dynamic path also satisfies the gate; Vulkan 1.3 device (dynamic rendering is core) | **likely** | `gfx_vk_core.c:251`; Mesa docs; a630 report |
| Timeline semaphores | a630 report: `timelineSemaphore` pass; fallback existing | **likely** (non-fatal either way) | `gfx_vk_core.c:241-245` |
| Descriptor indexing, scalar layout, sync2, 16-bit float | a630 report (Mesa 24.3): descriptorIndexing yes, scalarBlockLayout yes, shaderFloat16 yes; **16-bit storage = no**, 8-bit storage = no — re-check on 26.2 with the feature dump | **likely** (features only enabled if present; rare games may need 16-bit storage) | a630 report; `gfx_vk_core.c:217-239` |
| Transform feedback, custom border colours, depth clip control, index uint8, vertex divisor | a630 report lists all as present/pass | **likely** | a630 report; `gfx_vk_core.c:192-200,254-275` |
| **BC-compressed textures** | Turnip enabled `textureCompressionBC` in 2019 (commit) and the a630 report passes it with BC1–BC7 sampled formats | **likely** on this machine (UNVERIFIED here); fallback CPU decode costs 4–8× texture RAM | commit `8626d339`; a630 report; `gfx_vk_tex.c:120-122` |
| dma-buf heaps for zero-copy pools | kernel support built as modules, **not loaded**; `modprobe system_heap` (root) + boot-time load needed; no `/dev/dma_heap` now | **works after root enables module**; **not needed for M2** (copied pools) | `/proc/config.gz`, `/proc/modules` |
| Copied-pool fallback (correctness path) | pure Vulkan host-visible memory, no special device support | **works now** in code (`DM_VK_NO_ZEROCOPY`); slow and RAM-hungry | `gfx_vk_core.c:771-774`, `docs/ARCHITECTURE.md:94` |
| SDL3 Wayland presentation | Wayland session live; SDL3 3.4.20 has the wayland backend; port code not yet written (M2) | **likely** (M2 work item) | `wayland-info`; `strings libSDL3.so`; `docs/PORTING_PLAN.md:56-58` |
| Audio (PipeWire/ALSA) | PipeWire 1.6.11 running, speaker sink works; SDL3 has pipewire/pulseaudio/alsa backends | **works now** (end-to-end SDL3 playback UNVERIFIED in the sandbox) | `pactl info`; `strings libSDL3.so` |
| Gamepad / controller input | no gamepad node; no built-in controller; BT `hci0` soft-blocked and unstable; USB-C role switching not supported | **unknown / blocker for controller play** — touch + keyboard only | `/proc/bus/input/devices`, `/sys/class/rfkill`, hardware audit §3.4-3.5 |
| Guest 4–5 GiB address-space reservation | 64-bit, 4 KiB pages; a single 5 GiB `mmap(PROT_NONE)` should succeed (needs validation in the loader) | **likely** | `getconf PAGE_SIZE`; `hle_svc.c:40-44` (Android path over-reserves by 1 GiB) |
| RAM headroom | 7.32 GiB total, **no swap**, 2.7–3.4 GiB available with GNOME; 3285 MiB app pool + host + Mesa + Vulkan pools | **likely insufficient for 5 GiB-layout / 3D titles**; plausible for 2D/light (<1.5–2 GB working set) — UNVERIFIED | `/proc/meminfo`; `hle_svc.c:67` |
| Storage | 203 GiB free UFS + 51 GiB SD; RomFS for big titles can be ~20–30 GB | **works now** | `df -h`; `docs/ARCHITECTURE.md:267` |
| CPU/GPU performance | See §7: far below the Adreno 830-class devices nx2android targets | **limited** — light titles only, honest assessment below | specs cited in §7 |
| glibc guest loader / TLS slot | loader currently assumes bionic `TLS_SLOT_APP`; glibc layout differs | **unknown** — highest-risk M3 item, must be validated at first boot | `loader.c:47-70`; `docs/PORTING_PLAN.md:88-91` |
| `hle_fs` portability | uses `mach/mach.h`, `clonefile`, `RENAME_SWAP` | **unknown** — needs Linux save-transaction implementation (M3) | `hle_fs.c:1-5,31-35` |
| Game data / keys | none on this machine (`prod.keys`, NSP/XCI/NSZ absent) | **blocker for M3 only** (owner supplies) | `docs/ALARM_PORT.md:70` |

**No hard GPU-feature blocker was found on paper.** Turnip on a6xx covers the backend's hard gate
and almost all optional features; BC is supported. The genuine risks are (a) Turnip on *this
custom kernel/Mesa build* losing the device at runtime, (b) RAM/thermal performance, and
(c) the M3 glibc loader + `hle_fs` work — not a missing Vulkan capability.

## 4. Arch Linux ARM package / infrastructure gaps

Read-only checks used: `pacman -Q`, `pacman -Si`, `pacman -Ss vulkan-freedreno`, `pacman -Ql mesa`,
`zcat /proc/config.gz`. **Nothing was installed and no `sudo` was used.**

| Item | Status here | Available / needed to install | Notes |
|---|---|---|---|
| `vulkan-freedreno` (Turnip) | **not installed** | `extra`, `1:26.2.4-1` aarch64 | The required ICD; depends on `vulkan-icd-loader` ✔, `wayland` ✔, `libdrm` ✔ |
| `vulkan-tools` (`vulkaninfo`) | **not installed** | `extra`, `1.4.363.0-1` aarch64 | Needed to verify the ICD and dump device features |
| `vulkan-mesa-layers` | not installed | optional | Validation layers for M2 debugging |
| `mesa` | installed `1:26.2.7-1` (repo shows `1:26.2.4-1`) | — | Provides `msm_dri.so` GL; Vulkan is a *separate* package, which is why there is no ICD |
| `sdl3` | installed `3.4.20-1` | — | wayland/pipewire/pulseaudio/alsa/evdev in the library |
| PipeWire stack | installed (`pipewire 1.6.11`, `pipewire-pulse`, `wireplumber`) | — | Audio path ready |
| `gcc`, `make`, `ninja`, `meson`, `python3` | installed | — | M1/M2 build chain ready |
| `clang`, `cmake`, `.NET SDK`, Java | absent | needed at M3 for the porter (`nx_extract`, shader tools) | Not needed for M1/M2 |
| Kernel: `drm/msm` + `adreno` | loaded, a630 firmware present | — | No kernel change needed for Vulkan |
| Kernel: `DMABUF_HEAPS` | `=y`, `SYSTEM=m`, `CMA=m`, **modules not loaded** | root: `modprobe system_heap` + persist (e.g. `modules-load.d`) | Only for M5 zero-copy; M2 uses copied pools |
| `/dev/dri` access for the app | render node exists in `/sys`; app launched from the GNOME seat gets the usual logind ACL | — | UNVERIFIED in the sandbox (its `/dev` is masked) |
| Chromium SwiftShader ICD | Chromium's `/usr/lib/chromium/vk_swiftshader_icd.json`, not registered | optional stop-gap via `VK_ICD_FILENAMES` | CPU-only; useful to smoke-test the backend without freedreno, **not for gameplay** |

Commands the owner runs (root; not executed here):

```sh
sudo pacman -S vulkan-freedreno vulkan-tools     # then: vulkaninfo --summary
sudo modprobe system_heap                        # optional, M5 zero-copy; add to modules-load.d
```

## 5. Cross-check with the hardware audit (the workspace's `硬件功能缺失审计.md`)

| Audit finding | Matters for nx2alarm? |
|---|---|
| **Vulkan missing** (loader only, no ICD) | **Yes — exactly the install above.** The audit marks it user-space-only (`§3.7`); no kernel work involved. This is the one "missing hardware feature" that actually blocks M2. |
| Adreno GPU OpenGL works (`renderD128`, GNOME Wayland) | **Yes, positively** — confirms the kernel/DRM side of the same GPU Vulkan will use |
| Audio/display/touch/stylus/UFS/SD/WiFi work | **Yes, positively** — presentation, audio, input and storage prerequisites exist |
| Battery/charging absent | **No** — no gameplay feature needs it; only power/suspend policy and in-app battery display (not planned) |
| Sleep: `s2idle` only, no swap/hibernate | **No** for M1–M3 — port sessions are foreground runs; only suspend/resume robustness is affected |
| USB-C role/PD/DP Alt Mode absent; no external display | **Minor** — no external-monitor output; USB-C OTG gamepad is unverified/likely unavailable |
| Bluetooth unstable + rfkill soft-blocked | **Minor** — only blocks a wireless gamepad; `rfkill unblock` is a root action and stability remains questionable |
| Keyboard/touchpad "not detected" | **Minor** — touchscreen + a HID keyboard node exist in `/proc`; touch or on-screen overlay is the reliable input path (port plans an overlay in M4) |
| Camera, motion sensors, ambient light, fingerprint absent | **No** — nx2alarm uses none of them |
| Venus H.264/HEVC codec works | **Positive, later** — gives M3+ movie playback a hardware path (V4L2) instead of MediaCodec; not an M2 requirement |
| No deep sleep / thermals monitored by 21 zones | **Minor** — the app leaves thermal policy to the OS; 12" fanless chassis will throttle under sustained 710 MHz GPU load (see §7) |

Net: the audit's six missing items reduce to one relevant install (Vulkan ICD) plus controller/USB-C caveats.

## 6. Risk register

| # | Risk | Minimal validation | Fallback / mitigation |
|---|---|---|---|
| R1 | Turnip cannot create a device on this **custom 7.3.4 kernel** (or hangs/loses the device), so the backend dies at `vkCreateDevice` | `vulkaninfo --summary` (expect `a6xx`/Turnip, Vulkan 1.3) + the planned `nx2alarm vk-smoke --frames 120` (`docs/PORTING_PLAN.md:59-69`) | Report with `TU_DEBUG`/`MESA_VK_ABORT_ON_DEVICE_LOSS=1`; try the Chromium SwiftShader ICD to separate backend bugs from driver bugs; null backend keeps M1/M3 bring-up alive |
| R2 | Missing Turnip features for some games (a630 report: no `storageBuffer16BitAccess`, no 8-bit storage, no int64 atomics) | `vkCreateDevice` feature dump probe (below); compile/run a small game's shader bundle; compare with the backend's `VKLOG` feature line (`gfx_vk_core.c:326-328`) | Backend already only enables present features; some titles may fail in the shader translator — test a light 2D title first |
| R3 | BC textures absent or broken on this Mesa build | Small BC probe: create BC1/BC3/BC7 sampled images, draw a known checker, read back; also check `textureCompressionBC` in the feature dump | `DM_VK_X_BC_CPU=1` decodes on the CPU (correct, 4–8× memory — may exhaust RAM); prefer titles with ASTC/none |
| R4 | RAM exhaustion (7.32 GiB, no swap, 3285 MiB pool + copies + BC decode) | Run a light title, watch `/proc/meminfo` and the host's memory diagnostics; repeat with GNOME stopped | Lower render scale / frame cap; copied pools if no `/dev/dma_heap`; realistically only light titles (see §7) |
| R5 | Zero-copy unavailable (no `/dev/dma_heap`) | Check `DM_VK_NO_ZEROCOPY` behavior and the copied-pool sync MB logs (`gfx_vk_core.c:1083`); after `modprobe system_heap`, re-test | Correctness first: copied pools (`DM_VK_NO_ZEROCOPY=1`); enable heaps in a later milestone |
| R6 | Thermal throttling at 2160×1440 / sustained 710 MHz | 10-minute `vk-smoke` loop; sample `thermal_zone*/temp`, `/sys/class/devfreq/5000000.gpu/cur_freq` and GPU/CPU frequency | Render internally at 720p and upscale; cap 30 fps; accept lower clocks (12" fanless) |
| R7 | glibc guest-TLS mismatch (`loader.c:47-70`) | First guest boot: loader aborts with "TLS_SLOT_APP is already in use" if broken (`loader.c:68`); else run the TSD layout check | Adapt `dm_tsd_set/get` + rewriter's slot offset; this is the single highest-risk M3 item (`docs/PORTING_PLAN.md:88-91`) |
| R8 | `hle_fs` Apple-only save transactions | Open a save, commit, kill, remount; verify old-or-new content only | Port to `renameat2(RENAME_EXCHANGE)` / copy; correctness of saves is a hard requirement |
| R9 | No controller | Launch SDL3 gamepad test with a USB/BT pad; else use keyboard/touch | On-screen overlay (M4); BT can be unblocked by root but is unstable (audit); touch keyboard fallback |
| R10 | No game files/`prod.keys` | N/A on this machine | Owner supplies own dump (M3 gate); not a hardware problem |

Minimal probes to run once the ICD is installed:

1. `vulkaninfo --summary` — `apiVersion 1.3.x`, device `a6xx`/Turnip, vendor `0x5143`.
2. **Feature dump** — a ~100-line C probe (repo Vulkan headers 1.4.363) calling
   `vkGetPhysicalDeviceFeatures2` and printing: `textureCompressionBC`, `timelineSemaphore`,
   `dynamicRendering`, `imagelessFramebuffer`, `descriptorIndexing`, `scalarBlockLayout`,
   `synchronization2`, `transformFeedback`, `16-bit storage`, `subgroup` props — this is the single
   most useful artifact for the M2 report.
3. **BC probe** — BC1/BC3/BC7 image upload + sample, read back expected texels (`docs/PORTING_PLAN.md:64-66`).
4. **Present smoke test** — the planned `nx2alarm vk-smoke --frames 120 --screenshot /tmp/vk.ppm`
   (`docs/PORTING_PLAN.md:59-61`), then run it 10 minutes while sampling thermals.

## 7. Performance expectation (honest)

| Metric | SD850 (this device) | Adreno 830 reference (nx2android test device) | Switch |
|---|---|---|---|
| CPU | 4× Cortex-A75 @ ≤2.84 GHz measured + 4× A55 @ 1.77 GHz | Oryon 2× 4.32 GHz + 6× 3.53 GHz (Snapdragon 8 Elite) | 4× Cortex-A57 @ 1.02 GHz |
| GPU | Adreno 630 @ 710 MHz, **~727 GFLOPS** FP32 (Wikipedia Adreno table; 737 in some databases) | Adreno 830, 1536 cores @ 1.1 GHz, **~3,379 GFLOPS** FP32 (zh-Wikipedia spec table) | Maxwell, 256 SP @ 307–768 MHz (~393 GFLOPS) |
| RAM bandwidth | 8 GB LPDDR4X, 64-bit; Wikipedia's Adreno table lists **29.8 GB/s** at 1866 MHz (34.1 GB/s in another revision at 2133 MHz); actual clock UNVERIFIED | LPDDR5X dual-channel 5300 MHz (≈85 GB/s class) | 4 GB LPDDR4-1600, **25.6 GB/s** |
| Storage | 225 GiB UFS | — | 32 GB eMMC |

Interpretation (ESTIMATES, not measurements):

- **Compute gap is the problem, not bandwidth.** The SD850's theoretical bandwidth (~30–34 GB/s
  depending on the LPDDR4X bin) is comparable to or above the Switch's 25.6 GB/s, but its GPU is
  ~4.6× below the Adreno 830 that nx2android's "tested" rows use, and its CPU single-thread is
  roughly a 2018 flagship. Because nx2alarm
  replays NVN on Vulkan (driver overhead on top of the game's own work), it effectively needs
  *more* GPU headroom than native Switch rendering, not less.
- **Likely playable class:** 2D/light engines and simple 3D at 720p internal resolution, 30 fps
  (e.g. Celeste, Balatro, ANIMAL WELL, Picross S+ — profiles exist for all four; their guest
  layouts are 3.5–4 GiB, `dm/games/*/game.tsv`) and the lowest RAM footprints.
- **Unlikely class:** 3D AAA ports (Tears of the Kingdom, Odyssey, Mario Wonder, Unreal Engine 5
  titles) — they target 4–5 GiB layouts and Adreno 8xx-class GPUs; expect boot/visual validation
  only, not real-time play.
- **Display cost:** the panel is 2160×1440 @60 Hz; render at 1280×720 and scale in the blit, and
  cap 30 fps on weak titles. GNOME compositing adds a full-screen pass.
- **What to measure first:** the four probes in §6 (`vulkaninfo`, feature dump, vk-smoke frame
  time, 10-minute thermal/frequency trace). They decide whether M2 is green and whether M3 should
  target a 2D title first. No number above is a benchmark of this machine — specs and estimates.

## 8. Bottom line

1. **Kernel/GPU/OS prerequisites are present** (msm + a630 firmware + render node, Wayland,
   PipeWire, SDL3, UFS, 203 GiB free). The only missing user-space prerequisite is a **Vulkan ICD**:
   install `vulkan-freedreno` (+ `vulkan-tools`) and the M2 path becomes testable.
2. **The Turnip a630 feature set is sufficient for the backend's hard gates on paper** (Vulkan 1.3,
   timeline semaphores, imageless/render pass 2, descriptor indexing, transform feedback, BC) —
   from Mesa docs/commit + a same-class a630 report, **not** this machine; the M2 probe must confirm.
3. **No `/dev/dma_heap` today**; M2 must use copied pools (correct, slower, more RAM), and
   zero-copy waits for a root module load (M5).
4. **Performance and RAM, not Vulkan, are the real limits** (2018 tablet vs 2024 flagships,
   7.3 GiB, no swap): expect light 2D/light-3D titles only.
5. **Controller input is the weakest non-GPU requirement**: no gamepad, no USB-C role support, BT
   soft-blocked/unstable; touch and keyboard are the usable inputs for now.

## 9. Sources

External (all cited claims above):

- Mesa freedreno/Turnip docs — "Turnip is a Vulkan 1.3 driver for Adreno 6xx GPUs":
  <https://docs.mesa3d.org/drivers/freedreno.html>
- Turnip enabling BC texture compression, commit `8626d339` (2019-10-04),
  `.textureCompressionBC = true`:
  <https://gitlab.freedesktop.org/bl4ckb0ne/mesa/-/commit/8626d339865c6d7b32240c6b864be67bc8e9b681>
- Same-class Adreno 630 (Xiaomi MI 8) Turnip 24.3.0 report — Vulkan 1.3.296, 117 extensions,
  timeline semaphores, imageless framebuffer, transform feedback, `VK_EXT_external_memory_dma_buf`,
  `VK_EXT_image_drm_format_modifier`, `textureCompressionBC` pass, BC1–BC7 sampled formats:
  <https://fauxvkdb.canonspike.com/gpu/5143-06030001/3a1d3425a87cfd0e>
  (single unverified-tier report, not this machine)
- Arch Linux ARM package pages: <https://archlinuxarm.org/packages/aarch64/vulkan-freedreno>,
  <https://archlinuxarm.org/packages/aarch64/vulkan-tools>; exact versions/arch from this machine's
  read-only `pacman -Si` on 2026-11-16.
- DMA-BUF heaps UAPI:
  <https://docs.kernel.org/userspace-api/dma-buf-heaps.html>
- Snapdragon 850 specs (8× Kryo 385 up to ~2.96 GHz, Adreno 630):
  <https://www.cnx-software.com/2018/06/05/snapdragon-850-windows-10-mobile-pcs/>
- Adreno table (Adreno 630/850 GFLOPS and LPDDR4X bandwidth; revisions list 29.8 GB/s @1866 and 34.1 GB/s @2133 MHz):
  <https://en.wikipedia.org/wiki/Adreno>
- Snapdragon 8 Elite / Adreno 830 specs (Oryon 4.32/3.53 GHz, Adreno 830 1100 MHz,
  3379.2 GFLOPS FP32, LPDDR5X): <https://zh.wikipedia.org/wiki/高通驍龍元件列表>;
  GPU details: <https://www.notebookcheck.net/Qualcomm-Adreno-830.911296.0.html>
- Nintendo Switch specs (4 GB LPDDR4-1600, 25.6 GB/s, Cortex-A57, Maxwell 256 SP):
  <https://en.wikipedia.org/wiki/Nintendo_Switch> and
  <https://www.gadgets360.com/games/features/nintendo-switch-price-release-date-specs-and-everything-else-you-need-to-know-1664118>

Internal evidence (paths relative to the repository root):

- `docs/ALARM_PORT.md` (environment facts, M1 contract), `docs/ALARM_ARCHITECTURE.md`
  (platform contract, replacements), `docs/PORTING_PLAN.md` (M2/M3/M5 tasks and risks),
  `docs/ARCHITECTURE.md` (pools, RomFS, save/overlay behaviour),
  `docs/UPSTREAM_README_nx2android.md:14` (BC requirement).
- `nxnative/dm/android/gfx_vk_core.c` (Vulkan init/feature chain/pools),
  `gfx_vk_tex.c` (BC decode fallback), `gfx_vk_present.c` (present path),
  `nxnative/dm/host/{loader.c,hle_svc.c,hle_fs.c}`, `dm/games/*/game.tsv`.

Marked **UNVERIFIED** on this machine: Turnip/Vulkan/BC on this exact kernel+Mesa build, RAM
sufficiency, GPU thermal sustain, USB-C/BT gamepad support, and the SD850's actual LPDDR4X clock.
