#!/usr/bin/env python3
"""port_alarm.py — the Arch Linux ARM (ALARM) half of nx2android's porter (M3).

Runs the platform-independent porter stages for a game folder (scan/survey, extraction,
rewrite), builds the nx2alarm host with `build_host.py` for that game profile, and assembles a
Linux port folder the host can be pointed at:

    <out>/<App>-linux/
      nx2alarm-host      the host binary for this profile (null or vk backend)
      mods/              the rewritten modules (patch_nso.py's outputs)
      RomFS/             symlink to the extraction's RomFS
      aoc/, sysdata/     when the game has them
      save/, tmp/        the host's DM_SAVE / DM_TEMP
      run.sh             sets DM_ROOT/DM_ROMFS/DM_SAVE/DM_TEMP and runs the host

This is the M3 path: the Android porter (`python -m porter port`) is untouched, and the
Linux-only stages live here until they are wired into `porter` as `--target alarm`.
Shaders (`shaders.dmsv`) are not made here yet, so use `--gfx null` for the first boot; the
Vulkan backend needs the shader bundle (M3b).

Usage:
    tools/venv/bin/python nxnative/dm/alarm/port_alarm.py <game folder> [-k prod.keys]
        [--out DIR] [--gfx null|vk] [--experimental] [--stop-after inputs|extract|rewrite|host]
        [-j N]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from porter import config, toolchain                      # noqa: E402
from porter.pipeline import Options, Port, app_stem, PY, NXNATIVE, nsp, profile_guess   # noqa: E402
from porter.upstream import BuildError, say               # noqa: E402


def link_tree(src: Path, dst: Path) -> None:
    """One file system copy of the rewritten modules: hard links where possible (the same
    volume), copies otherwise."""
    dst.mkdir(parents=True, exist_ok=True)
    for p in src.rglob('*'):
        t = dst / p.relative_to(src)
        if p.is_dir():
            t.mkdir(parents=True, exist_ok=True)
            continue
        t.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(p, t)
        except OSError:
            shutil.copy2(p, t)


class AlarmPort(Port):
    """The platform-independent stages plus the ALARM rewrite target (patch_nso.py DM_TARGET=alarm)."""
    tsd_offset = 16

    def rewrite(self):
        self.patched = self.aw / f'patched_{self.pkg_ver}'
        nsp.rewrite(PY, NXNATIVE, self.game / 'ExeFS', self.patched,
                    dict(self.env, DM_TARGET='alarm', DM_ALARM_TSD_OFFSET=str(self.tsd_offset)),
                    self.logs, self.g)
        if self.derived or self.provisional:   # the keys that need the rewritten modules (upstream's provisional steps)
            nsp.provisional_prep(PY, NXNATIVE, self.env, self.logs, self.g)
            self.g = nsp.refine_after_rewrite(self.g, self.game, self.patched)
            if self.provisional and profile_guess.heap_check_needed(self.game / 'ExeFS') and not nsp.heap_checked(self.g):
                say('[RED] no heap check: it boots the game in the Mac host; the provisional profile keeps its guessed heap keys')


def prelude(o: Options, p: Port) -> None:
    """Port._run's setup, without the Android tools: an ALARM port needs neither the NDK nor
    adb, only .NET (nx_extract) and the Python modules the porter's python checks."""
    o.folder = Path(o.folder).expanduser().resolve()
    if not o.folder.is_dir():
        sys.exit(f'port_alarm: not a folder: {o.folder}')
    keys = o.keys or p.cfg.get('keys')
    if not keys:
        sys.exit('port_alarm: no prod.keys: give -k/--keys, or set it once with `porter config keys <path>`')
    o.keys = Path(keys).expanduser().resolve()
    dotnet = toolchain.find_dotnet(p.cfg)
    if not dotnet:
        sys.exit('port_alarm: .NET SDK 10 not found: install it, or `porter config dotnet <path>`')
    p.tools = {'ndk': None, 'dotnet': dotnet, 'adb': None}
    o.out = Path(o.out or p.cfg.get('out') or o.folder).expanduser().resolve()
    os.environ['PATH'] = os.pathsep.join([str(dotnet.parent), os.environ.get('PATH', '')])
    os.environ['DOTNET_ROOT'] = str(toolchain.dotnet_root(dotnet))
    p.env = dict(os.environ, J=str(o.jobs), PYTHONIOENCODING='utf-8', PYTHONUTF8='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')
    p.result = {}
    p.unpacked = {}
    say(f'port_alarm: game folder {o.folder}')
    say(f'port_alarm: prod.keys {o.keys}')
    say(f'port_alarm: .NET {dotnet}')


