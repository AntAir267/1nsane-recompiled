#!/bin/bash
# tools/dev/win_cross.sh OUT: 1NSANE for 64-bit Windows, cross-built here on Linux with Zig (the
# toolkit's runtime/win host; its tests/stubgame/run.sh recipe plus the D3D7 renderer and Vulkan),
# for test runs on a Windows PC without MSYS2 (Mixcraft-Machine). Not the release build
# (tools/release/windows.sh, in MSYS2): no DLSS, FSR 2 or FreeType (FONT keeps the game's letters),
# no icon or manifest, a console program. Needs gen/ (make) and SDL3 / zlib built for Windows
# (SDL3_WIN: penguins-native's third_party/sdl3-win; ZLIB_WIN: the toolkit's work/zlib-win, by default).
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=$(realpath -m "${1:?usage: win_cross.sh OUT}")
ZIG=${ZIG:-$HOME/DevWork/1nsane-native/third_party/zig-x86_64-linux-0.17.0/zig}
SDL3_WIN=${SDL3_WIN:-$HOME/DevWork/penguins-native/third_party/sdl3-win}
ZLIB_WIN=${ZLIB_WIN:-$HOME/DevWork/AntiStatic-Recomp-Toolkit/work/zlib-win}   # (a full zlib: capture.c uses gzopen)
JOBS=${JOBS:-4}
AS=third_party/antistatic/runtime
[ -f gen/funcs.h ] || { echo "needs gen/ (run make first)"; exit 1; }
mkdir -p "$OUT/obj" "$OUT/vkinc"
ln -sfn /usr/include/vulkan "$OUT/vkinc/vulkan"        # (Vulkan's headers only, not the rest of /usr/include)
ln -sfn /usr/include/vk_video "$OUT/vkinc/vk_video"
export ZIG_GLOBAL_CACHE_DIR=$OUT/zig-cache ZIG_LOCAL_CACHE_DIR=$OUT/zig-cache
WCC="$ZIG cc -target x86_64-windows-gnu"
INC="-Iruntime -I$AS/win32 -I$AS/d3d7 -I$AS/cpu -Igen -I$SDL3_WIN/include -I$OUT/vkinc"
# the generated C as the player's build compiles it (launcher.c), the runtime with the Windows host
GEN_FLAGS="-std=gnu11 -O2 -w -DCPU_FAST_MEM -fno-strict-aliasing -fno-omit-frame-pointer -fno-slp-vectorize $INC"
RT_FLAGS="-std=gnu11 -O2 -w -g -fno-strict-aliasing -fno-omit-frame-pointer $INC -I$AS/win -include $AS/win/compat.h -I$ZLIB_WIN/include"
{
    for f in gen/code_*.c gen/table.c gen/stubs.c; do echo "gen $f"; done
    for f in runtime/*.c $AS/win32/*.c $AS/d3d7/*.c $AS/cpu/fpu.c $AS/win/platform.c; do echo "rt $f"; done
} > "$OUT/sources.txt"
compile() {   # KIND FILE
    local o="$OUT/obj/$(basename "$(dirname "$2")")_$(basename "$2" .c).o"
    if [ "$1" = gen ]; then $WCC $GEN_FLAGS -c "$2" -o "$o"; else $WCC $RT_FLAGS -c "$2" -o "$o"; fi
}
export -f compile; export WCC GEN_FLAGS RT_FLAGS OUT
xargs -P "$JOBS" -L 1 bash -c 'compile "$0" "$1" 2>>"$OUT/build.log" || { echo "did not compile: $1"; exit 255; }' < "$OUT/sources.txt"
# Vulkan's loader: an import library for the vk* functions the objects call (vulkan-1.dll exports them)
{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"
  nm -u "$OUT"/obj/*.o | awk '$1 == "U" && $2 ~ /^vk[A-Z]/ {print $2}' | sort -u; } > "$OUT/vulkan-1.def"
"$ZIG" dlltool -d "$OUT/vulkan-1.def" -l "$OUT/libvulkan-1.a" -m i386:x86-64
$WCC -o "$OUT/1nsane.exe" "$OUT"/obj/*.o -static "$SDL3_WIN/lib/libSDL3.a" "$ZLIB_WIN/lib/libz.a" "$OUT/libvulkan-1.a" \
    -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lcfgmgr32 \
    -lshell32 -lpsapi -ldbghelp -ldinput8 -Wl,--stack,8388608 >> "$OUT/build.log" 2>&1
ls -la "$OUT/1nsane.exe"
