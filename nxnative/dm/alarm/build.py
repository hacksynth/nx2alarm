#!/usr/bin/env python3
# nx2alarm shell/tool build: native aarch64 Linux binaries built with gcc and the
# system SDL3 (pkg-config sdl3), no Android NDK, no .NET.
#
#   python3 nxnative/dm/alarm/build.py --target shell
#       -> build/alarm/nx2alarm    (the shell + doctor; SDL3 only)
#   python3 nxnative/dm/alarm/build.py --target vk-smoke
#       -> build/alarm/nx2alarm-vk (M2 hardware test: info/smoke/bc; links -lvulkan)
#
# Objects go to build/alarm/obj-<target> and are rebuilt when a source, a header
# it includes (gcc -MMD) or the flags change. --jobs N limits the parallelism
# (default 4: this machine has little free RAM), --clean removes this target's
# objects and binary (the other target and build_host.py's binaries are left
# alone), --debug switches to -O0 -g3.
#
# The shell sources are entry_alarm.c, front_alarm.c and doctor.c. They include
# the bitmap fonts of nxnative/dm/android (pixel_font*.h) and the host front-end
# header of nxnative/dm/host (front.h). No game profile header is generated.
# The vk-smoke target is vk_smoke.c plus the BC compute shader compiled by glslc
# into build/alarm/vk/gen/bc_sample_comp.spv.h; it includes the repo's Vulkan
# headers through nxnative/dm/alarm/compat/vulkan/vulkan.h.
import argparse
import concurrent.futures as cf
import hashlib
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ALARM = Path(__file__).resolve().parent          # nxnative/dm/alarm
REPO = ALARM.parents[2]                          # repository root

SOURCES = ["entry_alarm.c", "front_alarm.c", "doctor.c"]

ap = argparse.ArgumentParser(description="Build the nx2alarm shell or the M2 Vulkan test tool.")
ap.add_argument("--target", default="shell", choices=("shell", "vk-smoke"),
                help="shell (M1) or vk-smoke (M2 Vulkan hardware test)")
ap.add_argument("--jobs", type=int, default=4, help="parallel compiles (default 4, keep it low)")
ap.add_argument("--clean", action="store_true", help="remove this target's objects and binary first")
ap.add_argument("--debug", action="store_true", help="debug build: -O0 -g3 instead of -O2")
ap.add_argument("--out", default=None, help="output directory (default build/alarm)")
ap.add_argument("--cc", default=os.environ.get("CC", "gcc"), help="C compiler (default gcc)")
ap.add_argument("--verbose", action="store_true", help="print every compile command")
args = ap.parse_args()

if args.jobs < 1:
    sys.exit("build.py: --jobs must be at least 1")

out_dir = Path(args.out) if args.out else REPO / "build" / "alarm"
if args.target == "shell":
    SOURCES = ["entry_alarm.c", "front_alarm.c", "doctor.c"]
    obj_dir = out_dir / "obj-shell"
    exe = out_dir / "nx2alarm"
else:
    SOURCES = ["vk_smoke.c"]
    obj_dir = out_dir / "obj-vk"
    exe = out_dir / "nx2alarm-vk"


def fail(message):
    print(f"build.py: {message}", file=sys.stderr)
    sys.exit(1)


def pkg_config(*flags):
    command = ["pkg-config", *flags, "sdl3"]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        fail("pkg-config sdl3 failed; install the SDL3 development package "
             "(Arch Linux ARM: sdl3). " + result.stderr.strip())
    return result.stdout.split()


# Toolchain and SDL3.
gcc_version = subprocess.run([args.cc, "--version"], capture_output=True, text=True)
if gcc_version.returncode != 0:
    fail(f"compiler '{args.cc}' not found")
gcc_line = gcc_version.stdout.splitlines()[0] if gcc_version.stdout else args.cc
sdl_version = pkg_config("--modversion")[0]
sdl_cflags = pkg_config("--cflags")
sdl_libs = pkg_config("--libs")

if args.clean:
    shutil.rmtree(obj_dir, ignore_errors=True)
    try:
        exe.unlink()
    except FileNotFoundError:
        pass
    print(f"build.py: cleaned {obj_dir.relative_to(REPO)} and {exe.relative_to(REPO)}")

out_dir.mkdir(parents=True, exist_ok=True)
obj_dir.mkdir(parents=True, exist_ok=True)

CFLAGS = [
    "-std=c11",
    "-DDM_ALARM=1",
    "-D_GNU_SOURCE",
    "-D_FILE_OFFSET_BITS=64",
    "-O0" if args.debug else "-O2",
    "-Wall",
    "-Wextra",
]
if args.debug:
    CFLAGS += ["-g3", "-fno-omit-frame-pointer"]

VULKAN_INCLUDE = ALARM.parent / "third_party" / "vulkan-headers" / "include"
if args.target == "shell":
    INCLUDES = [
        f"-I{ALARM}",
        f"-I{ALARM.parent / 'android'}",   # pixel_font.h, pixel_font_latin1.h
        f"-I{ALARM.parent / 'host'}",      # front.h
    ]
    LINK_FLAGS = ["-o", str(exe)] + sdl_libs + ["-ldl", "-lm"]
