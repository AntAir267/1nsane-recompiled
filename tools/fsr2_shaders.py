#!/usr/bin/env python3
"""FSR 2's Vulkan shaders for its Vulkan backend (third_party/FidelityFX-FSR2, v2.2.1).

The backend (src/ffx-fsr2-api/vk/shaders/ffx_fsr2_shaders_vk.cpp) includes a header per pass,
<pass>_permutations.h, that AMD's FidelityFX_SC (a Windows program, never run here) writes: a
union PermutationKey of the options' bits, a table PermutationInfo of SPIR-V blobs with their
resources' names and bindings, and an IndirectionTable from a key's index to a row of it. This
writes the same headers with glslangValidator and a small SPIR-V reader for the bindings.

Only the permutations the port can ask for are compiled: LDR colour, render-size motion vectors
without jitter, depth not inverted (vk.c's scene), and the device-dependent ones: the Lanczos
lookup table or not (the subgroup size), 16-bit maths or not, sharpening or not. The other keys
map to -1 (the port never asks for them).

The history's reprojection always uses the reference Lanczos (REPROJECT_USE_LANCZOS_TYPE 0), also
where the backend asks for the lookup table (on 64-wide subgroups, as AMD's iGPUs report): with the
table the reprojected history came out heavily blurred, frame after frame (a still picture at
QUALITY had a third of OFF's detail, less than a plain bilinear upscale), its edges ghosted and
moving with the jitter, a fifth of a pixel each frame: the "heat-haze" shimmer. The table's values
do not reach the shader (the formula in place of its read: sharp and steady; a constant: the same
blur and wobble). The reference weights cost about 0.4 ms a frame at QUALITY on a Radeon 890M
(DESIGN.md decision 4, temporal anti-aliasing).

usage: fsr2_shaders.py FSR2_API_DIR OUT_DIR [glslangValidator]"""
import os
import struct
import subprocess
import sys

PASSES = ['ffx_fsr2_tcr_autogen_pass', 'ffx_fsr2_autogen_reactive_pass', 'ffx_fsr2_accumulate_pass',
          'ffx_fsr2_compute_luminance_pyramid_pass', 'ffx_fsr2_depth_clip_pass', 'ffx_fsr2_lock_pass',
          'ffx_fsr2_reconstruct_previous_depth_pass', 'ffx_fsr2_rcas_pass']
# the key's bits, in the order of FidelityFX_SC's permutation arguments (CMakeLists.txt)
OPTIONS = ['FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE', 'FFX_FSR2_OPTION_HDR_COLOR_INPUT',
           'FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS', 'FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS',
           'FFX_FSR2_OPTION_INVERTED_DEPTH', 'FFX_FSR2_OPTION_APPLY_SHARPENING', 'FFX_HALF']
FIXED = {'FFX_FSR2_OPTION_HDR_COLOR_INPUT': 0, 'FFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS': 1,
         'FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS': 0, 'FFX_FSR2_OPTION_INVERTED_DEPTH': 0}
BASE = ['-DFFX_GPU=1', '-DFFX_GLSL=1',
        '-DFFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
        '-DFFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
        '-DFFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
        '-DFFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
        '-DFFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2']


