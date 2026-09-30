#!/bin/sh
# test_host.sh -- the checks that run on a PC, against your own files:
#   1. every import of libssasr.so is served (tools/gen_imports.py --check)
#   2. the game's data is found in each file given, in place, as the game
#      program finds it (source/ssr_pack.h): the .obb, the zip it came in, or
#      a single APK with the data inside
#
#   tools/test_host.sh <your apk> [the obb | its .zip ...]
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
APK="$1"
[ -n "$APK" ] || { echo "usage: $0 <apk> [expansion file or zip ...]"; exit 2; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== imports"
unzip -o -q "$APK" 'lib/armeabi/libssasr.so' -d "$TMP"
python3 "$HERE/tools/gen_imports.py" --libs "$TMP/lib/armeabi" --check 2>&1 | tail -2

echo "== the game's data"
cc -O1 -Wall -Wextra -o "$TMP/test_pack" "$HERE/tools/host/test_pack.c"
"$TMP/test_pack" "$@"
echo "host checks passed"