def build_host(p: Port, gfx: str) -> Path:
    env = dict(p.env)
    # the provisional/derived profile folder is already in DM_GAMES_DIR/DM_GAMES_FIRST (use_profile_dir)
    cmd = [sys.executable, str(REPO / 'nxnative/dm/alarm/build_host.py'), '--game', f'{p.app:016X}', '--gfx', gfx]
    if getattr(p, 'gl', False):
        cmd.append('--gl')   # a GL game: the EGL/GLES host layer (gl_alarm.c)
    say(f'port_alarm: build host: {" ".join(cmd)}')
    r = subprocess.run(cmd, env=env)
    if r.returncode:
        sys.exit(r.returncode)
    return REPO / 'build/alarm' / ('nx2alarm-host' if gfx == 'null' else 'nx2alarm-host-vk')


def assemble(p: Port, host: Path, out: Path) -> Path:
    stem = app_stem(p.g, p.app_name)
    port = out / f'{stem}-linux'
    if port.exists():
        shutil.rmtree(port)
    port.mkdir(parents=True)

    link_tree(p.patched, port / 'mods')
    romfs = p.game / 'RomFS'
    if romfs.is_dir():
        (port / 'RomFS').symlink_to(romfs, target_is_directory=True)
    aoc = getattr(p, 'aoc', None)
    if aoc and Path(aoc).is_dir():
        (port / 'aoc').symlink_to(Path(aoc), target_is_directory=True)
    sysdata = getattr(p, 'sysdata', None)
    if sysdata and Path(sysdata).is_dir():
        (port / 'sysdata').symlink_to(Path(sysdata), target_is_directory=True)
    (port / 'save').mkdir()
    (port / 'tmp').mkdir()
    shutil.copy2(host, port / 'nx2alarm-host')
    (port / 'run.sh').write_text(
        '#!/bin/sh\n'
        '# nx2alarm M3: run the rewritten game headless (the null backend needs no window).\n'
        'set -e\n'
        f'd="{port}"\n'
        'export DM_ROOT="$d" DM_ROMFS="$d/RomFS" DM_SAVE="$d/save" DM_TEMP="$d/tmp"\n'
        'exec "$d/nx2alarm-host" "$d/mods" "$@"\n', encoding='utf-8')
    (port / 'run.sh').chmod(0o755)
    say(f'port_alarm: port folder {port}')
    say(f'port_alarm: run it with {port / "run.sh"} (RMFS {romfs})')
    return port


def main() -> int:
    ap = argparse.ArgumentParser(description='nx2alarm M3 pipeline: extract + rewrite + build host + assemble a Linux port folder.')
    ap.add_argument('folder', help='the game folder (XCI/NSP/NSZ/XCZ files, updates, DLC)')
    ap.add_argument('-k', '--keys', default=None, help='prod.keys (default: the configured one)')
    ap.add_argument('-o', '--out', default=None, help='output folder for <App>-linux (default: the game folder)')
    ap.add_argument('--gfx', choices=('null', 'vk'), default='null', help='host graphics backend (null for the first boot; vk needs shaders.dmsv, not made yet)')
    ap.add_argument('--experimental', action='store_true', help='try a game without a profile (survey + provisional profile; it may not start)')
    ap.add_argument('--stop-after', choices=('inputs', 'extract', 'rewrite', 'host'), default='host', help='stop after this stage (debugging)')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    p = AlarmPort(Options(folder=Path(a.folder), keys=Path(a.keys) if a.keys else None, out=Path(a.out) if a.out else None,
                          experimental=a.experimental, jobs=a.jobs))
    prelude(p.o, p)
    try:
        p.inputs()
        if a.stop_after == 'inputs':
            return 0
        p.extract()
        p.check_graphics()
        if a.stop_after == 'extract':
            return 0
        # The host build only needs the profile; it comes before the rewrite so its --tsd-offset is known to
        # patch_nso.py (DM_TARGET=alarm): the TLS offset is a property of the host binary, not of the game.
        host = build_host(p, a.gfx)
        if a.gfx == 'null':
            out = subprocess.check_output([str(host), '--tsd-offset'], env=p.env, text=True).strip()
            p.tsd_offset = int(out)
            say(f'port_alarm: guest-slot TLS offset {p.tsd_offset} (tpidr_el0 + {p.tsd_offset})')
            if not 0 <= p.tsd_offset <= 4095:
                sys.exit(f'port_alarm: TLS offset {p.tsd_offset} is outside the rewriter\'s 0..4095 range')
        p.rewrite()
    finally:
        p.drop_unpacked()
    if a.stop_after == 'rewrite':
        return 0
    assemble(p, host, Path(a.out or p.cfg.get('out') or p.o.folder).expanduser().resolve())
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BuildError as e:
        sys.exit(f'port_alarm: {e}')
