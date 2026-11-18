"""gencmp.py OLD_GEN NEW_GEN OUTDIR: split code_*.c into functions, compare by name.
Writes OUTDIR/summary.txt and OUTDIR/diff_<name>.txt for differing functions; prints counts."""
import sys, os, re, glob, difflib, collections


def unwrap_x87(s):
    out, i = [], 0
    while True:
        j = s.find('X87(', i)
        if j < 0:
            out.append(s[i:]); return ''.join(out)
        out.append(s[i:j]); k = j + 4; depth = 1
        while depth:
            if s[k] == '(': depth += 1
            elif s[k] == ')': depth -= 1
            k += 1
        out.append(s[j + 4:k - 1]); i = k

def norm(line):
    line = re.sub(r'\b(W8|W16|W32|WF32|WF64)T\((?:0x[0-9a-fA-F]+|\d+)u?, ', r'\1(', line)
    line = line.replace('fpu_sw_cc(', 'fpu_sw(')
    line = re.sub(r'API_CALL\((w_\w+), (?:0x[0-9a-f]+|\d+)u?\)', r'\1(c)', line)
    if re.match(r'\s*if \((capture_armed|dev_recording)\) capture_(point|return)\(c, \w+\);\s*$', line):
        return ''
    if 'X87(' in line:
        line = unwrap_x87(line)
    return line

def load(d):
    funcs = {}
    for p in sorted(glob.glob(os.path.join(d, 'code_*.c'))):
        cur, buf = None, []
        for line in open(p):
            m = re.match(r'^(?:static )?\S+ (F_[0-9a-f]{8}\w*)\(', line)
            if m and not line.rstrip().endswith(';'):
                if cur:
                    funcs[cur] = buf
                cur, buf = m.group(1), [norm(line)]
            elif cur:
                l2 = norm(line) if NORM else line
                if l2: buf.append(l2)
        if cur:
            funcs[cur] = buf
    return funcs

old, new, out = sys.argv[1:4]
NORM = len(sys.argv) > 4
os.makedirs(out, exist_ok=True)
A, B = load(old), load(new)
only_a = sorted(set(A) - set(B)); only_b = sorted(set(B) - set(A))
same = diff = 0
kinds = collections.Counter()
lines = []
for n in sorted(set(A) & set(B)):
    if A[n] == B[n]:
        same += 1
        continue
    diff += 1
    d = list(difflib.unified_diff(A[n], B[n], n=0))
    ch = [l for l in d if (l.startswith('+') or l.startswith('-')) and not l.startswith('+++') and not l.startswith('---')]
    # classify each changed line by a normalised form
    for l in ch:
        s = re.sub(r'0x[0-9a-f]+|\b\d+\b', 'N', l[1:].strip())
        s = re.sub(r'F_[0-9a-f]{8}', 'F_X', s)
        kinds[l[0] + ' ' + s[:110]] += 1
    open(os.path.join(out, 'diff_%s.txt' % n), 'w').writelines(d)
    lines.append('%s %d\n' % (n, len(ch)))
with open(os.path.join(out, 'summary.txt'), 'w') as f:
    f.write('old %d new %d same %d differ %d only_old %d only_new %d\n' % (len(A), len(B), same, diff, len(only_a), len(only_b)))
    f.write('only old: %s\nonly new: %s\n' % (' '.join(only_a), ' '.join(only_b)))
    f.writelines(lines)
    f.write('\n--- changed-line kinds (most common) ---\n')
    for k, v in kinds.most_common(80):
        f.write('%6d %s\n' % (v, k))
print('old %d new %d same %d differ %d only_old %d only_new %d' % (len(A), len(B), same, diff, len(only_a), len(only_b)))
