#!/bin/bash
# check_vk.sh: compile-check the Linux port of the Vulkan backend (M2) without a Vulkan device.
#
#   bash nxnative/dm/alarm/check_vk.sh [game title id]
#
# Every source of the backend (nxnative/dm/android/gfx_common.c, gfx_vk_*.c) and the Linux
# surface/present glue (nxnative/dm/alarm/gfx_vk_alarm.c) is compiled with -DDM_ALARM=1 against
# the repo's Vulkan headers and the Linux shims in nxnative/dm/alarm/compat/. The generated
# per-game headers (game_profile.h, android_profile.h) and the backend's small shaders (glslc)
# go to build/alarm/vkcheck/gen/; objects go to build/alarm/vkcheck/.
#
# This only proves the sources compile for Linux/glibc. Running the backend needs a Vulkan ICD
# (e.g. vulkan-freedreno) and a window; see nxnative/dm/alarm/GFX_VK_PORT.md (M2/M2.1).
#
# The game title id only supplies the compiled-in profile constants, as the host build does; the
# default is an existing profile because no game is booted by this check.
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
cd "$ROOT" || exit 1
GAME=${1:-${NX2ALARM_GAME:-0100152000022000}}
GEN=build/alarm/vkcheck/gen
OBJ=build/alarm/vkcheck
mkdir -p "$GEN" "$OBJ" || exit 1
CC=${CC:-gcc}

echo "== generated headers =="
python3 nxnative/dm/games/game.py header "$GAME" "$GEN/game_profile.h" || exit 1
python3 nxnative/dm/android/tools/android_profile.py header "$GAME" "$GEN/android_profile.h" || exit 1

echo "== backend shaders (glslc) =="
GLSLC=$(command -v glslc || command -v "${ANDROID_NDK_HOME:-/nonexistent}/shader-tools/linux-x86_64/glslc") || {
    echo "check_vk: glslc is missing (Arch: pacman -S shaderc; the Android build uses the NDK's)" >&2
    exit 1
}
for sh in nxnative/dm/android/shaders/*.*; do
    base=${sh##*/}
    stem=${base%.*}.${base##*.}
    spv=$OBJ/$base.spv
    "$GLSLC" --target-env=vulkan1.1 -O "$sh" -o "$spv" || exit 1
    python3 - "$spv" "$GEN/$stem.spv.h" "$base" <<'PY' || exit 1
import struct, sys
spv, out, src = sys.argv[1:4]
data = open(spv, 'rb').read()
name = out.rsplit('/', 1)[-1][:-6]          # <stem>_<stage>.spv.h -> <stem>_<stage>
words = ', '.join('%#010x' % w for w in struct.unpack('<%dI' % (len(data) // 4), data))
open(out, 'w').write('// generated from %s by glslc (compile check only)\n'
                     'static const uint32_t %s_spv[] = {%s};\n' % (src, name, words))
PY
done

INC=(
    -include nxnative/dm/android/compat/include/darwin_compat.h
    -Inxnative/dm/android/compat/include -Inxnative/dm/alarm/compat -Inxnative/dm/alarm/compat/include
    -Inxnative/dm/host -Inxnative/dm/gfx/nvn -Inxnative/dm/gfx/nvn/include -Inxnative/dm/gfx/tex
    -Inxnative/dm/third_party/vulkan-headers/include -Inxnative/dm/third_party/stb_vorbis
    -Inxnative/dm/third_party/zstd -Inxnative/dm/third_party/libvpx
    -Inxnative/dm/android -Inxnative/dm/android/shaders -I"$GEN"
)
FLAGS=(-O2 -DDM_ALARM=1 -D_GNU_SOURCE -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -Wno-format -Wno-overflow)
SRCS=(
    nxnative/dm/android/gfx_common.c
    nxnative/dm/android/gfx_vk_core.c
    nxnative/dm/android/gfx_vk_present.c
    nxnative/dm/android/gfx_vk_exec.c
    nxnative/dm/android/gfx_vk_tex.c
    nxnative/dm/android/gfx_vk_shader.c
    nxnative/dm/android/gfx_vk_dq.c
    nxnative/dm/android/gfx_vk_subgroup.c
    nxnative/dm/alarm/gfx_vk_alarm.c
)

echo "== compile ($CC) =="
rc=0
for src in "${SRCS[@]}"; do
    obj=$OBJ/${src##*/}.o
    if "$CC" -c "$src" -o "$obj" "${FLAGS[@]}" "${INC[@]}" 2>"$obj.err"; then
        echo "OK   $src"
        if [ -s "$obj.err" ]; then
            grep -E 'warning:|error:' "$obj.err" | head -6 | sed 's/^/       /'
        fi
    else
        echo "FAIL $src"
        head -40 "$obj.err"
        rc=1
    fi
done
rm -f "$OBJ"/*.err
[ $rc -eq 0 ] && echo "all ${#SRCS[@]} Vulkan sources compile for Linux (-DDM_ALARM=1)"
exit $rc
