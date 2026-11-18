"""scene_cmp.py BASE NEW: compare a scenes.sh run (work/antistatic/scenes.sh) with the baselines.

For each scene in both folders:
- hashes: the first frame whose memory hash (and runtime call count) differs, and how many differ
  (a 4th column, the toolkit's draw-stream hash, is left out: the baselines have none);
- frame dumps: per frame, the pixels that differ and the largest channel difference. For the
  Vulkan scenes (fan_rt, fan_off) the baseline's own noise is fan_rt vs fan_rt2, printed beside.
"""
import os, sys
import numpy as np


def hashes(path):
    out = {}
    if os.path.exists(path):
        for line in open(path):
            f = line.split()
            if len(f) >= 3:
                out[int(f[0])] = (f[1], f[2])
    return out


def ppm(path):
    with open(path, 'rb') as fh:
        data = fh.read()
    parts, i = [], 0
    while len(parts) < 4:   # P6 W H MAX, whitespace separated
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data, np.uint8, w * h * 3, i).reshape(h, w, 3).astype(np.int16)


def dumpdiff(a, b):
    if not (os.path.exists(a) and os.path.exists(b)):
        return 'missing'
    x, y = ppm(a), ppm(b)
    if x.shape != y.shape:
        return 'size %s vs %s' % (x.shape, y.shape)
    d = np.abs(x - y)
    px = int((d.max(axis=2) > 0).sum())
    return 'same' if px == 0 else '%d px, max %d' % (px, int(d.max()))


base, new = sys.argv[1:3]
for scene in ('std_swrast', 'fan_rt', 'fan_rt2', 'fan_off'):
    hb, hn = hashes(os.path.join(base, scene + '.hash')), hashes(os.path.join(new, scene + '.hash'))
    if not hn:
        continue
    frames = sorted(set(hb) & set(hn))
    diff = [f for f in frames if hb[f][1] != hn[f][1]]
    # runtime calls: the same on every frame apart from a constant (the toolkit's start-up calls)
    off = int(hn[frames[0]][0]) - int(hb[frames[0]][0]) if frames else 0
    calls = [f for f in frames if int(hn[f][0]) - int(hb[f][0]) != off]
    print('%s: %d frames (base %d, new %d); memory differs at %d (first %s); calls: offset %+d from frame %s, '
          'other offsets at %d frames (first %s)'
          % (scene, len(frames), len(hb), len(hn), len(diff), diff[0] if diff else '-', off,
             frames[0] if frames else '-', len(calls), calls[0] if calls else '-'))
    dn = os.path.join(new, scene + '.dump')
    if not os.path.isdir(dn):
        continue
    for f in sorted(os.listdir(dn)):
        line = '  %s  new vs base: %s' % (f, dumpdiff(os.path.join(base, scene + '.dump', f), os.path.join(dn, f)))
        if scene in ('fan_rt', 'fan_rt2'):
            line += ' | base noise (fan_rt vs fan_rt2): %s' % dumpdiff(os.path.join(base, 'fan_rt.dump', f),
                                                                     os.path.join(base, 'fan_rt2.dump', f))
        print(line)
