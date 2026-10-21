#!/bin/bash
# (development script; run from the repository root)
# usage: shot.sh OUTDIR FRAMES EVERY KEYS [extra args]
out=$1; frames=$2; every=$3; keys=$4; shift 4
rm -rf "$out" "$out.user"; mkdir -p "$out" "$out.user"
cp ~/.local/share/1nsane-native/options.txt "$out.user/" 2>/dev/null
INSANE_GPU_OFFSCREEN=1 INSANE_GPU_OFFSCREEN_SIZE=${SIZE:-1280x960} INSANE_DUMP_FRAMES=$out INSANE_DUMP_EVERY=$every INSANE_KEYS="$keys" \
  timeout 600 build/1nsane --game ~/.local/share/1nsane-native/game --user "$out.user" --test --frames $frames --logfile "$out.log" "$@" >/dev/null 2>&1
echo "exit $?"; ls "$out" | wc -l
