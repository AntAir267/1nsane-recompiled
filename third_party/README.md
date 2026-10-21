Third-party sources fetched and built locally (not committed):

- SDL3 (zlib licence): official source release from github.com/libsdl-org/SDL, unpacked here
  as SDL3-<version>/ and installed into sdl3/ (static library + headers) by the build.
- NVIDIA DLSS SDK 310.9.1 (NVIDIA RTX SDKs licence, DLSS/LICENSE.txt): cloned from
  github.com/NVIDIA/DLSS at tag v310.9.1 into DLSS/ (its Linux and ARM libraries and git history
  can be removed: DLSS/SOURCE.txt). The build
  takes its headers, lib/Windows_x86_64/x64/nvsdk_ngx_d.lib (linked with lld, runtime/dlss.h) and
  copies lib/Windows_x86_64/rel/nvngx_dlss.dll next to the program. Windows only so far; without
  the folder the game builds without DLSS (ANTI-ALIASING offers OFF, 4X and FSR).
- FidelityFX Super Resolution 2.2.1 (MIT licence, Advanced Micro Devices): source cloned from
  github.com/GPUOpen-Effects/FidelityFX-FSR2 at tag v2.2.1 into FidelityFX-FSR2/. Its tools/sc folder holds Windows programs; they are never run
  here: the shaders are compiled with the Linux Vulkan tools instead. The build takes only its
  API and Vulkan backend (src/ffx-fsr2-api, as AMD ships them; runtime/fsr2_compat.h stands in
  for two Windows library calls), with the shader permutations tools/fsr2_shaders.py compiles
  with glslangValidator into build/fsr2/. Without the folder the game builds without FSR (its
  ANTI-ALIASING offers OFF and 4X). A program built with it carries this notice:

      FidelityFX Super Resolution 2.2
      Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.

      Permission is hereby granted, free of charge, to any person obtaining a copy
      of this software and associated documentation files (the "Software"), to deal
      in the Software without restriction, including without limitation the rights
      to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
      copies of the Software, and to permit persons to whom the Software is
      furnished to do so, subject to the following conditions:

      The above copyright notice and this permission notice shall be included in
      all copies or substantial portions of the Software.

      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
      IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
      FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
      AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
      LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
      THE SOFTWARE.

For the player download (tools/release/linux.sh), also:

- Zig 0.17.0 (MIT licence): the official Linux x86_64 build from ziglang.org, unpacked as
  zig-x86_64-linux-0.17.0/. It builds the download, and the download carries it (without doc/) to
  compile the player's translation.
- SDL3 again, built with that Zig for glibc 2.31 into sdl3-rel/: rel-tools/ holds zcc, zcxx, zar
  and zranlib (`zig cc -target x86_64-linux-gnu.2.31`, and so on); configure SDL3-<version> in
  SDL3-build-rel/ with CMAKE_C_COMPILER, CMAKE_AR and CMAKE_RANLIB set to them,
  CMAKE_C_FLAGS="-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu",
  CMAKE_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu, SDL_SHARED=OFF, SDL_STATIC=ON,
  SDL_X11_XSCRNSAVER=OFF, CMAKE_BUILD_TYPE=Release, CMAKE_INSTALL_PREFIX=sdl3-rel; then build
  and install.
- python-standalone/: CPython 3.14.8 for any Linux with glibc 2.17 or later, from
  github.com/astral-sh/python-build-standalone (release 20261020,
  cpython-3.14.8+20261020-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz, 36,606,645
  bytes, sha256 b88a4ac994ac55c35a03a3672103618d5c5d5bf342db7b2093b8d328b6516b42, as its
  SHA256SUMS lists), unpacked as python/. The download carries it, without Tk and the test
  suite, to run the translator's Python files (this computer's own Python needs glibc 2.38).
- pack-venv/: a Python virtual environment with Capstone (5.0.9; its manylinux build needs
  glibc 2.14), whose package the download puts beside that Python, and PyInstaller (6.22).
- appimagetool-x86_64.AppImage, from github.com/AppImage/appimagetool (continuous release), and
  appimage-runtime-x86_64, the runtime it puts at the front of an AppImage (it fetches one from
  github.com/AppImage/type2-runtime each time otherwise; this one was taken from the front of
  the first AppImage it made, up to its --appimage-offset).
- innoextract 1.9 (zlib licence, Daniel Scharrer): the author's ready-made builds from
  github.com/dscharrer/innoextract (release 1.9: innoextract-1.9-linux.tar.xz and
  innoextract-1.9-windows.zip, signed with key 28555A66D7E1DEC9), in innoextract/: bin/amd64's
  program copied up as innoextract, the Windows one as innoextract.exe, and its license/ folder
  (the libraries built into it: Boost, liblzma, bzip2, zlib, libstdc++). It unpacks the GOG
  installer for the player without running it.

For the Windows download (tools/release/windows.sh, in MSYS2's UCRT64 shell), also:

- Zig 0.17.0: the official Windows x86_64 build from ziglang.org (zig-x86_64-windows-0.17.0.zip,
  sha256 b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428), unpacked as
  zig-x86_64-windows-0.17.0/.
- FreeType 2.15.0 (FreeType Licence): the source release from
  download.savannah.gnu.org/releases/freetype (freetype-2.15.0.tar.xz, sha256
  93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5), unpacked as
  freetype-2.15.0/. windows.sh builds the modules OpenType fonts need with Zig, for the runtime and
  the launcher (MSYS2's static FreeType needs HarfBuzz, which is C++, and GLib).
- FFmpeg 9.0.2 (LGPL 2.1): the source release from ffmpeg.org/releases (ffmpeg-9.0.2.tar.xz,
  sha256 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e), unpacked as
  ffmpeg-9.0.2/. windows.sh builds ffmpeg.exe from it with MSYS2's gcc, static, with only what the
  intro videos use (AVI, Indeo 5, PCM, raw output): 3.5 MB.
- innoextract as above: the Windows zip's innoextract.exe and license/ in innoextract/.
- PyInstaller: MSYS2's package (mingw-w64-ucrt-x86_64-pyinstaller, 6.22.3), with its Python and
  mingw-w64-ucrt-x86_64-python-capstone.
- downloads/: the archives above as they were fetched, and the sources windows.sh puts beside the
  zip for its LGPL parts: ffmpeg-9.0.2.tar.xz, and MSYS2's source packages for the libiconv and
  7-Zip builds it carries, from repo.msys2.org/mingw/sources with their .sig files
  (mingw-w64-libiconv-1.19-1.src.tar.zst, sha256
  74428280c17094da5b702c29b2e1a0abae59556ea5dfdd65705cc8ccc1e000fb;
  mingw-w64-7zip-26.04-1.src.tar.zst, sha256
  75a5d9f4e714f891b1e6204bf6f5b9a5740ba2247d41cf82478a670c47523bbe; both signed by MSYS2's key
  5F944B027F7FE2091985AA2EFA11531AA0AA7F57). A newer MSYS2 package needs its source package too.
