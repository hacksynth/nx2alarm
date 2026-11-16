#!/usr/bin/env python3
# build_host.py: build the upstream host core natively for aarch64 Linux/glibc (the nx2alarm-host binary).
#
# Sources are the upstream host core (dm/host, dm/gfx, dm/gfx/nvn, dm/gfx/tex, the null graphics backend dm/android/gfx_null.c +
# gfx_common.c) plus the Darwin-compat shims the Android port added, the Linux platform layer of dm/alarm (weak stubs, the ELF
# registration table, this directory's platform/ implementations) and the third-party decoders the host needs. The Apple-only
# files (hle_table.c's Mach-O section, hle_movie_avc.c's VideoToolbox, front_mac.m / nvn_mtl.m and every other .m) are not built.
#
# Usage: python3 nxnative/dm/alarm/build_host.py [--debug] [--clean] [--game <title ID>] [-j N]
#   The output binary is build/alarm/nx2alarm-host; the generated profile header and the objects live under build/alarm/host/.
#   A game profile is required only to compile the host (its constants are compiled in); M1 does not boot a game, so the default
#   is an existing profile.
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent            # nxnative/dm/alarm
DM = HERE.parent                                  # nxnative/dm
NX = DM.parent                                    # nxnative
ROOT = NX.parent                                  # repository root
OUT = ROOT / 'build' / 'alarm' / 'host'
OBJ = OUT / 'obj'
ASM = OUT / 'asm'
BIN = ROOT / 'build' / 'alarm' / 'nx2alarm-host'
DEFAULT_GAME = '0100152000022000'                 # Mario Kart 8 Deluxe: a profile that exists; M1 only compiles its constants in

# Upstream warning set (dm/host/Makefile: -O1 -g -Wall -Wno-unused-function) at -O2 for a release-like binary.
CFLAGS = ['-O2', '-g', '-fPIC', '-Wall', '-Wno-unused-function',
          '-DDM_ALARM=1', '-D_GNU_SOURCE',
          '-include', str(DM / 'android/compat/include/darwin_compat.h')]
# Include order: compat shims first (they replace Darwin headers), then the upstream include dirs of dm/host/Makefile + the Android port.
INCLUDES = [
    DM / 'android/compat/include',
    DM / 'host',
    DM / 'gfx/nvn',
    DM / 'gfx/nvn/include',
    DM / 'gfx/tex',
    DM / 'android',
    DM / 'third_party/vulkan-headers/include',
    DM / 'third_party/stb_vorbis',
    DM / 'third_party/zstd',
    DM / 'third_party/libvpx',
    DM / 'third_party/libvpx/config',
]
LIBS = ['-lSDL3', '-lpthread', '-ldl', '-lm', '-lz']
ASM_CONVERTER = DM / 'android/tools/darwin2elf.py'
# Third-party C is built with its own flags, as dm/host/Makefile does for libvpx (clang -O2 -g -I<vpx> -I<vpx>/config): the
# Darwin-compat force-include is for host code only. libvpx's vpx_thread.c has a static function named sync(), which glibc's
# <unistd.h> declares as void sync(void); the force-include brought unistd in and made that a compile error.
THIRD_PARTY = DM / 'third_party'
TP_FLAGS = ['-O2', '-g', '-fPIC', '-Wall', '-Wno-unused-function']
TP_INCLUDES = [THIRD_PARTY / 'libvpx', THIRD_PARTY / 'libvpx/config', THIRD_PARTY / 'zstd', THIRD_PARTY / 'stb_vorbis']


def is_third_party(src):
    try:
        src.relative_to(THIRD_PARTY)
        return True
    except ValueError:
        return False


def rel(p):
    """Path as printed to the user (repository-relative)."""
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return str(p)


def obj_path(src, kind='c'):
    return OBJ / (str(src.relative_to(DM)).replace('/', '__') + '.' + kind + '.o')


def run(cmd, cwd=None, quiet=False):
    if not quiet:
        print('  $', ' '.join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None)


