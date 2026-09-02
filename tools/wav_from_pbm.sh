#!/bin/sh
# usage: tools/wav_from_pbm.sh images/sphere.pbm out.wav
# Builds the host tools if needed, then renders the full transmission as a 44.1 kHz WAV.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
make -s -C "$root/tests" bin/sstv_wav
"$root/tests/bin/sstv_wav" "$1" "$2"
