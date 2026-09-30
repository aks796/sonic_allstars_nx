#!/bin/sh
# Build sonicracing_nx.nsp inside the AArch32 Switch toolchain container
# (devkitARM + libnx32 from the vita2hos image). Arguments are passed to make:
#   ./build.sh            # sonicracing_nx.nsp
#   ./build.sh clean
#
# libnx itself comes from libnx32 (github.com/aks796/libnx32: the vita2hos
# AArch32 libnx with its fixes: IPC data that depended on the size of an enum,
# among it the supported-controller list that kept wireless controllers out).
# Its headers and archives are mounted over the image's; the image's other
# libraries in the same folder (miniz...) stay. The default is the prefix/ of
# a libnx32 checkout next to this one (../libnx32/prefix), then
# ../../thirtytwo/libnx32/prefix (the maintainer's layout); DCR_LIBNX32 names
# another install.
set -e
IMAGE="${DCR_TOOLCHAIN_IMAGE:-ghcr.io/vita2hos/devcontainer/vita2hos:latest}"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "${DCR_LIBNX32:-}" ]; then
  LIBNX32="$DCR_LIBNX32"
else
  LIBNX32="$HERE/../libnx32/prefix"
  for c in "$HERE/../libnx32/prefix" "$HERE/../../thirtytwo/libnx32/prefix"; do
    if [ -f "$c/lib/libnx.a" ] && [ -f "$c/include/switch.h" ]; then LIBNX32="$c"; break; fi
  done
fi
if [ ! -f "$LIBNX32/lib/libnx.a" ] || [ ! -f "$LIBNX32/include/switch.h" ]; then
  echo "build.sh: the patched libnx32 is not at $LIBNX32" >&2
  echo "  clone github.com/aks796/libnx32 next to this folder and run its ./build.sh," >&2
  echo "  or set DCR_LIBNX32 to its prefix/" >&2
  exit 1
fi
LIBNX32="$(cd "$LIBNX32" && pwd)"
NXD=/opt/devkitpro/libnx32
exec docker run --rm --platform linux/amd64 \
  -v "$HERE:/work" \
  -v "$LIBNX32/include/switch:$NXD/include/switch:ro" \
  -v "$LIBNX32/include/switch.h:$NXD/include/switch.h:ro" \
  -v "$LIBNX32/lib/libnx.a:$NXD/lib/libnx.a:ro" \
  -v "$LIBNX32/lib/libnxd.a:$NXD/lib/libnxd.a:ro" \
  -w /work "$IMAGE" \
  bash -lc "make -j\$(nproc) $*"