def source_set():
    """(C sources, assembly sources) of the host build; every entry is a Path."""
    c = []
    # Host core: everything but the two files the port replaces (Apple-only section / VideoToolbox)
    skip_host = {'hle_table.c', 'hle_movie_avc.c'}
    c += sorted(p for p in (DM / 'host').glob('*.c') if p.name not in skip_host)
    # Graphics replay (NVN) + the null backend and the shared backend diagnostics
    c += sorted((DM / 'gfx').glob('*.c'))
    c += sorted((DM / 'gfx/nvn').glob('*.c'))
    c += sorted((DM / 'gfx/tex').glob('*.c'))
    c += [DM / 'android/gfx_null.c', DM / 'android/gfx_common.c']
    # Darwin-compat shims the host calls (Mach / CoreFoundation / clonefile / sysctl / sha256)
    c += [DM / 'android/compat/darwin_compat.c', DM / 'android/compat/darwin_symbols.c', DM / 'android/compat/sha256.c']
    # Third party: the VP9/VP8 decoder (hle_movie.c) and the zstd decoder library. hle_movie.c includes
    # dm/third_party/stb_vorbis/stb_vorbis.c itself (with STB_VORBIS_NO_* defines), so it is NOT a separate source here:
    # linking it again gives multiple definitions of every stb_vorbis_* function.
    c += sorted((DM / 'third_party/libvpx').rglob('*.c'))
    c += [DM / 'third_party/zstd/zstddeclib.c']
    # The port's own layer: ELF libc table + main wrapper + weak platform stubs + the strong platform implementations.
    # These are named explicitly: dm/alarm/*.c also holds the shell binary's front end (front_alarm.c etc.), which is not part of this link.
    c += [HERE / 'hle_table_alarm.c', HERE / 'host_main_alarm.c', HERE / 'stubs_alarm.c']
    if (HERE / 'host_glue.c').exists():
        c += [HERE / 'host_glue.c']
    c += sorted((HERE / 'platform').glob('*.c'))
    a = [DM / 'host/entry.S'] + sorted((DM / 'gfx/nvn').glob('*.S'))
    # De-duplicate while keeping order (host platform glob could overlap nothing today, but be safe)
    seen = set()
    c = [p for p in c if not (p in seen or seen.add(p))]
    return c, a


def deps_of(obj, depfile):
    """Sources/headers from a -MMD depfile (empty when it does not exist yet)."""
    try:
        text = depfile.read_text(encoding='utf-8')
    except OSError:
        return []
    text = text.replace('\\\n', ' ')
    _, _, rest = text.partition(':')
    out = []
    for tok in re.split(r'\s+', rest):
        tok = tok.replace('\\ ', ' ')
        if tok:
            out.append(Path(tok))
    return out


def needs_build(src, obj, cmd):
    if not obj.exists():
        return True
    cmd_file = obj.with_suffix('.cmd')
    try:
        if cmd_file.read_text(encoding='utf-8') != cmd:
            return True
    except OSError:
        return True
    try:
        ot = obj.stat().st_mtime_ns
    except OSError:
        return True
    depfile = obj.with_suffix('.d')
    deps = deps_of(obj, depfile)
    if not deps:
        deps = [src]
    if src not in deps:
        deps.append(src)
    for d in deps:
        try:
            if d.stat().st_mtime_ns > ot:
                return True
        except OSError:
            return True   # a removed header: rebuild (the compiler reports the real error)
    return False


def posix_quote(arg):
    """A command string for the .cmd stamp (split()-stable enough: no spaces in this tree)."""
    return arg


