#!/bin/bash
# (development script; run from the repository root)
# ab2.sh ROUNDS BIN_A BIN_B: run the race with both binaries side by side ROUNDS times, swapping
# which starts first; prints each binary's best and median game-thread CPU ms per race frame.
rounds=$1; a=$2; b=$3
for r in $(seq 1 $rounds); do
  if [ $((r % 2)) = 1 ]; then tools/dev/ab.sh $a $b; else tools/dev/ab.sh $b $a; fi
done | python3 -c "
import sys, collections
v = collections.defaultdict(list)
for l in sys.stdin:
    f = l.split()
    v[f[0]].append(float(f[6]))
for n, x in v.items():
    x.sort()
    print('%-12s best %.2f ms  median %.2f ms  %s' % (n, x[0], x[len(x) // 2], x))
"