def reflect(spv):
    """the set-1 resources a SPIR-V module's code uses: (sampled images, storage images, uniform
    buffers), each a list of (name, binding). (Declared but unused ones are left out, as AMD's
    compiler does: the backend would bind them too, an image as a sampled one after its storage
    use, in the wrong layout.)"""
    words = struct.unpack('<%dI' % (len(spv) // 4), spv)
    names, binding, dset, ptr, image, var, used = {}, {}, {}, {}, {}, {}, set()
    i, infunc = 5, False
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        a = words[i + 1:i + n]
        if op == 54:                                    # OpFunction .. OpFunctionEnd: what the code names
            infunc = True
        elif op == 56:
            infunc = False
        elif infunc:
            used.update(a)
        if op == 5:                                     # OpName
            s = struct.pack('<%dI' % (n - 2), *a[1:])
            names[a[0]] = s.split(b'\0')[0].decode()
        elif op == 71 and a[1] == 33:                   # OpDecorate Binding
            binding[a[0]] = a[2]
        elif op == 71 and a[1] == 34:                   # OpDecorate DescriptorSet
            dset[a[0]] = a[2]
        elif op == 32:                                  # OpTypePointer
            ptr[a[0]] = (a[1], a[2])
        elif op == 25:                                  # OpTypeImage: sampled 1, storage 2
            image[a[0]] = a[6]
        elif op == 59:                                  # OpVariable
            var[a[1]] = (a[0], a[2])
        i += n
    srv, uav, cb = [], [], []
    for v, (t, sc) in var.items():
        if v not in binding or dset.get(v) != 1 or v not in used:
            continue
        pointee = ptr.get(t, (None, None))[1]
        entry = (names.get(v, ''), binding[v])
        if sc == 0 and pointee in image:
            (srv if image[pointee] == 1 else uav).append(entry)
        elif sc == 2:
            cb.append(entry)
    return sorted(srv, key=lambda e: e[1]), sorted(uav, key=lambda e: e[1]), sorted(cb, key=lambda e: e[1])


def main():
    api, out = sys.argv[1], sys.argv[2]
    glslang = sys.argv[3] if len(sys.argv) > 3 else 'glslangValidator'
    shaders = os.path.join(api, 'shaders')
    os.makedirs(out, exist_ok=True)
    tmp = os.path.join(out, 'tmp.spv')
    for p in PASSES:
        half = [0] if p == 'ffx_fsr2_compute_luminance_pyramid_pass' else [0, 1]
        rows, table = [], [-1] * (1 << len(OPTIONS))
        for lanczos in (0, 1):
            for sharpen in (0, 1):
                for h in half:
                    opts = dict(FIXED)
                    opts.update({'FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE': lanczos,
                                 'FFX_FSR2_OPTION_APPLY_SHARPENING': sharpen, 'FFX_HALF': h})
                    built = dict(opts, FFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE=0)   # (the reference: above)
                    cmd = [glslang, '--target-env', 'vulkan1.1', '-S', 'comp', '-e', 'main', '-I' + shaders] + BASE + \
                          ['-D%s=%d' % (k, built[k]) for k in OPTIONS] + ['-o', tmp, os.path.join(shaders, p + '.glsl')]
                    r = subprocess.run(cmd, capture_output=True, text=True)
                    if r.returncode:
                        sys.exit('fsr2_shaders: %s failed:\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
                    spv = open(tmp, 'rb').read()
                    key = sum(opts[k] << b for b, k in enumerate(OPTIONS))
                    table[key] = len(rows)
                    rows.append((spv, reflect(spv)))
        os.remove(tmp)
        h = ['/* %s permutations for FSR 2\'s Vulkan backend: written by tools/fsr2_shaders.py */' % p,
             '#pragma once', '#include <stdint.h>', '',
             'typedef union %s_PermutationKey {' % p, '    struct {']
        h += ['        uint32_t %s : 1;' % k for k in OPTIONS]
        h += ['    };', '    uint32_t index;', '} %s_PermutationKey;' % p, '',
              'typedef struct %s_PermutationInfo {' % p,
              '    const uint32_t blobSize;', '    const unsigned char* blobData;',
              '    const uint32_t numStorageImageResources;', '    const char** storageImageResourceNames;',
              '    const uint32_t* storageImageResourceBindings;',
              '    const uint32_t numSampledImageResources;', '    const char** sampledImageResourceNames;',
              '    const uint32_t* sampledImageResourceBindings;',
              '    const uint32_t numUniformBufferResources;', '    const char** uniformBufferResourceNames;',
              '    const uint32_t* uniformBufferResourceBindings;', '} %s_PermutationInfo;' % p, '']
        for n, (spv, (srv, uav, cb)) in enumerate(rows):
            h.append('static const unsigned char g_%s_blob_%d[] = {' % (p, n))
            for o in range(0, len(spv), 24):
                h.append('    ' + ','.join(str(b) for b in spv[o:o + 24]) + ',')
            h.append('};')
            for tag, lst in (('uav', uav), ('srv', srv), ('cb', cb)):
                if lst:
                    h.append('static const char* g_%s_%s_names_%d[] = {%s};' % (
                        p, tag, n, ', '.join('"%s"' % e[0] for e in lst)))
                    h.append('static const uint32_t g_%s_%s_bindings_%d[] = {%s};' % (
                        p, tag, n, ', '.join(str(e[1]) for e in lst)))
        h.append('')
        h.append('static const %s_PermutationInfo g_%s_PermutationInfo[] = {' % (p, p))
        for n, (spv, (srv, uav, cb)) in enumerate(rows):
            f = []
            for tag, lst in (('uav', uav), ('srv', srv), ('cb', cb)):
                f += [str(len(lst)), 'g_%s_%s_names_%d' % (p, tag, n) if lst else '0',
                      'g_%s_%s_bindings_%d' % (p, tag, n) if lst else '0']
            h.append('    {%d, g_%s_blob_%d, %s},' % (len(spv), p, n, ', '.join(f)))
        h.append('};')
        h.append('static const int32_t g_%s_IndirectionTable[] = {%s};' % (p, ', '.join(map(str, table))))
        with open(os.path.join(out, p + '_permutations.h'), 'w', newline='\n') as f:
            f.write('\n'.join(h) + '\n')
        print('%s: %d permutations' % (p, len(rows)))


main()
