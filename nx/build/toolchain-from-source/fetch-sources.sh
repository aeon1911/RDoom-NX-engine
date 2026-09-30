#!/bin/bash
# Fetches everything needed to build devkitA64 + libnx + the switch portlibs
# from GitHub (for machines where the devkitPro package servers can't be
# reached). Normal setups should just install devkitPro with its pacman.
#   fetch-sources.sh [src dir]
set -e
SRC=${1:-$PWD/src}
mkdir -p "$SRC" && cd "$SRC"
c() { [ -d "$3" ] || git clone -q --depth 1 --branch "$2" "https://github.com/$1.git" "$3"; }
c devkitPro/buildscripts master buildscripts
c devkitPro/pacman-packages master pacman-packages
c gnutools/binutils-gdb binutils-2_46 binutils-2.46.0
c gcc-mirror/gcc releases/gcc-16.1.0 gcc-16.1.0
c devkitPro/newlib devkitPro newlib-dkp
c devkitPro/devkita64-rules v1.1.1 devkita64-rules-1.1.1
c switchbrew/libnx v4.12.0 libnx
c switchbrew/switch-tools v1.13.1 switch-tools
c devkitPro/general-tools v1.4.4 general-tools
c devkitPro/libdrm_nouveau v1.0.1 libdrm_nouveau
c chaotic-cx/mesa-mirror mesa-20.1.0-rc3 mesa-20.1.0-rc3
c libsdl-org/SDL release-2.28.5 SDL2-2.28.5
c kcat/openal-soft 1.21.1 openal-soft-1.21.1
c webmproject/libvpx v1.8.0 libvpx-1.8.0
c xiph/ogg v1.3.4 libogg-1.3.4
c xiph/vorbis v1.3.7 libvorbis-1.3.7
c xiph/opus v1.3 opus-1.3
c xiph/flac 1.3.2 flac-1.3.2
c libsndfile/libsndfile 1.2.2 libsndfile-1.2.2
c libsdl-org/mpg123 v1.31.3-SDL mpg123-1.31.3
c madler/zlib v1.3.1 zlib-1.3.1

P=buildscripts/patches
[ -f binutils-2.46.0/.patched ] || { (cd binutils-2.46.0 && patch -p1 -s < ../$P/binutils-2.46.0-1.patch && touch .patched); }
[ -f gcc-16.1.0/.patched ] || { (cd gcc-16.1.0 && patch -p1 -s < ../$P/gcc-16.1.0-1.patch && touch .patched); }
# (the devkitPro branch of newlib already contains newlib-4.6.0.20260123-4.patch)

# fixes needed with GCC 16 / current newlib:
# - newlib now declares timespec_get(): mesa's C11 fallback must use another name
grep -q mesa_timespec_get mesa-20.1.0-rc3/include/c11/threads.h || \
	sed -i 's|^#include <sys/time.h>$|#include <sys/time.h>\n#define timespec_get mesa_timespec_get|' mesa-20.1.0-rc3/include/c11/threads.h
# - openal-soft 1.21.1 has a typo in a template that GCC 16 no longer ignores
sed -i 's/const T& operator\*() const& { return this->mValue; }/const T\& operator*() const\& { return mStore.mValue; }/' openal-soft-1.21.1/common/aloptional.h
echo "sources ready in $SRC"
