#!/bin/bash
# tools/release/windows.sh OUT_DIR: the Windows download (a zip), with no part of the game in it.
#
# In MSYS2's UCRT64 shell (BUILDING.md), in a checkout of its own (a git worktree): its build/ becomes the
# release build. Like linux.sh it needs the user's Game.exe once (gen/funcs.h; FUNCS_SHA256).
#
# The runtime is built with Zig 0.17.0 (third_party/zig-x86_64-windows-0.17.0) for x86_64-windows-gnu,
# with the flags the players' Zig uses (launcher/launcher.c; linux.sh says why: no -frounding-math,
# -fno-slp-vectorize). It links MSYS2's static SDL3, zlib and libiconv (plain C) and Vulkan's import
# library; NVIDIA's DLSS link library goes in lib/ with empty stand-ins for the MSVC runtime libraries
# its objects name (zig cc has no -nodefaultlib). The zip is one folder: the launcher with its bundle
# beside it, and bin/ (DLSS's library, 7-Zip and libarchive for fan downloads), which the launcher copies
# next to the game it builds.
#
# Not in it yet: FreeType (MSYS2's static one needs HarfBuzz, which is C++, and GLib: it wants a Zig build
# from source; until then the runtime goes without, and FONT keeps the game's letters), ffmpeg (the
# intro videos; MSYS2's needs 170 MB of libraries), and without them in third_party/ (third_party/README.md)
# innoextract (GOG installers; only a folder works) and the frozen translator (PyInstaller in pack-venv;
# TRANSLATE=<exe> gives one).
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$(realpath -m "${1:?usage: windows.sh OUT_DIR}")
JOBS=${JOBS:-8}
ZDIR=$ROOT/third_party/zig-x86_64-windows-0.17.0
Z=$ZDIR/zig.exe
T=x86_64-windows-gnu
DLSS=$ROOT/third_party/DLSS
[ -x "$Z" ] || { echo "needs third_party/zig-x86_64-windows-0.17.0"; exit 1; }

R=$ROOT/build/rel
rm -rf "$R"; mkdir -p "$R/inc" "$R/lib" "$R/bin" "$OUT"
echo "== the third-party headers and static libraries the runtime is built and linked with"
# (only these: MinGW's own C headers and libraries come with Zig)
cp -r /ucrt64/include/SDL3 /ucrt64/include/vulkan /ucrt64/include/vk_video /ucrt64/include/zlib.h \
      /ucrt64/include/zconf.h "$R/inc/"
cp /ucrt64/lib/libSDL3.a /ucrt64/lib/libz.a /ucrt64/lib/libiconv.a /ucrt64/lib/libuuid.a "$R/lib/"
# (Vulkan's import library under a static library's name: with -static, zig takes only those)
cp /ucrt64/lib/libvulkan-1.dll.a "$R/lib/libvulkan-1.a"
for n in MSVCRT OLDNAMES msvcprt; do ar rc "$R/lib/lib$n.a"; done
# no FreeType yet: a pkg-config that finds nothing
printf '#!/bin/sh\nexit 1\n' > "$R/bin/pkg-config"; chmod +x "$R/bin/pkg-config"

echo "== the game built with Zig (the runtime is what the download carries; the rest checks it)"
COMMON="-std=gnu11 -g -fno-strict-aliasing -fno-omit-frame-pointer -Iruntime -Igen -I$R/inc"
LIBS="-static $R/lib/libSDL3.a -L$R/lib -liconv -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion \
      -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -lz -lpthread -lvulkan-1 -lc++ -Wl,--stack,8388608 -mwindows"
[ -f "$DLSS/include/nvsdk_ngx_vk.h" ] && LIBS="$LIBS $DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
PATH="$R/bin:$PATH" make JOBS="$JOBS" CC="$Z cc -target $T" CXX="$Z c++ -target $T -I$R/inc" SDL3="$R" \
     CFLAGS_COMMON="$COMMON" CFLAGS_GEN="$COMMON -O2 -w -DCPU_FAST_MEM -fno-slp-vectorize" \
     LDFLAGS="$LIBS" HELPERS=
rm "$R/bin/pkg-config"

B=$OUT/1nsane-recompiled
rm -rf "$B"
mkdir -p "$B/lib" "$B/include" "$B/builds" "$B/bin"

