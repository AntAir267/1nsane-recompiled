#!/bin/bash
# (development script; run from the repository root)
# bench.sh NAME [profile]: the scripted quick race at 1920x1080 (widescreen, native), deterministic;
# prints the race segment's mean game-thread CPU ms per frame and wall ms per frame.
n=$1; d=work/bench_$n; rm -rf $d.user; mkdir -p $d.user
printf "resolution = native\nwidescreen = on\n" > $d.user/options.txt
P=; [ "$2" = profile ] && P="INSANE_PROFILE=$d.prof"
env $P INSANE_BENCH=1 INSANE_VK_STATS=1 INSANE_ENHANCED=1 INSANE_GPU_OFFSCREEN=1 INSANE_GPU_OFFSCREEN_SIZE=1920x1080 \
  INSANE_KEYS="900:RETURN*10,1000:DOWN*10,1100:RETURN*10,1600:RETURN*10,2200:UP*900" \
  timeout 900 build/1nsane --game ~/.local/share/1nsane-native/game --user $d.user --test --frames 3200 --logfile $d.log >/dev/null 2>&1
grep "vk: per frame" $d.log | awk 'NR>=19 && NR<=31' | sed 's/.*passes; \([0-9.]*\) ms in all.*game thread CPU \([0-9.]*\) ms.*/\1 \2/' | \
  awk '{w+=$1; c+=$2; n++} END {printf "race frames: wall %.2f ms, game thread CPU %.2f ms (%d windows)\n", w/n, c/n, n}'
grep "frame limit" $d.log | head -1
