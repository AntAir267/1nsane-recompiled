#!/bin/bash
# publish.sh [PUBLIC_CHECKOUT]: bring the public repository (1nsane-recompiled) up to date with this
# one's master: its files but the private notes (PRIVATE below), one commit naming the private
# commits since the last publish, under GitHub's noreply address. Refuses if a file it would publish
# names a private path or address.
set -euo pipefail
cd "$(dirname "$0")/../.."
pub=${1:-work/public}
PRIVATE=(DESIGN.md WINDOWS.md)
url=git@github.com:AntAir267/1nsane-recompiled.git
name=AntAir267
mail=185841325+AntAir267@users.noreply.github.com
[ -d "$pub/.git" ] || git clone -q "$url" "$pub" 2>/dev/null || { mkdir -p "$pub"; git -C "$pub" init -q -b main; git -C "$pub" remote add origin "$url"; }
git -C "$pub" fetch -q origin 2>/dev/null && git -C "$pub" reset -q --hard origin/main 2>/dev/null || true
last=$({ git -C "$pub" log -1 --format=%B 2>/dev/null || true; } | sed -n 's/^Private master: \([0-9a-f]*\)$/\1/p')
head=$(git rev-parse --short master)
[ "$last" = "$head" ] && { echo "public is already at $head"; exit 0; }
tmp=$(mktemp -d)
git archive master | tar -x -C "$tmp"
for f in "${PRIVATE[@]}"; do rm -rf "$tmp/$f"; done
if grep -rIl -F -e "/home/$USER" -e "$(git config user.email)" "$tmp" --exclude='*.spv.h'; then
    echo "refused: the files above name a private path or address"; rm -rf "$tmp"; exit 1
fi
find "$pub" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -a "$tmp"/. "$pub"/
rm -rf "$tmp"
if [ -n "$last" ]; then log=$(git log --format='- %s' "$last..master" --no-merges); else log="- First public version"; fi
git -C "$pub" add -A
git -C "$pub" -c user.name="$name" -c user.email="$mail" commit -q -m "$(printf 'Update from the private repository\n\n%s\n\nPrivate master: %s\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n' "$log" "$head")"
git -C "$pub" push -q origin HEAD:main
echo "published $head"
