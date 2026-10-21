# lamp_check.py OUTDIR IDF...: each fan car's headlights against its own model, as HEADLIGHTS places them in the
# game (runtime/headlights.c "Where a car's headlights are"). From its .vht (plain text; the stock cars' and some
# fan cars' are scrambled: "unread"): the [files] object .gem (its first .fff: the body at the highest detail, its
# POS chunk's vertices) and the [lights] lines. Its own lamps (front and rear lines, lit at night; in the body's
# front quarter; the lowest group; the outermost pair) if on the body (within 12 units of a vertex, none in front of
# its nose), else from the body's shape. Reports per car: flags (ok; none: no lamp in front; off: one more than 12
# units from any vertex; ahead: in front of the nose; nobody: no body mesh found), the source, the result, the
# body's box, the old guess (the physics points' front - 8, two fifths up; here the body's box stands in for the
# points, which are encrypted in the .vhx). Writes OUTDIR/<car>.ppm: front (x, y) and side (z, y; the front on the
# left) views of the body's vertices, the lamps used red, the car's other lamps blue, the old guess green.
# Usage: python tools/lamp_check.py work/lampcheck "$APPDATA"/1nsane-native/modcache/Vehicles/*.idf
import os, re, struct, sys
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def members(path):
    d = open(path, 'rb').read()
    if d[:4] != b'FFFL':
        return d, {}
    ver, off, flags = struct.unpack_from('<III', d, 8)
    if ver == 0x10100:
        off ^= 0x123
    else:
        flags = 0
    count = struct.unpack_from('<I', d, off)[0]
    ent = bytearray(d[off + 4:off + 4 + 64 * count])
    key = ((flags & 0xff) + 0x27) & 0xff
    if flags >> 24 == 1:
        for i in range(len(ent)):
            ent[i] ^= key
            key = (key * 5 + ent[i] + i) & 0xff
    m = {}
    for i in range(count):
        e = ent[64 * i:64 * i + 64]
        name = e[:0x38].split(b'\0')[0].decode('latin-1').replace('\\', '/').lower()
        a, b = struct.unpack_from('<II', e, 0x38)
        m[name.split('/')[-1]] = (a, b)
    return d, m

def get(d, m, name):
    e = m.get(name.lower())
    return d[e[0]:e[0] + e[1]] if e else None

def fff_verts(b):
    o = 0
    while o + 8 <= len(b):
        tag, n = b[o:o + 4], struct.unpack_from('<I', b, o + 4)[0]
        if tag in (b'FFF\0', b'LOD '):
            v = fff_verts(b[o + 8:o + 8 + n])
            if v:
                return v
        elif tag == b'POS\0':
            k = struct.unpack_from('<I', b, o + 8)[0]
            f = struct.unpack_from('<%df' % (3 * k), b, o + 12)
            return [f[i:i + 3] for i in range(0, 3 * k, 3)]
        o += 8 + n
    return None

def ppm(path, verts, picked, others, guess):
    W, H = 640, 260
    img = bytearray(b'\x10' * (W * H * 3))
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    span = max(max(zs) - min(zs), max(xs) - min(xs), max(ys) - min(ys)) * 1.1
    sc = 240.0 / span
    def put(px, py, c, r=0):
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                x, y = int(px) + dx, int(py) + dy
                if 0 <= x < W and 0 <= y < H:
                    img[3 * (y * W + x):3 * (y * W + x) + 3] = c
    cy = H / 2 + (max(ys) + min(ys)) / 2 * sc
    for v in verts:     # front view on the left (x, y), side view on the right (z, y; the front on the left)
        put(130 + v[0] * sc, cy - v[1] * sc, b'\xa0\xa0\xa0')
        put(450 + v[2] * sc, cy - v[1] * sc, b'\xa0\xa0\xa0')
    for p, c in [(q, b'\x40\x60\xff') for q in others] + [(q, b'\xff\x20\x20') for q in picked] + [(guess, b'\x20\xff\x20')]:
        put(130 + p[0] * sc, cy - p[1] * sc, c, 2)
        put(450 + p[2] * sc, cy - p[1] * sc, c, 2)
    open(path, 'wb').write(b'P6 %d %d 255\n' % (W, H) + bytes(img))

def pct(a, q):
    a = sorted(a)
    return a[min(len(a) - 1, max(0, int(q * (len(a) - 1) + 0.5)))]

