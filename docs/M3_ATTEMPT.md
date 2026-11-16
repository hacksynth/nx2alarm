# M3 attempt — first game boot on Arch Linux ARM

Date: 2026-11-16 (Asia/Shanghai) · Machine: Huawei MateBook E 2019 (SDM850, Adreno 630),
Arch Linux ARM, kernel 7.3.4, Mesa 26.2.7 / Turnip, SDL3 3.4.20.

This is the M3 milestone's first end-to-end attempt with **the owner's own game files and keys**
(nothing from them is committed). It reached `nnMain`, initialized the Nintendo SDK, created a
real desktop OpenGL 4.6 context on the Adreno 630, resolved 1,000+ GL entry points, then the
guest crashed in its own initialization code on a null dispatch pointer. The pipeline
(extract → rewrite → Linux host → port folder) works; the remaining work is guest bring-up.

## 1. Materials and where they live

| Material | Where | Notes |
|---|---|---|
| Game dump `[010008900705c000][US][v0].xci` (830 MB) | `nx2alarm/.gamedump/DragonQuestBuilders/` | git-ignored; internal title ID is **01007B5002F56000** (the file name's ID is not the NACP title ID), **DRAGON QUEST BUILDERS** v1.0.0, US, SDK 1 |
| `prod.keys` (231 entries) + `title.keys` (19 entries), v22.5.0 | `~/.config/nx2alarm/keys/` (mode 700/600) | outside the repository; never printed; `porter config keys` points at it |
| Python deps | `nx2alarm/tools/venv` (git-ignored) | lz4, capstone, pycryptodome, pillow, fontTools, pyyaml |
| .NET SDK 10.0.404 | `nx2alarm/tools/dotnet` (git-ignored) | for `nx_extract` |
| `glslc` | system (`shaderc`) | shader headers for the host build |
| gdb 18.1 | system | guest-crash debugging (the only other system change after the Vulkan driver) |

## 2. The ALARM port pipeline (`port_alarm.py`)

The Android porter is untouched. `nxnative/dm/alarm/port_alarm.py` drives its
platform-independent stages and replaces the Android host/APK stages:

```sh
tools/venv/bin/python nxnative/dm/alarm/port_alarm.py .gamedump/DragonQuestBuilders --experimental
```

Stages that ran:

1. **inputs/survey** — no upstream profile for 01007B5002F56000, so the survey wrote a
   provisional profile into `<game>/01007B5002F56000_build/profile/` (modules, heap guesses,
   shader groups), with `status experimental`.
2. **extract** — `nx_extract` (built by the .NET SDK) extracted the XCI: ExeFS modules (main,
   subsdk0–subsdk5, sdk), RomFS (789 MB), `control.nacp`.
3. **graphics classification** — `main` imports EGL and names `glShaderSource`: the game renders
   with **OpenGL**, not NVN. `port_alarm.py` then builds the host with `--gl`.
4. **host** — `build_host.py --game 01007B5002F56000 --gfx null --gl` built
   `nx2alarm-host` (8.2 MB) with the new ALARM GL layer; it printed the guest-slot TLS offset
   (80) that the rewrite needs.
5. **rewrite** — `patch_nso.py` with the new `DM_TARGET=alarm` and
   `DM_ALARM_TSD_OFFSET=80` rewrote main, subsdk0, subsdk2–subsdk5, sdk.
6. **assemble** — `<game>/DragonQuestBuilders-linux/`:
   `mods/` (hard-linked rewritten modules), `RomFS/` symlink, `nx2alarm-host`, `run.sh`,
   `save/`, `tmp/`.

## 3. Two port-level fixes this needed

1. **Guest thread slots on glibc.** Android stores the per-thread guest slot block pointer in
   bionic's `TLS_SLOT_APP` (`tpidr_el0[2]`); glibc's TCB only occupies TP+0..15, so that offset
   is another module's TLS data. The port now defines one `__thread` pointer in the host
   (`dm_alarm_tsd_block`, `host/loader.c`), prints its TP-relative offset at build time
   (`nx2alarm-host --tsd-offset`), and the rewriter's new `DM_TARGET=alarm` emits
   `mrs tpidr_el0; add tp, tp, #offset; ldr block, [tp]` — the same slot-block model, in a
   variable glibc will not overwrite. The loader's fixups target header now accepts
   `TARGET alarm`. **Verified at runtime**: the loader loaded every module, `tsd_check` passed,
   and guest threads read/wrote their slots without corruption.
2. **GL games.** `gl_alarm.c` is the Linux port of `gl_android.c`: EGL/GLES/desktop-GL driver
   behind the host import table, big-stack trampolines, GL wrappers, BC decode, and a no-op
   present (offscreen pbuffer) for M3a. It now honors `eglBindAPI(EGL_OPENGL_API)` and creates a
   desktop core context (4.5 → 3.3 fallback) through `libGL`; ES remains the default. It also
   registers the 24 GL functions Dragon Quest Builders imports directly (not via
   `eglGetProcAddress`) in the host import table.

## 4. Boot result

```console
[dm] guest thread slots in a glibc TLS variable: K18=1 KTLS=2 KTLR=3 KGATE=4
[dm] >>> nnosInitialize
[dm] guest AS ... heap ... alias ... stack ...
[dm] >>> nninitInitializeSdkModule
[dm] >>> nninitStartup
[dm] [mem] heap 3.14 GB (heap region 3.25 GB)
[dm] main: DT_INIT +0x24 (init_array 678 entries)
[dm] >>> nnMain
[dm] vi::SetLayerScalingMode 2 (ignored)
[dm] [gl] device EGL 1.5
[dm] [gl] eglBindAPI(0x30a2): desktop OpenGL (libGL)
[dm] [gl] window surface: a 1920x1080 pbuffer, rendered offscreen (presentation is not wired yet)
[dm] [gl] EGL context ... (desktop OpenGL)
[dm] [gl] GL_VERSION 4.6 (Core Profile) Mesa 26.2.7-arch1.1 | GL_RENDERER FD630 | GL_SHADING_LANGUAGE_VERSION 4.60
[dm] [gl] eglGetProcAddress: 200/400/600/800/1000 names asked, all found
[dm] CRASH sig=11 addr=0 pc=0 lr=main+0x2b90b8
```

So: the loader, glibc TLS slot, SDK initialization (heap, threads, services), EGL 1.5,
**desktop OpenGL 4.6 on the Adreno 630 via freedreno**, and 1,000+ GL entry points all work.
The crash is inside the guest: an indirect call through a pointer in a zero-initialized bss
singleton (the guest's graphics-device/dispatch object) that was never filled in; the same
pattern earlier pointed at a different singleton. No `UNIMPL` host imports are reported at this
point (`stubs=0`), so the likely causes are an engine subsystem the provisional profile/host
does not set up (possibilities: the omitted `subsdk1` NintendoSDK_OpenGL module, the `vi`/native
window path, or a desktop-GL-specific init step), not a missing HLE symbol.

## 5. What is verified / what is not

| Item | Status |
|---|---|
| Keys stored securely, parser accepts them, porter extracts/rewrites with them | verified |
| XCI → ExeFS/RomFS extraction, provisional profile survey | verified |
| `DM_TARGET=alarm` rewrite + glibc TLS slot mechanism | verified at runtime (modules load, tsd_check passes) |
| SDK/HLE boot to `nnMain`, 3.14 GB heap, guest threads | verified |
| EGL 1.5 + desktop GL 4.6 (FD630) via the new GL layer; 1,000+ GL functions | verified |
| Game reaches gameplay / title screen | **not yet** (guest init crash described above) |
| Presentation | no-op: pbuffer, offscreen only (M3b) |
| Vulkan backend with this game | not applicable (game is GL); Vulkan shaders not made |
| Input (`DM_KEYS`) / front end | not wired yet |

## 6. Next steps

1. **Debug the guest null dispatch object.** With gdb/core dumps, watchpoint the bss singleton
   (guest offsets around `main+0x109c4f0`; the call site is `main+0x2b8ffc..0x2b90b8`) and find
   which constructor/initializer should store its first pointer. The earlier gdb attempt was
   inconclusive because the game's threads and crash handler interleave; a `SIGSEGV`-only batch
   run or a host-side guest-memory dump is the next tool.
2. **Try a supported game.** A title with an upstream profile (e.g. Celeste, Balatro, ANIMAL WELL,
   Metroid Dread) is much more likely to boot and validates the same pipeline; DQB is an
   unsupported provisional port and its crash is engine-specific.
3. **M3b front end.** SDL3 window + input (`DM_KEYS` for scripted A presses), then wire the GL
   layer's `eglSwapBuffers` to the Vulkan swapchain (or a GL window) so frames are visible.
4. **Shaders/Vulkan** for NVN games: `porter`'s shader stages plus `--gfx vk`; not needed for
   this GL title beyond a present path.
5. **M2.1** dma-buf zero-copy once `/dev/dma_heap` is enabled (`modprobe system_heap`).

## 7. Reproduce / clean up

```sh
# one-time toolchain (git-ignored):
python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone pycryptodome pillow fonttools pyyaml
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0 --install-dir "$PWD/tools/dotnet" --no-path
tools/venv/bin/python -m porter config keys ~/.config/nx2alarm/keys/prod.keys

# port + boot (game files in .gamedump/, git-ignored):
tools/venv/bin/python nxnative/dm/alarm/port_alarm.py .gamedump/DragonQuestBuilders --experimental
.gamedump/DragonQuestBuilders/DragonQuestBuilders-linux/run.sh
```

`tools/` (763 MB) and `.gamedump/` (1.7 GB: the XCI plus its extraction and build folder) are
git-ignored and stay for continued work; delete `.gamedump/` when the games are no longer
needed. The keys are only in `~/.config/nx2alarm/keys` (never in the repository).
