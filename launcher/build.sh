#!/bin/sh
# Build sonic_allstars_nx.nro (the launcher) with the runtime's launcher build
# (devkitPro's 64-bit toolchain container). Build the wrapper first
# (../build.sh): the NRO carries ../sonicracing_nx.nsp and ../sonicracing_nx.build.
HERE="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$HERE" PAYLOAD=sonicracing_nx exec "$HERE/../runtime/launcher/build.sh" "$@"
