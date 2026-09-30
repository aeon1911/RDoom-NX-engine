#!/bin/bash
# libsndfile is not packaged by devkitPro: build it into the switch portlibs.
# Needs switch-libogg, switch-libvorbis, switch-libopus and switch-flac.
#   build-libsndfile.sh <libsndfile 1.2.2 source dir>
set -e
SRC=$1
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
source "$DEVKITPRO/switchvars.sh"
# extra flags go through the environment: -DCMAKE_C_FLAGS=... would replace
# the toolchain's (-mtp=soft etc.) and produce a crashing binary
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
cmake -S "$SRC" -B "$SRC/build-nx" -G "Unix Makefiles" \
	-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
	-DCMAKE_INSTALL_PREFIX="$PORTLIBS_PREFIX" -DBUILD_SHARED_LIBS=OFF \
	-DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CPACK=OFF \
	-DENABLE_EXTERNAL_LIBS=ON -DENABLE_MPEG=OFF -DINSTALL_MANPAGES=OFF -DBUILD_REGTEST=OFF
make -C "$SRC/build-nx" -j"$(nproc)"
make -C "$SRC/build-nx" install
# ZMusic's CMake on the Switch expects an (empty) libdl
[ -f "$PORTLIBS_PREFIX/lib/libdl.a" ] || aarch64-none-elf-ar rcs "$PORTLIBS_PREFIX/lib/libdl.a"
