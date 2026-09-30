#!/bin/sh
# package_sd.sh -- assemble SD_CARD/ (and SD_CARD.zip) for a test on a Switch.
#
#   tools/package_sd.sh [path/to/your/apk [path/to/your/obb-or-its-zip]]
#
# Builds the wrapper and the launcher, then lays out what goes on the card:
#   SD_CARD/switch/sonic_allstars_nx/sonic_allstars_nx.nro
#   SD_CARD/switch/sonic_allstars_nx/<your apk>   (only if given: your own copy)
#   SD_CARD/switch/sonic_allstars_nx/<your data>  (only if given: your own copy)
#   SD_CARD/README_FIRST.txt
# File names do not matter: the game finds its APK and data by their contents.
# With your files in it, SD_CARD.zip holds your copy of the game: keep it to
# yourself. The ExeFS override is not included: the launcher writes it for
# whichever sphaira forwarder it is started from.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
DIR=SD_CARD/switch/sonic_allstars_nx
./build.sh
launcher/build.sh
if [ -n "$1" ]; then
  tools/test_host.sh "$@" | tail -1
fi
rm -rf SD_CARD SD_CARD.zip
mkdir -p "$DIR"
cp launcher/sonic_allstars_nx.nro "$DIR/"
cp tools/README_FIRST.txt SD_CARD/
if [ -n "$1" ]; then
  cp -p "$1" "$DIR/$(basename "$1")"
fi
if [ -n "$2" ]; then
  cp -p "$2" "$DIR/$(basename "$2")"
fi
(cd SD_CARD && zip -qr0 ../SD_CARD.zip . -x '*.DS_Store')
echo "build $(cat sonicracing_nx.build): SD_CARD/ and SD_CARD.zip ready"
