#!/bin/bash
# tools/release/windows.sh OUT_DIR: the Windows download (a zip), with no part of the game in it.
#
# In MSYS2's UCRT64 shell (BUILDING.md), in a checkout of its own (a git worktree): its build/ becomes the
# release build. Like linux.sh it needs the user's Game.exe once (gen/funcs.h; FUNCS_SHA256), and the
# tools third_party/README.md lists for it.
#
# The runtime is built with Zig 0.17.0 (third_party/zig-x86_64-windows-0.17.0) for x86_64-windows-gnu,
# with the flags the players' Zig uses (launcher/launcher.c; linux.sh says why: no -frounding-math,
# -fno-slp-vectorize). It links MSYS2's static SDL3, zlib and libiconv (plain C), Vulkan's import library
# and FreeType, built here with the same Zig from its source (MSYS2's static one needs HarfBuzz, which
# is C++, and GLib). NVIDIA's DLSS link library goes in lib/ with empty stand-ins for the MSVC runtime
# libraries its objects name (zig cc has no -nodefaultlib). The zip is one folder: the launcher with its
# bundle beside it, and bin/ (DLSS's library; 7-Zip and libarchive for fan downloads; an ffmpeg built
# here with only what the intro videos need), which the launcher copies next to the game it builds.
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$(realpath -m "${1:?usage: windows.sh OUT_DIR}")
JOBS=${JOBS:-8}
TP=${TP:-$ROOT/third_party}        # (another checkout's, to share it with a worktree)
ZDIR=$TP/zig-x86_64-windows-0.17.0
Z=$ZDIR/zig.exe
T=x86_64-windows-gnu
DLSS=$TP/DLSS
FT=$TP/freetype-2.15.0
FF=$TP/ffmpeg-9.0.2
INNO=${INNO:-$TP/innoextract/innoextract.exe}
GIT=${GIT:-git}             # (MSYS2 may have none of its own: GIT="/c/Program Files/Git/cmd/git.exe")
for x in "$Z" "$FT/include/ft2build.h" "$FF/configure" "$INNO"; do
    [ -e "$x" ] || { echo "needs $x (third_party/README.md)"; exit 1; }
done
command -v pyinstaller >/dev/null || { echo "needs PyInstaller (pacman -S mingw-w64-ucrt-x86_64-pyinstaller)"; exit 1; }

R=$ROOT/build/rel
rm -rf "$R"; mkdir -p "$R/inc" "$R/lib" "$R/bin" "$R/ft" "$OUT"
echo "== the third-party headers and static libraries the runtime is built and linked with"
# (only these: MinGW's own C headers and libraries come with Zig)
cp -r /ucrt64/include/SDL3 /ucrt64/include/vulkan /ucrt64/include/vk_video /ucrt64/include/zlib.h \
      /ucrt64/include/zconf.h "$R/inc/"
cp /ucrt64/lib/libSDL3.a /ucrt64/lib/libz.a /ucrt64/lib/libiconv.a /ucrt64/lib/libuuid.a "$R/lib/"
# (Vulkan's import library under a static library's name: with -static, zig takes only those)
cp /ucrt64/lib/libvulkan-1.dll.a "$R/lib/libvulkan-1.a"
for n in MSVCRT OLDNAMES msvcprt; do ar rc "$R/lib/lib$n.a"; done

echo "== FreeType, with the modules OpenType fonts need (docs/INSTALL.ANY)"
cat > "$R/ft/ftmodule_min.h" <<'EOF'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
EOF
for s in base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbitmap base/ftglyph base/ftbbox base/ftmm \
         sfnt/sfnt truetype/truetype cff/cff psaux/psaux pshinter/pshinter psnames/psnames smooth/smooth \
         autofit/autofit gzip/ftgzip; do
    "$Z" cc -target $T -O2 -DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule_min.h>' -I"$R/ft" -I"$FT/include" \
        -c "$FT/src/$s.c" -o "$R/ft/$(basename $s).o"