echo "== the runtime"
RT_OBJS=$(ls build/obj/rt_*.o build/obj/fsr2_*.o | grep -v -e rt_main_nomain)
"$Z" ar rcs "$B/lib/libruntime.a" $RT_OBJS
cp "$R"/lib/*.a "$B/lib/"
[ -f "$DLSS/include/nvsdk_ngx_vk.h" ] && cp "$DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" "$B/lib/"
cp runtime/cpu.h runtime/runtime.h "$B/include/"
cp builds/gog_1_0.py builds/gog_1_0.manifest.sha256 "$B/builds/"
sha256sum gen/funcs.h | cut -c1-64 > "$B/FUNCS_SHA256"
GIT=${GIT:-git}             # (MSYS2 may have none of its own: GIT="/c/Program Files/Git/cmd/git.exe")
echo "1nsane-recompiled $(date -u +%Y-%m-%d) $("$GIT" rev-parse --short HEAD) windows" > "$B/VERSION"

echo "== beside the game: DLSS's library, 7-Zip and libarchive (with the MSYS2 libraries they use)"
[ -f build/nvngx_dlss.dll ] && cp build/nvngx_dlss.dll "$B/bin/"
for x in /ucrt64/bin/7z.exe /ucrt64/bin/7z.dll /ucrt64/bin/libarchive-13.dll; do
    cp "$x" "$B/bin/"
    ldd "$x" | awk '/\/ucrt64\//{print $3}' | xargs -r cp -u -t "$B/bin/"
done

echo "== the translator, frozen"
if [ -n "${TRANSLATE:-}" ]; then cp "$TRANSLATE" "$B/translate.exe"
elif [ -x "$ROOT/third_party/pack-venv/Scripts/pyinstaller.exe" ]; then
    "$ROOT/third_party/pack-venv/Scripts/pyinstaller.exe" --onefile --name translate --paths tools --distpath build/rel-dist \
        --workpath build/rel-pyi --specpath build/rel-pyi --log-level WARN tools/recomp.py
    cp build/rel-dist/translate.exe "$B/"
else echo "(no translator: set TRANSLATE or put PyInstaller in third_party/pack-venv)"; fi

echo "== the launcher (MSYS2's gcc, with its static SDL3 and FreeType: it is never rebuilt by players)"
gcc -O2 -static -Igen -o "$B/1nsane-recompiled.exe" launcher/launcher.c launcher/sha256.c \
    /ucrt64/lib/libSDL3.a $(pkg-config --static --cflags --libs freetype2) -liconv -luser32 -lgdi32 -lwinmm \
    -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -lpthread -lstdc++ -mwindows

echo "== Zig, the fan pack's list, innoextract"
cp -a "$ZDIR" "$B/zig"
rm -rf "$B/zig/doc"
PACK=https://github.com/AntAir267/1nsane-recompiled-fan-pack/releases/download/v1
{ echo "url $PACK"
  sums=$(curl -fsSL "$PACK/SHA256SUMS")
  rel=$(curl -fsSL https://api.github.com/repos/AntAir267/1nsane-recompiled-fan-pack/releases/tags/v1)
  for f in fan-cars.tar fan-maps.tar; do
      size=$(printf '%s' "$rel" | python -c "import json,sys; print(next(a['size'] for a in json.load(sys.stdin)['assets'] if a['name']==sys.argv[1]))" "$f")
      echo "$(printf '%s\n' "$sums" | grep " $f\$" | cut -c1-64) $size $f"
  done; } > "$B/fanpack.txt"
INNO=${INNO:-$ROOT/third_party/innoextract/innoextract.exe}     # (the author's build: third_party/README.md)
if [ -x "$INNO" ]; then
    cp "$INNO" "$B/innoextract.exe"
    cp -r "$(dirname "$INNO")/license" "$B/innoextract-license"
else echo "(no innoextract: the GOG installer can't be unpacked, only a folder)"; fi

echo "== zip"
rm -f "$OUT/1NSANE_Recompiled-windows-x86_64.zip"
(cd "$OUT" && 7z a -tzip -mx=7 -bso0 -bsp0 1NSANE_Recompiled-windows-x86_64.zip 1nsane-recompiled)
ls -la "$OUT/1NSANE_Recompiled-windows-x86_64.zip"
