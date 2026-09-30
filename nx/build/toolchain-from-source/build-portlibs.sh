#!/bin/bash
# libnx + the Switch portlibs used by this port, built from GitHub sources with
# the devkitPro pacman-packages recipes/patches (run fetch-sources.sh first).
# Also needs the host tools/cmake files installed by build-toolchain.sh.
#   build-portlibs.sh [libnx drm mesa sdl2 openal ogg vorbis opus flac mpg123 sndfile vpx zlib]
set -e
set -o pipefail
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
SRC=${SRC:-$PWD/src}
PK=$SRC/pacman-packages/switch
J=${J:-3}
stamp() { echo "=== $(date +%T) $*"; }
only="$*"
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
sv() { source $DEVKITPRO/devkita64.sh; source $DEVKITPRO/switchvars.sh; }

if want libnx; then stamp libnx
  cd $SRC/libnx/nx && make -j$J && make install
fi
if want drm; then stamp libdrm_nouveau
  cd $SRC/libdrm_nouveau && make -j$J && make install
fi
if want mesa; then stamp mesa
  cd $SRC/mesa-20.1.0-rc3
  if [ ! -f .patched ]; then
    patch -p1 -i $PK/mesa/switch-mesa-20.1.0-5.patch
    patch -p1 -i $PK/mesa/gl_XML.py.patch
    patch -p1 -i $PK/mesa/glX_XML.py.patch
    touch .patched
  fi
  rm -rf build
  ( sv; $DEVKITPRO/meson-cross.sh switch ../crossfile.txt build -Db_ndebug=true )
  ninja -C build -j$J || exit 1
  ninja -C build install || exit 1
  sed -i "s,-lEGL ,-lEGL -ldrm_nouveau ," $DEVKITPRO/portlibs/switch/lib/pkgconfig/egl.pc || true
  install -Dm644 $PK/mesa/OpenGLConfig.cmake $DEVKITPRO/portlibs/switch/lib/cmake/OpenGL/OpenGLConfig.cmake
fi
if want sdl2; then stamp SDL2
  cd $SRC/SDL2-2.28.5
  [ -f .patched ] || { patch -p1 -i $PK/SDL2/SDL2-2.28.5.patch; touch .patched; }
  ( sv; ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf --disable-shared --enable-static \
    --enable-audio --enable-joystick --disable-power --disable-filesystem --enable-cpuinfo --enable-pthreads \
    --enable-timers --enable-video && make -j$J && make install ) || exit 1
fi
if want openal; then stamp openal-soft
  cd $SRC/openal-soft-1.21.1
  [ -f .patched ] || { patch -Np1 -i $PK/openal-soft/avoid-readlink.patch; sed -i 's#SDL2/##' alc/backends/sdl2.cpp; touch .patched; }
  rm -rf build-nx && mkdir build-nx && cd build-nx
  ( sv; unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS; aarch64-none-elf-cmake -G"Unix Makefiles" -DUSE_SWITCH=ON -DALSOFT_UTILS=OFF -DLIBTYPE=STATIC \
    -DALSOFT_EXAMPLES=OFF -DALSOFT_REQUIRE_SDL2=ON -DALSOFT_BACKEND_SDL2=ON -DSDL2_INCLUDE_DIR="${PORTLIBS_PREFIX}/include" \
    -DCMAKE_INSTALL_PREFIX="${PORTLIBS_PREFIX}" .. && make -j$J && make install ) || exit 1
fi
if want ogg; then stamp libogg
  cd $SRC/libogg-1.3.4 && [ -f configure ] || ./autogen.sh
  ( sv; ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf --disable-shared --enable-static && make -j$J && make install ) || exit 1
fi
if want vorbis; then stamp libvorbis
  cd $SRC/libvorbis-1.3.7 && [ -f configure ] || ./autogen.sh
  ( sv; ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf --disable-shared --enable-static --disable-examples --disable-docs && make -j$J && make install ) || exit 1
fi
if want opus; then stamp opus
  cd $SRC/opus-1.3 && [ -f configure ] || ./autogen.sh
  ( sv; ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf --disable-shared --enable-static --disable-doc --disable-extra-programs && make -j$J && make install ) || exit 1
fi
if want flac; then stamp flac
  cd $SRC/flac-1.3.2
  # only the compat.h part applies to the git tree (the rest patches generated Makefile.in files)
  [ -f .patched ] || { patch -Np1 -i $PK/flac/flac-1.3.2.patch || true; touch .patched; }
  [ -f configure ] || ./autogen.sh
  ( sv; CFLAGS="${CFLAGS} -D__SWITCH__ " ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf \
    --disable-shared --enable-static --disable-xmms-plugin --enable-cpplibs --disable-sse --disable-doxygen-docs \
    && make -C src/libFLAC -j$J && make -C src/libFLAC install && make -C include install )
  install -Dm644 $SRC/flac-1.3.2/src/libFLAC/flac.pc $DEVKITPRO/portlibs/switch/lib/pkgconfig/flac.pc || true
fi
if want mpg123; then stamp mpg123
  cd $SRC/mpg123-1.31.3
  [ -f .patched ] || { patch -Np1 -i $PK/mpg123/mpg123-1.31.3.patch || true; touch .patched; }
  autoreconf -fi
  ( sv; export CFLAGS="${CFLAGS} -Wno-error=incompatible-pointer-types"; LIBS="-lnx -lm" ./configure --prefix="${PORTLIBS_PREFIX}" --host=aarch64-none-elf --disable-shared --enable-static \
    --enable-fifo=no --enable-ipv6=no --enable-network=no --enable-int-quality=no --with-cpu=generic \
    --with-default-audio=dummy --disable-components --enable-libmpg123 && make -j$J && make install ) || exit 1
fi
if want sndfile; then stamp libsndfile
  cd $SRC/libsndfile-1.2.2 && rm -rf build-nx && mkdir build-nx && cd build-nx
  ( sv; unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS; aarch64-none-elf-cmake -G"Unix Makefiles" -DCMAKE_INSTALL_PREFIX="${PORTLIBS_PREFIX}" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CPACK=OFF \
    -DENABLE_EXTERNAL_LIBS=ON -DENABLE_MPEG=OFF -DINSTALL_MANPAGES=OFF -DBUILD_REGTEST=OFF .. && make -j$J && make install ) || exit 1
fi
if want vpx; then stamp libvpx
  cd $SRC/libvpx-1.8.0
  [ -f .patched ] || { patch -p1 -i $PK/libvpx/libvpx.patch; touch .patched; }
  ( sv; CROSS=aarch64-none-elf- ./configure --prefix="${PORTLIBS_PREFIX}" --target=generic-gnu --disable-examples \
    --disable-tools --disable-docs --disable-unit-tests --disable-shared --enable-static && make -j$J && make install ) || exit 1
fi
if want zlib; then stamp zlib
  cd $SRC/zlib-1.3.1
  ( sv; CHOST=aarch64-none-elf ./configure --prefix="${PORTLIBS_PREFIX}" --static && make -j$J libz.a && make install ) || exit 1
fi
# ZMusic's CMake on the Switch expects an (empty) libdl
[ -f $DEVKITPRO/portlibs/switch/lib/libdl.a ] || aarch64-none-elf-ar rcs $DEVKITPRO/portlibs/switch/lib/libdl.a
stamp DONE
