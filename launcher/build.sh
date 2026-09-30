#!/bin/sh
# Build sonic_allstars_nx.nro (the launcher) in devkitPro's 64-bit toolchain
# container. Build the wrapper first (../build.sh): the NRO carries
# ../sonicracing_nx.nsp and ../sonicracing_nx.build.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/../sonicracing_nx.nsp" ] && [ -f "$HERE/../sonicracing_nx.build" ] || { echo "build the wrapper first (../build.sh)"; exit 1; }
exec docker run --rm --platform linux/amd64 \
  -v "$HERE/..:/work" -w /work/launcher devkitpro/devkita64:latest \
  bash -lc "make -j\$(nproc) $*"
