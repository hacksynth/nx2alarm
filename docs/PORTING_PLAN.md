# Porting plan: nx2alarm milestones

This is the engineering plan for taking nx2android from its current state (M1 shell + host
compile) to playing games natively on Arch Linux ARM. Each milestone has a concrete
deliverable, an acceptance test that can be run on this device, and its risks. The plan is
deliberately ordered so that each step is verifiable without game files where possible.

Working branch, machine and constraints: see `docs/ALARM_PORT.md`.

## M0 — Upstream snapshot (done)

Fork of nx2android `e5a4703` (`chore: import nx2android e5a4703 as the base of nx2alarm`),
Android target kept intact, `nxnative/dm/alarm/` added for the Linux layer.

**Acceptance:** repository imports cleanly; upstream docs preserved in
`docs/UPSTREAM_README_nx2android.md` and `docs/upstream/`; `LICENSE` attribution kept.

## M1 — Shell + host compile (this session)

**Deliverables**

1. `build/alarm/nx2alarm` — SDL3 shell + `doctor` (device/prerequisite audit), built natively
   with gcc 16; no Android NDK, no .NET, no Vulkan ICD required. `shell` opens a window,
   `--headless` and `--screenshot` work for automated checks.
2. `build/alarm/nx2alarm-host` — upstream host core (`dm/host`, `dm/gfx`, null graphics,
   Darwin-compat shim) compiled natively for aarch64 Linux/glibc and linked; starts and prints
   the upstream usage message without game data. No Android libraries in `ldd`.
3. Platform layer `nxnative/dm/alarm/`: `build.py` (shell), `build_host.py` (host), SDL3 front
   end, `doctor`, weak stubs + strong Linux implementations for audio/saves/LAN/haptics/perf/
   assets; ELF replacement for the Apple-only `hle_table.c`.
4. Vulkan backend M2 groundwork: `#ifdef DM_ALARM` surface/present port map
   (`dm/alarm/GFX_VK_PORT.md`), SDL3 surface glue, Android-header shims, copy-pool memory
   fallback; compile-checked but not runnable here (no Vulkan ICD).
5. Docs: `README.md`, `docs/FEASIBILITY.md`, `docs/ALARM_ARCHITECTURE.md`, `docs/PORTING_PLAN.md`.

**Acceptance (run by the Lead):** exact commands in `docs/ALARM_PORT.md` §6, all passing;
`nx2alarm doctor` reports the missing Vulkan ICD as a warning; `nx2alarm-host` usage path
works; no Android libs in `ldd`.

**Risks:** glibc/bionic differences (guest TLS slot, `sysctl`, futex, `__clear_cache`),
Apple-only sections in shared sources, missing Android emulation for some HLE paths (movie,
IME, sensors). None of these is exercised by M1 without game files; each is recorded with the
file that needs the change.

## M2 — Vulkan on the device (next)

**Goal:** the Vulkan backend initializes on this device and presents a test frame from a
window, with the copy-pool memory path.

**Prerequisites (the owner must install, `sudo`):**
- a Vulkan ICD for Adreno 630: `vulkan-freedreno` (Turnip) from the Arch Linux ARM repos, or
  an equivalent Mesa build; confirm `vulkaninfo --summary` lists an `a6xx`/`Turnip` device.
- `vulkan-tools` for `vulkaninfo` (optional but recommended).

**Work**
1. Finish the `#ifdef DM_ALARM` present path in `gfx_vk_present.c` / `gfx_vk_core.c`: create
   the instance with `VK_KHR_surface` + `VK_KHR_wayland_surface` extensions from SDL, device
   selection, swapchain (format/size/present mode FIFO), image views, resize handling.
2. Provide a `dm/alarm/vk_smoke.c` mode: `nx2alarm vk-smoke` opens the SDL window, runs the
   backend's device init, clears the swapchain to a moving color, presents N frames, writes a
   PPM screenshot, exits 0. This validates the device and the present path **without** a game.
3. Implement the copy-pool path and verify against the code comments in `gfx_vk_core.c`
   ("allocator refuses" fallback). Keep the zero-copy path compiled out behind `DM_ALARM`.
4. Validate the BC texture path: a small probe that creates BC1/BC3/BC7 sampled images and
   renders a gradient/checker to prove the GPU can read BC-compressed data (the porting
   requirement from the README).

**Acceptance:** `vulkaninfo` shows a6xx; `nx2alarm vk-smoke --frames 120 --screenshot
/tmp/vk.ppm` exits 0 with a correctly colored frame; BC probe renders expected texels; no
validation-layer errors (if installed).
**Risks:** Turnip feature gaps vs Android Qualcomm driver (descriptor indexing, timeline
semaphores, dynamic rendering, 16-bit storage), Wayland swapchain vs eDP scaling, thermal
throttling at 1440p (use 720p/1080p).
**Fallback:** if Turnip cannot run the backend, keep the null backend for bring-up and
revisit with a Mesa/Turnip bug report or a reduced feature set.