else:
    gen_dir = out_dir / "vk" / "gen"
    gen_dir.mkdir(parents=True, exist_ok=True)
    glslc = shutil.which("glslc")
    if not glslc:
        fail("glslc is missing (Arch: pacman -S shaderc); the BC probe shader cannot be built")
    for shader in sorted((ALARM / "vk").glob("*.*")):
        stem, stage = shader.stem, shader.suffix.lstrip(".")
        header = gen_dir / f"{stem}_{stage}.spv.h"
        if header.exists() and header.stat().st_mtime >= shader.stat().st_mtime:
            continue
        spv = out_dir / "vk" / f"{shader.name}.spv"
        result = subprocess.run([glslc, "--target-env=vulkan1.2", "-O", str(shader), "-o", str(spv)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            fail(f"glslc {shader.name}: {result.stderr.strip()}")
        import struct
        data = spv.read_bytes()
        words = struct.unpack(f"<{len(data) // 4}I", data)
        body = ", ".join(f"{w:#010x}" for w in words)
        header.write_text(f"// generated from nxnative/dm/alarm/vk/{shader.name} by glslc (build.py)\n"
                          f"static const uint32_t {stem}_{stage}_spv[] = {{{body}}};\n")
        spv.unlink()
    INCLUDES = [
        f"-I{ALARM}",
        f"-I{ALARM / 'compat'}",            # <vulkan/vulkan.h> umbrella for the repo headers
        f"-I{VULKAN_INCLUDE}",              # vulkan_core.h
        f"-I{gen_dir}",                     # generated shader headers
    ]
    CFLAGS += ["-Wno-missing-field-initializers"]   # Vulkan aggregates are habitually partially initialised
    LINK_FLAGS = ["-o", str(exe)] + sdl_libs + ["-lvulkan", "-ldl", "-lm"]

CFLAGS += INCLUDES

# Flag changes rebuild every object: make's style would leave stale objects.
stamp = obj_dir / "flags.stamp"
fingerprint = hashlib.sha256(repr((CFLAGS, LINK_FLAGS, args.cc)).encode()).hexdigest()
if stamp.exists() and stamp.read_text() != fingerprint:
    for old in obj_dir.glob("*.o"):
        old.unlink()
    for old in obj_dir.glob("*.d"):
        old.unlink()
    print("build.py: compiler flags changed, rebuilding all shell objects")
stamp.write_text(fingerprint)


def dependency_is_stale(obj, dep, src):
    if not obj.exists() or not dep.exists():
        return True
    obj_time = obj.stat().st_mtime
    if src.stat().st_mtime > obj_time:
        return True
    text = dep.read_text(errors="replace").replace("\\\n", " ")
    dependencies = text.split(":", 1)[1] if ":" in text else ""
    for item in dependencies.split():
        path = Path(item.replace("\\ ", " "))
        if path.exists() and path.stat().st_mtime > obj_time:
            return True
    return False


def compile_source(name):
    src = ALARM / name
    obj = obj_dir / (src.stem + ".o")
    dep = obj.with_suffix(".d")
    if not dependency_is_stale(obj, dep, src):
        return name, obj, 0, "", False
    command = [args.cc, *CFLAGS, "-MMD", "-MF", str(dep), "-c", str(src), "-o", str(obj)]
    if args.verbose:
        print(" ".join(command))
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    return name, obj, result.returncode, result.stderr, True


print(f"build.py: target {args.target}, {len(SOURCES)} source(s), -j{args.jobs}")
print(f"  compiler : {gcc_line}")
print(f"  SDL3     : {sdl_version} ({' '.join(sdl_cflags) or 'default include path'})")
print(f"  output   : {exe}")

objects = []
failed = []
warning_count = 0
compiled = 0
with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for name, obj, code, stderr, did_compile in pool.map(compile_source, SOURCES):
        objects.append(obj)
        if did_compile:
            compiled += 1
        if stderr.strip():
            print(f"--- {name}\n{stderr.rstrip()}\n", file=sys.stderr)
            warning_count += stderr.count("warning:")
        if code != 0:
            failed.append(name)

print(f"build.py: compiled {compiled} of {len(SOURCES)} source(s), {warning_count} warning(s)")
if failed:
    fail("compile failed: " + ", ".join(failed))

link = subprocess.run([args.cc, *objects, *LINK_FLAGS], capture_output=True, text=True)
if link.returncode != 0:
    print(link.stderr, file=sys.stderr)
    fail("link failed")
if not exe.exists():
    fail(f"link reported success but {exe} does not exist")

size_kib = exe.stat().st_size / 1024.0
print(f"build.py: built {exe.relative_to(REPO)} ({size_kib:.0f} KiB)")
print(f"build.py: warnings: {warning_count}")
sys.exit(0)