done
"$Z" ar rcs "$R/lib/libfreetype.a" "$R"/ft/*.o
cp -r "$FT/include/ft2build.h" "$FT/include/freetype" "$R/inc/"
# the Makefile asks pkg-config for FreeType: this one answers with the build above
printf '#!/bin/sh\ncase "$*" in *--exists*) exit 0;; *--cflags*) echo "-I%s";; *--libs*) echo "-L%s -lfreetype";; esac\n' \
    "$R/inc" "$R/lib" > "$R/bin/pkg-config"
chmod +x "$R/bin/pkg-config"

echo "== the game built with Zig (the runtime is what the download carries; the rest checks it)"
rm -rf build/obj build/1nsane.exe       # (from scratch: make doesn't see changed flags)
COMMON="-std=gnu11 -g -fno-strict-aliasing -fno-omit-frame-pointer -Iruntime -Igen -I$R/inc"
SYSLIBS="-liconv -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi \
         -lshell32 -ldinput8 -lz -lpthread"
LIBS="-static $R/lib/libSDL3.a -L$R/lib -lfreetype $SYSLIBS -lvulkan-1 -lc++ -Wl,--stack,8388608 -mwindows"
[ -f "$DLSS/include/nvsdk_ngx_vk.h" ] && LIBS="$LIBS $DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
PATH="$R/bin:$PATH" make JOBS="$JOBS" CC="$Z cc -target $T" CXX="$Z c++ -target $T -I$R/inc" SDL3="$R" \
     CFLAGS_COMMON="$COMMON" CFLAGS_GEN="$COMMON -O2 -w -DCPU_FAST_MEM -fno-slp-vectorize" \
     LDFLAGS="$LIBS" HELPERS=
grep -q HAVE_FREETYPE <(PATH="$R/bin:$PATH" make -s --eval='pv: ; @echo $(CFLAGS_RT)' pv) || { echo "FreeType not found"; exit 1; }

echo "== a small ffmpeg: the intro videos are AVI with Indeo 5 and PCM, decoded to raw pictures and sound (video.c)"
FB=$ROOT/build/rel-ffmpeg
if [ ! -x "$FB/ffmpeg.exe" ]; then
    rm -rf "$FB"; mkdir -p "$FB"
    FFCONF="--disable-everything --disable-autodetect --disable-doc --disable-ffplay --disable-ffprobe
            --disable-network --disable-debug --disable-x86asm --enable-protocol=file,pipe --enable-demuxer=avi
            --enable-decoder=indeo5,pcm_s16le --enable-encoder=rawvideo,pcm_s16le --enable-muxer=rawvideo,pcm_s16le
            --enable-filter=scale,format,aresample,aformat,null,anull --extra-ldflags=-static"
    (cd "$FB" && "$FF/configure" $FFCONF > configure.log && make -j"$JOBS" ffmpeg.exe > make.log)
    echo "$FFCONF" | tr -s ' \n' ' ' > "$FB/configure-flags.txt"
fi

B=$OUT/1nsane-recompiled
rm -rf "$B"
mkdir -p "$B/lib" "$B/include" "$B/builds" "$B/bin" "$B/licenses"

echo "== the runtime"
RT_OBJS=$(ls build/obj/rt_*.o build/obj/fsr2_*.o | grep -v -e rt_main_nomain)
"$Z" ar rcs "$B/lib/libruntime.a" $RT_OBJS
cp "$R"/lib/*.a "$B/lib/"
[ -f "$DLSS/include/nvsdk_ngx_vk.h" ] && cp "$DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" "$B/lib/"
cp runtime/cpu.h runtime/runtime.h "$B/include/"
cp builds/gog_1_0.py builds/gog_1_0.manifest.sha256 "$B/builds/"
sha256sum gen/funcs.h | cut -c1-64 > "$B/FUNCS_SHA256"
REL=$(cat tools/release/VERSION)        # (the release: 0.1.1-alpha; the launcher shows it)
echo "1nsane-recompiled $REL $(date -u +%Y-%m-%d) $("$GIT" rev-parse --short HEAD) windows" > "$B/VERSION"
echo "== beside the game: DLSS's library, 7-Zip and libarchive (with the MSYS2 libraries they use), ffmpeg"
[ -f build/nvngx_dlss.dll ] && cp build/nvngx_dlss.dll "$B/bin/"
for x in /ucrt64/bin/7z.exe /ucrt64/bin/7z.dll /ucrt64/bin/libarchive-13.dll; do
    cp "$x" "$B/bin/"
    ldd "$x" | awk '/\/ucrt64\//{print $3}' | xargs -r cp -u -t "$B/bin/"
done
cp "$FB/ffmpeg.exe" "$B/bin/"

echo "== the translator, frozen"
# (without the network and OpenSSL, which the translator never uses)
pyinstaller --onefile --name translate --paths tools --distpath build/rel-dist --workpath build/rel-pyi \
    --specpath build/rel-pyi --log-level WARN --exclude-module ssl --exclude-module _ssl \
    --exclude-module _hashlib --exclude-module socket tools/recomp.py
cp build/rel-dist/translate.exe "$B/"
# the libraries packed into it (for the licences)
pyi-archive_viewer -l build/rel-dist/translate.exe | grep -oE '[A-Za-z0-9_.+-]+\.dll' | sort -u > "$R/translate-dlls.txt"

echo "== the launcher"
"$Z" cc -target $T -O2 -Igen -I"$R/inc" -o "$B/1nsane-recompiled.exe" launcher/launcher.c launcher/sha256.c \
    "$R/lib/libSDL3.a" -L"$R/lib" -lfreetype $SYSLIBS -static -mwindows
rm -f "$B/1nsane-recompiled.pdb"

echo "== Zig, the fan pack's list, innoextract, the licences"
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
cp "$INNO" "$B/innoextract.exe"
cp -r "$(dirname "$INNO")/license" "$B/innoextract-license"
L=$B/licenses
mkdir -p "$L/FreeType" "$L/FFmpeg" "$L/FidelityFX-FSR2" "$L/DLSS" "$L/Zig" "$L/Python"
cp "$FT/LICENSE.TXT" "$FT/docs/FTL.TXT" "$L/FreeType/"
cp "$FF/COPYING.LGPLv2.1" "$L/FFmpeg/"
{ echo "bin/ffmpeg.exe is FFmpeg 9.0.2 (LGPL 2.1 or later), built unchanged from its source release"
  echo "ffmpeg-9.0.2.tar.xz (from https://ffmpeg.org/releases/, and beside this download: SOURCES.txt) with:"
  echo "configure $(cat "$FB/configure-flags.txt")"; } > "$L/FFmpeg/SOURCE.txt"
# what MSYS2 built: bin/'s DLLs and programs (but our ffmpeg), lib/'s SDL3, zlib, libiconv, uuid, Vulkan's
# loader; each package's own licence files
for f in "$B"/bin/*; do
    n=$(basename "$f")
    [ "$n" = ffmpeg.exe ] || [ ! -e "/ucrt64/bin/$n" ] || echo "/ucrt64/bin/$n"
done > "$R/msys2-files.txt"
printf '/ucrt64/lib/%s\n' libSDL3.a libz.a libiconv.a libuuid.a libvulkan-1.dll.a >> "$R/msys2-files.txt"
sed 's#^#/ucrt64/bin/#' "$R/translate-dlls.txt" >> "$R/msys2-files.txt"      # (and translate.exe's)
for p in $(xargs pacman -Qqo < "$R/msys2-files.txt" | sort -u) mingw-w64-ucrt-x86_64-crt; do
    s=${p#mingw-w64-ucrt-x86_64-}
    if [ -d "/ucrt64/share/licenses/$s" ]; then mkdir -p "$L/$s"; cp -r "/ucrt64/share/licenses/$s/." "$L/$s/"
    else echo "(no licence files in MSYS2's $p)"; fi
done
pacman -Q $(xargs pacman -Qqo < "$R/msys2-files.txt" | sort -u) > "$L/msys2-packages.txt"
# lz4 ships none: its library's licence (lib/LICENSE in its source), BSD 2-clause
[ -d "$L/lz4" ] || { mkdir -p "$L/lz4"; cp "$ROOT/tools/release/lz4-LICENSE.txt" "$L/lz4/LICENSE"; }
# in translate.exe: Python, Capstone, PyInstaller's bootloader
cp /ucrt64/lib/python3.14/LICENSE.txt "$L/Python/"
mkdir -p "$L/capstone" "$L/pyinstaller"
cp -r /ucrt64/share/licenses/capstone/. "$L/capstone/"; cp -r /ucrt64/share/licenses/pyinstaller/. "$L/pyinstaller/"
# built into the game and the launcher: FSR 2, DLSS's notice, Zig's libc++/libc++abi/libunwind (its MinGW-w64: crt above)
cp "$TP/FidelityFX-FSR2/LICENSE.txt" "$L/FidelityFX-FSR2/"
cp "$DLSS/LICENSE.txt" "$L/DLSS/"
cp "$ZDIR/LICENSE" "$L/Zig/"
for d in libcxx libcxxabi libunwind; do cp "$ZDIR/lib/$d/LICENSE.TXT" "$L/Zig/$d-LICENSE.TXT"; done
cp CREDITS.md LICENSE "$L/"

echo "== the LGPL parts' exact sources, to publish beside the zip"
# FFmpeg's release, and MSYS2's source packages (recipe, patches and the original source) for the
# libiconv and 7-Zip builds the zip carries, in third_party/downloads (third_party/README.md)
ICONV=$(pacman -Q mingw-w64-ucrt-x86_64-libiconv | awk '{print $2}')
SEVENZ=$(pacman -Q mingw-w64-ucrt-x86_64-7zip | awk '{print $2}')
SRC=("ffmpeg-9.0.2.tar.xz" "mingw-w64-libiconv-$ICONV.src.tar.zst" "mingw-w64-7zip-$SEVENZ.src.tar.zst")
rm -rf "$OUT/sources"; mkdir -p "$OUT/sources"
for s in "${SRC[@]}"; do
    [ -f "$TP/downloads/$s" ] || { echo "needs third_party/downloads/$s (the source of what the zip carries)"; exit 1; }
    cp "$TP/downloads/$s" "$OUT/sources/"
    [ -f "$TP/downloads/$s.sig" ] && cp "$TP/downloads/$s.sig" "$OUT/sources/"
done
cat > "$L/SOURCES.txt" <<EOF
Three parts of this download are under the GNU LGPL, and their exact sources are published with it,
as files beside the zip on the release page it came from:

- bin/ffmpeg.exe, FFmpeg 9.0.2: ffmpeg-9.0.2.tar.xz (also https://ffmpeg.org/releases/), built as
  FFmpeg/SOURCE.txt says.
- GNU libiconv $ICONV (bin/libiconv-2.dll, and lib/libiconv.a, linked into the game on your
  computer): mingw-w64-libiconv-$ICONV.src.tar.zst, MSYS2's source package (its build recipe and
  patches with libiconv's source; also https://repo.msys2.org/mingw/sources/).
- 7-Zip $SEVENZ (bin/7z.exe, bin/7z.dll): mingw-w64-7zip-$SEVENZ.src.tar.zst, the same way.

The .sig files beside them are MSYS2's signatures.
EOF

echo "== zip"
ZIP=1NSANE_Recompiled-$REL-windows-x86_64.zip
rm -f "$OUT"/1NSANE_Recompiled-*windows-x86_64.zip
(cd "$OUT" && 7z a -tzip -mx=7 -bso0 -bsp0 "$ZIP" 1nsane-recompiled)
(cd "$OUT" && sha256sum "$ZIP" > SHA256SUMS && cd sources && sha256sum * >> ../SHA256SUMS)
ls -la "$OUT/$ZIP" "$OUT/sources"