## M3 — First game boot (needs the owner's game data)

**Goal:** one light title boots to a rendered frame on this device with audio.

**Prerequisites:** the owner's own NSP/XCI/NSZ + `prod.keys`, plus the tools the porter needs
(.NET SDK 10 for `nx_extract`, Python packages). No game files are shipped or downloadable here.

**Work**
1. `porter --target alarm` (new target): reuse inputs/decompress/extract/rewrite/shaders;
   replace the host stage with `build_host.py` (gcc) and the apk/data stage with a Linux port
   dir (`mods/`, `RomFS/`, `aoc/`, `sysdata/`, `shaders.dmsv`, `env.txt`, `settings.txt`).
2. Verify the rewritten NSO loads on glibc: loader, relocations, gate dispatch, guest TLS
   (`dm_tsd_set` slot must be validated against glibc's TCB — this is the single highest-risk
   loader item), `mmap` layout vs the game profile's guest sizes (5 GB layouts need the same
   address-space assumptions).
3. Wire the SDL front end: create the window before the guest starts, run the guest on a
   thread, present from the Vulkan backend, forward gamepad/keyboard/touch.
4. Implement the pause-menu/overlay path (pause menu, gamepad overlay, settings) or start
   headless with scripted input (`DM_KEYS`) and add UI after the first frame.
5. Audio: SDL3 backend through the AudioToolbox-compatible surface, `hle_audio.c` unchanged.
6. Choose the first titles: 2D/light engines with no host-specific fixes — e.g. Celeste
   (MonoGame/BRUTE, no host change upstream), then Hollow Knight: Silksong (Unity, heavy),
   Metroid Dread (NVN, 60 fps) if Turnip holds. Avoid games needing firmware `sysdata` or
   movie decoding first.

**Acceptance:** the first title reaches gameplay at a stable frame rate with audio; a 10-minute
run shows no host crash; save/export works in the XDG save dir; logs contain no unresolved
`[RED]` host diagnostics that block play.
**Risks:** glibc loader/TLS differences, Turnip feature gaps, RAM (7.3 GB total), thermal
throttling, missing MediaCodec replacement (movies skipped), gamepad/touch overlay not yet
ported.

## M4 — Feature parity / usability

Pause menu + on-screen gamepad overlay + layout editor, text entry, haptics/rumble, touch,
fullscreen/refresh handling, saves UI, multithread replay (`--mt-replay` equivalent), GLES
games via EGL (`gl_alarm.c`), movie playback via V4L2 Venus or ffmpeg, LAN local play,
per-game `android.tsv`/`env` port metadata applied on ALARM.

**Acceptance:** the M1/M3 titles pass a manual checklist; GL games and the overlay work;
movies play; no Android-specific features leaked into the Linux build.

## M5 — Performance + packaging

dma-buf zero-copy pools (`/dev/dma_heap`), shader bundle/caches, CPU affinity/thermal policy,
render scale presets, a Linux app-dir with launcher `.desktop`, `porter` integration, CI for
aarch64 Linux (build + unit tests), release zips (`nx2alarm-<version>-linux-aarch64.zip`).

**Acceptance:** packaged app starts from the desktop environment; docs cover build, porting and
troubleshooting; release artifacts reproducible on CI.

## Effort sketch (single experienced engineer, optimistic)

| Milestone | Estimate |
|---|---|
| M1 shell + host compile | 1–3 days (this session covers the scaffolding) |
| M2 Vulkan device + present | 3–7 days |
| M3 first game boot | 1–3 weeks (depends heavily on the title and Turnip stability) |
| M4 parity | 3–6 weeks |
| M5 packaging/perf | 1–2 weeks |

These are engineering estimates, not promises; the first game boot is where most unknowns are.

## What the owner must provide for M2/M3

1. A Vulkan ICD on the device (`sudo pacman -S vulkan-freedreno vulkan-tools`, verify with
   `vulkaninfo --summary`).
2. The owner's own game files and `prod.keys` in a folder; never committed.
3. Optional: a cooled device (thermals cap sustained performance), a controller for input
   testing, and patience for the first shader translation pass on a weak CPU.

## Milestone tracker

- [x] M0 upstream snapshot
- [ ] M1 shell + host compile (this session)
- [ ] M2 Vulkan smoke on a6xx
- [ ] M3 first game boot
- [ ] M4 feature parity
- [ ] M5 performance + packaging
