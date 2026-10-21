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
