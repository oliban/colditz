#!/bin/bash
# film.sh <seconds> <fps> <out.png> — burst-capture /screen frames and montage
# into one contact sheet so the brain can "watch" a short clip as one image.
set -u
SECS=${1:-6}; FPS=${2:-2}; OUT=${3:-/tmp/film.png}
D=$(mktemp -d)
N=$((SECS * FPS))
for i in $(seq -w 1 $N); do
  curl -s -o "$D/f$i.png" http://127.0.0.1:8765/screen
  sleep $(python3 -c "print(1/$FPS)")
done
magick montage "$D"/f*.png -tile 4x -geometry +2+2 -background black "$OUT"
rm -rf "$D"
echo "$OUT ($N frames @ ${FPS}fps)"
