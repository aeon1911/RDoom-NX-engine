#!/bin/bash
# Builds the Switch port: gzdoom.nro (engine), gzdoom_launcher.nro and the
# engine .pk3 files, then assembles the SD card package.
#
# Needs a devkitPro install (devkitA64 + libnx + the switch portlibs listed in
# nx/build/README.md) and, on the host, cmake, ninja and a C/C++ compiler.
#
#   nx/build/build-switch.sh            # build + package into build/package
#   JOBS=4 nx/build/build-switch.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build}
JOBS=${JOBS:-$(nproc)}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PORTLIBS_PREFIX=$DEVKITPRO/portlibs/switch
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PORTLIBS_PREFIX/bin:$PATH

# 1. host tools (zipdir, re2c, lemon) used while building
if [ ! -f "$OUT/native/ImportExecutables.cmake" ]; then
	cmake -S "$ROOT" -B "$OUT/native" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
ninja -C "$OUT/native" -j"$JOBS" zipdir re2c lemon

# 2. the Switch build
if [ ! -f "$OUT/nx/build.ninja" ]; then
	cmake -S "$ROOT" -B "$OUT/nx" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
		-DCMAKE_BUILD_TYPE=Release \
		-DNINTENDO_SWITCH=ON \
		-DDYN_OPENAL=OFF -DDYN_SNDFILE=OFF -DDYN_MPG123=OFF \
		-DIMPORT_EXECUTABLES="$OUT/native/ImportExecutables.cmake" \
		-DSNDFILE_INCLUDE_DIR="$PORTLIBS_PREFIX/include" \
		-DSNDFILE_LIBRARY="$PORTLIBS_PREFIX/lib/libsndfile.a"
fi
ninja -C "$OUT/nx" -j"$JOBS"

# 3. sanity check: libnx never sets up the hardware thread pointer, so no code
#    may use TLS through tpidr_el0 (everything must be built with -mtp=soft)
for elf in "$OUT/nx/uzdoom.elf" "$OUT/nx/tools/nxlauncher/gzdoom_launcher.elf"; do
	[ -f "$elf" ] || continue
	n=$(aarch64-none-elf-objdump -d "$elf" | grep -c tpidr_el0 || true)
	if [ "$n" != "0" ]; then echo "ERROR: $elf uses tpidr_el0 ($n times)"; exit 1; fi
done

# 4. SD card package
"$ROOT/nx/build/package.sh" "$OUT/nx" "$OUT/package"