def mesh_lamps(verts):
    """from the body alone: two thirds out to each side (of the bulk of it: 5th..95th percentiles, stray bits left
    out), on its front (the vertices in its front 15% there), two fifths up what is there, on the frontmost of
    it at that height"""
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    cx = (pct(xs, 0.05) + pct(xs, 0.95)) / 2
    hw = pct([abs(x - cx) for x in xs], 0.95)
    zlo, zhi = pct(zs, 0.02), pct(zs, 0.98)
    lx = 0.65 * hw
    front = []
    for wx in (0.35, 0.7, 10.0):
        front = [v for v in verts if v[2] < zlo + 0.15 * (zhi - zlo) and abs(abs(v[0] - cx) - lx) < wx * hw]
        if len(front) >= 4:
            break
    if not front:
        return []
    fy = [v[1] for v in front]
    ly = pct(fy, 0.4)
    band = 0.12 * (pct(ys, 0.95) - pct(ys, 0.05))
    at = [v[2] for v in front if abs(v[1] - ly) < band] or [v[2] for v in front]
    z = min(at) - 2
    return [(cx + lx, ly, z), (cx - lx, ly, z)]

src_count = {}
rows = []
for path in sys.argv[2:]:
    car = os.path.basename(path)[:-4].lower()
    d, m = members(path)
    vht = next((get(d, m, n) for n in m if n.endswith('.vht')), None)
    if vht is None or vht[:3] == b'XFF':
        rows.append((car, 'unread', '')); continue
    t = vht.decode('latin-1')
    mo = re.search(r'^\s*object\s+(\S+)', t, re.M | re.I)
    gem = get(d, m, mo.group(1)) if mo else None
    fff = None
    if gem:
        fm = re.search(rb'([^\s\\/]+\.fff)', gem, re.I)
        fff = get(d, m, fm.group(1).decode('latin-1')) if fm else None
    verts = fff_verts(fff) if fff else None
    if not verts:
        rows.append((car, 'nobody', '')); continue
    lo = [min(v[k] for v in verts) for k in range(3)]
    hi = [max(v[k] for v in verts) for k in range(3)]
    lamps = []
    sec = re.search(r'\[lights\](.*?)(\n\s*\[|\Z)', t, re.S | re.I)
    for line in (sec.group(1) if sec else '').splitlines():
        w = line.split(';')[0].split()
        if len(w) >= 6 and w[0].lower() in ('front', 'rear'):
            try:
                lamps.append((float(w[3]), float(w[4]), float(w[5])))
            except ValueError:
                pass
    cx = (lo[0] + hi[0]) / 2
    cand = [p for p in lamps if p[2] <= lo[2] + 0.25 * (hi[2] - lo[2])]
    if cand:                    # (the lowest of them: a roof bar over headlights is left out)
        y0 = min(p[1] for p in cand)
        cand = [p for p in cand if p[1] <= y0 + 0.15 * (hi[1] - lo[1])]
    guess = (cx, lo[1] + 0.4 * (hi[1] - lo[1]), lo[2] - 8)
    def near(p):
        return min((v[0] - p[0]) ** 2 + (v[1] - p[1]) ** 2 + (v[2] - p[2]) ** 2 for v in verts) ** 0.5
    flags, pair = [], []
    if not cand:
        flags.append('none')
    else:
        wide = max(abs(p[0] - cx) for p in cand)
        pair = [p for p in cand if abs(p[0] - cx) >= 0.85 * wide]
        if max(near(p) for p in pair) > 12: flags.append('off')
        if lo[2] - min(p[2] for p in pair) > 3: flags.append('ahead')
    src = 'vht' if not flags else 'mesh'
    final = pair if not flags else mesh_lamps(verts)
    src_count[src] = src_count.get(src, 0) + 1
    ly = sum(p[1] for p in final) / len(final); lz = min(p[2] for p in final)
    info = '%s: x %s y %.0f z %.0f (body y %.0f..%.0f, front %.0f); old guess z %.0f y %.0f' % (
        src, '/'.join('%.0f' % p[0] for p in final), ly, lz, lo[1], hi[1], lo[2], guess[2], guess[1])
    rows.append((car, ','.join(flags) or 'ok', info))
    ppm(os.path.join(out, car + '.ppm'), verts, final, [p for p in lamps if p not in final], guess)
for r in sorted(rows, key=lambda r: (r[1] == 'ok', r[0])):
    print('%-16s %-10s %s' % r)
from collections import Counter
print(Counter(r[1] for r in rows), src_count)
