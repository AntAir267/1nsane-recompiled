#!/bin/bash
# (development script; run from the repository root)
# ab.sh BIN_A BIN_B [BIN_C...]: run the scripted race with each binary at the same time (same machine
# load), print each one's mean race-frame game-thread CPU ms and wall ms.
i=0
for b in "$@"; do
  d=work/ab_$i; rm -rf $d.user; mkdir -p $d.user
  printf "resolution = native\nwidescreen = on\n" > $d.user/options.txt
  INSANE_BENCH=1 INSANE_VK_STATS=1 INSANE_ENHANCED=1 INSANE_GPU_OFFSCREEN=1 INSANE_GPU_OFFSCREEN_SIZE=1920x1080 \
    INSANE_KEYS="900:RETURN*10,1000:DOWN*10,1100:RETURN*10,1600:RETURN*10,2200:UP*900" \
    timeout 1200 $b --game ~/.local/share/1nsane-native/game --user $d.user --test --frames 3200 --logfile $d.log >/dev/null 2>&1 &
  i=$((i+1))
done
wait
i=0
for b in "$@"; do
  printf "%-28s " $(basename $b)
  grep "vk: per frame" work/ab_$i.log | awk 'NR>=19 && NR<=31' | sed 's/.*passes; \([0-9.]*\) ms in all.*game thread CPU \([0-9.]*\) ms.*/\1 \2/' | \
    awk '{w+=$1; c+=$2; n++} END {printf "race frames: game thread CPU %.2f ms, wall %.2f ms (%d windows)\n", c/n, w/n, n}'
  i=$((i+1))
done
