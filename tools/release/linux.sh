#!/bin/bash
# tools/release/linux.sh OUT_DIR: the Linux download (an AppImage), with no part of the game in it.
#
# Run it in a checkout of its own (a git worktree): its build/ becomes the release build. It needs the
# user's Game.exe once, as every build does: the runtime is compiled against the declarations the
# translator writes (gen/funcs.h), and the players' translation must give the same (FUNCS_SHA256).
#
# Everything is built with Zig 0.17.0 for glibc 2.31 (third_party/zig-*, SDL3 from third_party/sdl3-rel,
# built as third_party/README.md says), so the download runs on Linux from 2020 on. The players' Zig
# compiles their translation with the same flags (launcher/launcher.c), without -frounding-math (clang's
# strict floating point is half as fast; the game's results are the same without it: lockstep, hashes
# and fncheck) and with -fno-slp-vectorize (clang otherwise merges the memory helpers' stores).
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$(realpath -m "${1:?usage: linux.sh OUT_DIR}")
JOBS=${JOBS:-4}
ZDIR=$ROOT/third_party/zig-x86_64-linux-0.17.0
Z=$ZDIR/zig
SDL=$ROOT/third_party/sdl3-rel
T=x86_64-linux-gnu.2.31
LIBDIR=/usr/lib/x86_64-linux-gnu
INNO=${INNO:-$ROOT/third_party/innoextract/innoextract}
[ -x "$Z" ] && [ -f "$SDL/lib/libSDL3.a" ] || { echo "needs third_party/zig-x86_64-linux-0.17.0 and third_party/sdl3-rel"; exit 1; }

TOOLS=$ROOT/build/rel-tools
mkdir -p "$TOOLS" "$OUT"
printf '#!/bin/sh\nexec %s cc -target %s "$@"\n' "$Z" "$T" > "$TOOLS/zcc"
printf '#!/bin/sh\nexec %s c++ -target %s "$@"\n' "$Z" "$T" > "$TOOLS/zcxx"
chmod +x "$TOOLS/zcc" "$TOOLS/zcxx"
SYSINC="-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu"
COMMON="-std=gnu11 -g -fno-strict-aliasing -fno-omit-frame-pointer -Iruntime -Igen -I$SDL/include $SYSINC"

echo "== the game built with Zig (the runtime is what the download carries; the rest checks it)"
make JOBS="$JOBS" CC="$TOOLS/zcc -fPIE -pie -L$LIBDIR -Wl,--allow-shlib-undefined" \
     CXX="$TOOLS/zcxx -fPIE $SYSINC" SDL3="$SDL" CFLAGS_COMMON="$COMMON" \
     CFLAGS_GEN="$COMMON -O2 -w -DCPU_FAST_MEM -fno-slp-vectorize"

B=$OUT/AppDir/usr/lib/1nsane-recompiled
rm -rf "$OUT/AppDir"
mkdir -p "$B/lib/stubs" "$B/include" "$B/builds" "$OUT/AppDir/usr/bin"

echo "== the runtime"
RT_OBJS=$(ls build/obj/rt_*.o build/obj/fsr2_*.o | grep -v -e rt_main_nomain -e rt_platform_win)
"$Z" ar rcs "$B/lib/libruntime.a" $RT_OBJS
cp "$SDL/lib/libSDL3.a" "$B/lib/"
cp runtime/cpu.h runtime/runtime.h "$B/include/"
cp builds/gog_1_0.py builds/gog_1_0.manifest.sha256 "$B/builds/"
sha256sum gen/funcs.h | cut -c1-64 > "$B/FUNCS_SHA256"
echo "1nsane-recompiled $(date -u +%Y-%m-%d) $(git rev-parse --short HEAD)" > "$B/VERSION"