def main():
    ap = argparse.ArgumentParser(description='Build the upstream host core natively for aarch64 Linux (nx2alarm-host).')
    ap.add_argument('--debug', action='store_true', help='print every command and its full output')
    ap.add_argument('--clean', action='store_true', help='remove the build directory and the output binary first')
    ap.add_argument('--game', default=DEFAULT_GAME, help=f'game profile title ID whose constants are compiled in (default {DEFAULT_GAME})')
    ap.add_argument('-j', type=int, default=min(3, os.cpu_count() or 1), help='parallel compile jobs (default 3: the machine has 8 cores and tight RAM)')
    args = ap.parse_args()

    t0 = time.time()
    if args.clean:
        shutil.rmtree(OUT, ignore_errors=True)
        try:
            BIN.unlink()
        except OSError:
            pass
    OUT.mkdir(parents=True, exist_ok=True)
    OBJ.mkdir(parents=True, exist_ok=True)
    ASM.mkdir(parents=True, exist_ok=True)

    # ---- game profile header ----
    gen_dir = OUT / 'gen' / args.game
    profile = gen_dir / 'game_profile.h'
    gen_dir.mkdir(parents=True, exist_ok=True)
    r = run([sys.executable, DM / 'games/game.py', 'header', args.game, profile], quiet=not args.debug)
    if r.returncode:
        print(f'error: could not generate {rel(profile)} for game profile {args.game}', file=sys.stderr)
        return 1

    # ---- assembly: Apple (Mach-O) assembly converted to ELF ----
    c_sources, a_sources = source_set()
    asm_outputs = []
    asm_jobs = []
    for src in a_sources:
        dst = ASM / (str(src.relative_to(DM)).replace('/', '__'))
        asm_outputs.append(dst)
        if dst.exists() and dst.stat().st_mtime_ns >= src.stat().st_mtime_ns and dst.stat().st_mtime_ns >= ASM_CONVERTER.stat().st_mtime_ns:
            continue
        asm_jobs.append((src, dst))
    for src, dst in asm_jobs:
        if args.debug:
            print(f'  convert {rel(src)} -> {rel(dst)}')
        r = run([sys.executable, ASM_CONVERTER, src, dst], quiet=not args.debug)
        if r.returncode:
            print(f'error: darwin2elf.py failed on {rel(src)}', file=sys.stderr)
            return 1

    # ---- compile ----
    compile_cmd = ['gcc'] + CFLAGS + ['-I' + str(p) for p in INCLUDES + [gen_dir]]
    tp_cmd = ['gcc'] + TP_FLAGS + ['-I' + str(p) for p in TP_INCLUDES]
    jobs = []
    for src in c_sources:
        obj = obj_path(src)
        obj.parent.mkdir(parents=True, exist_ok=True)
        flags = (tp_cmd if is_third_party(src) else compile_cmd) + ['-MMD', '-MF', str(obj.with_suffix('.d'))]
        if src.name == 'main.c':
            flags += ['-Dmain=dm_host_main']   # host_main_alarm.c defines main(); the upstream file keeps its body under dm_host_main
        cmd = flags + ['-c', str(src), '-o', str(obj)]
        stamp = ' '.join(posix_quote(x) for x in cmd)
        if needs_build(src, obj, stamp):
            jobs.append((src, cmd, stamp, obj))
    asm_jobs2 = []
    for dst in asm_outputs:
        obj = OUT / (dst.name + '.o')
        cmd = ['gcc', '-c', str(dst), '-o', str(obj)]
        stamp = ' '.join(str(x) for x in cmd)
        if needs_build(dst, obj, stamp):
            asm_jobs2.append((dst, cmd, stamp, obj))
    all_jobs = jobs + asm_jobs2
    if not all_jobs:
        print(f'objects up to date ({len(c_sources) + len(asm_outputs)} files)')
    else:
        print(f'compiling {len(jobs)} C / {len(asm_jobs2)} asm files with -j{args.j} ...')

        def build_one(job):
            src, cmd, stamp, obj = job
            r = subprocess.run([str(x) for x in cmd], capture_output=True, text=True)
            if r.returncode:
                return (src, r)
            obj.with_suffix('.cmd').write_text(stamp, encoding='utf-8')
            return (src, None)

        from concurrent.futures import ThreadPoolExecutor, as_completed
        failed = []
        done = 0
        with ThreadPoolExecutor(max_workers=max(1, args.j)) as ex:
            futs = [ex.submit(build_one, j) for j in all_jobs]
            for fut in as_completed(futs):
                src, r = fut.result()
                done += 1
                if r is not None:
                    failed.append((src, r))
                    print(f'  FAILED {rel(src)}')
                    out = (r.stderr or '') + (r.stdout or '')
                    if out.strip():
                        print('\n'.join('    ' + ln for ln in out.rstrip().splitlines()))
                elif args.debug:
                    print(f'  ok     {rel(src)}')
                elif done % 25 == 0 or done == len(all_jobs):
                    print(f'  {done}/{len(all_jobs)}')
        if failed:
            print(f'\n{len(failed)} file(s) failed to compile', file=sys.stderr)
            return 1

    # ---- link ----
    objs = [obj_path(src) for src in c_sources] + [OUT / (dst.name + '.o') for dst in asm_outputs]
    link_cmd = ['gcc', '-o', BIN] + objs + LIBS
    if args.debug:
        print('  link:', ' '.join(str(x) for x in link_cmd))
    print('linking ...')
    r = subprocess.run([str(x) for x in link_cmd], capture_output=True, text=True)
    if r.returncode:
        print(f'error: link failed ({r.returncode})', file=sys.stderr)
        print((r.stderr or '') + (r.stdout or ''), file=sys.stderr)
        return 1
    if (r.stderr or '').strip() and args.debug:
        print(r.stderr)

    size = BIN.stat().st_size
    print(f'built {rel(BIN)} ({size} bytes, {size / 1048576:.1f} MiB) in {time.time() - t0:.1f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