echo "== link-time stand-ins for the system's Vulkan, FreeType and zlib (the players' own are used)"
for spec in vulkan:libvulkan.so.1 freetype:libfreetype.so.6 z:libz.so.1; do
    name=${spec%%:*} so=${spec#*:}
    nm -D --defined-only "$LIBDIR/$so" | awk '$2 ~ /^[TWi]$/ {print $3}' | sed 's/@.*//' | sort -u |
        sed 's/.*/void &(void) {}/' > "build/stub_$name.c"     # (its functions: only names matter)
    "$TOOLS/zcc" -shared -fPIC -o "$B/lib/stubs/lib$name.so" -Wl,-soname,"$so" "build/stub_$name.c"
done

echo "== the translator: its Python files on a standalone Python (glibc 2.17) with Capstone"
PYS=$ROOT/third_party/python-standalone/python
[ -x "$PYS/bin/python3.14" ] || { echo "needs third_party/python-standalone (third_party/README.md)"; exit 1; }
mkdir -p "$B/python/bin" "$B/python/lib" "$B/tools"
cp -a "$PYS/bin/python3.14" "$B/python/bin/"            # (all of Python in one program: no libpython)
tar -C "$PYS/lib" --exclude=test --exclude=idlelib --exclude=tkinter --exclude=turtledemo --exclude=ensurepip \
    --exclude=pydoc_data --exclude='config-3.14-*' --exclude='site-packages/*' --exclude='_tkinter.*' \
    --exclude='_dbm.*' -cf - python3.14 | tar -C "$B/python/lib" -xf -
cp -r "$ROOT/third_party/pack-venv/lib/python3.14/site-packages/capstone" "$B/python/lib/python3.14/site-packages/"
find "$B/python/lib/python3.14/site-packages/capstone" -name __pycache__ -prune -exec rm -r {} +
cp tools/recomp.py tools/codegen.py tools/discover.py tools/pe.py tools/x86.py tools/fpustack.py "$B/tools/"
cat > "$B/translate" <<'EOF2'
#!/bin/sh
# Game.exe to C: tools/recomp.py on the Python beside it (-E -s: none of the player's Python settings)
H=$(dirname "$(readlink -f "$0")")
exec "$H/python/bin/python3.14" -E -s -B "$H/tools/recomp.py" "$@"
EOF2
chmod +x "$B/translate"

echo "== the launcher"
"$TOOLS/zcc" -O2 -fPIE -pie -I"$SDL/include" -Igen -I/usr/include/freetype2 $SYSINC -o "$OUT/AppDir/usr/bin/1nsane-recompiled" \
    launcher/launcher.c launcher/sha256.c "$SDL/lib/libSDL3.a" -L"$B/lib/stubs" -lfreetype -lm -ldl -pthread \
    -Wl,--allow-shlib-undefined

echo "== Zig, the fan pack's list, innoextract"
cp -a "$(readlink -f "$ZDIR")" "$B/zig"
rm -rf "$B/zig/doc"
{ echo "url https://github.com/AntAir267/1nsane-recompiled-fan-pack/releases/download/v1"
  for f in fan-cars.tar fan-maps.tar; do
      line=$(gh release view v1 --repo AntAir267/1nsane-recompiled-fan-pack --json assets \
             --jq ".assets[] | select(.name==\"$f\") | \"\(.size)\"")
      sha=$(curl -fsSL "https://github.com/AntAir267/1nsane-recompiled-fan-pack/releases/download/v1/SHA256SUMS" | grep " $f\$" | cut -c1-64)
      echo "$sha $line $f"
  done; } > "$B/fanpack.txt"
[ -x "$INNO" ] || { echo "needs third_party/innoextract (third_party/README.md): it unpacks the GOG installer"; exit 1; }
cp "$INNO" "$B/innoextract"

echo "== the licences (every part the download carries; CREDITS.md names them)"
L=$B/licenses
mkdir -p "$L"
cp CREDITS.md LICENSE "$L/"
cp assets/1nsanefont/README.md "$L/1nsaneFont-README.md"
cp third_party/SDL3-*/LICENSE.txt "$L/SDL3-LICENSE.txt"
cp third_party/FidelityFX-FSR2/LICENSE.txt "$L/FidelityFX-FSR2-LICENSE.txt"
cp "$ZDIR/LICENSE" "$L/Zig-LICENSE.txt"
for x in libcxx libcxxabi libunwind; do cp "$ZDIR/lib/$x/LICENSE.TXT" "$L/LLVM-$x-LICENSE.txt"; done
cp -r "$(dirname "$INNO")/license" "$L/innoextract"
cp "$ROOT"/third_party/pack-venv/lib/python3.14/site-packages/capstone-*.dist-info/LICENSE.TXT "$L/Capstone-LICENSE.txt"
cp "$PYS/lib/python3.14/LICENSE.txt" "$L/Python-LICENSE.txt"
cp /usr/share/doc/libpython3.14/copyright "$L/Python-copyright-details.txt"
cp tools/release/python-builtins-NOTICE.txt "$L/Python-builtins-NOTICE.txt"
for x in openssl:libssl3t64 expat:libexpat1 zlib:zlib1g bzip2:libbz2-1.0 xz:liblzma5 zstd:libzstd1 \
         sqlite:libsqlite3-0 libedit:libedit2 ncurses:libncurses6 util-linux:libuuid1; do
    cp "/usr/share/doc/${x#*:}/copyright" "$L/${x%%:*}-copyright.txt"
done

echo "== AppImage"
A=$OUT/AppDir
cat > "$A/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/1nsane-recompiled" "$@"
EOF
chmod +x "$A/AppRun"
cat > "$A/1nsane-recompiled.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=1NSANE: Recompiled
Comment=The 2000 off-road racer, rebuilt from your own GOG copy
Exec=1nsane-recompiled
Icon=1nsane-recompiled
Categories=Game;SportsGame;
EOF
cp assets/icon/1nsane-recompiled.png "$A/1nsane-recompiled.png"
RUNTIME=()                  # (appimagetool fetches its runtime each time unless it is given one)
[ -f "$ROOT/third_party/appimage-runtime-x86_64" ] && RUNTIME=(--runtime-file "$ROOT/third_party/appimage-runtime-x86_64")
ARCH=x86_64 "$ROOT/third_party/appimagetool-x86_64.AppImage" --no-appstream "${RUNTIME[@]}" "$A" "$OUT/1NSANE_Recompiled-x86_64.AppImage" >/dev/null
ls -la "$OUT/1NSANE_Recompiled-x86_64.AppImage"
