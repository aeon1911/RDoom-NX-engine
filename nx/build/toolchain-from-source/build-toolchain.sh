#!/bin/bash
# devkitA64 (GCC 16.1.0 / binutils 2.46 / newlib devkitPro) built from GitHub
# sources, plus the devkitPro host tools and CMake/meson/pkg-config helpers.
# Run fetch-sources.sh first. Installs to /opt/devkitpro (must be writable).
# Only needed where devkitPro's own packages can't be installed.
set -e
export prefix=/opt/devkitpro/devkitA64
export target=aarch64-none-elf
export PATH=$prefix/bin:$PATH
SRC=${SRC:-$PWD/src}
B=${B:-$PWD/build}
J=${J:-3}
mkdir -p $B/binutils $B/gcc $B/newlib
stamp() { echo "=== $(date +%T) $*"; }

stamp binutils
cd $B/binutils
[ -f .done ] || { $SRC/binutils-2.46.0/configure --prefix=$prefix --target=$target --disable-nls --disable-werror \
  --disable-shared --disable-debug --enable-lto --enable-plugins --enable-poison-system-directories \
  --disable-gdb --disable-gdbserver --disable-sim --disable-readline --disable-libdecnumber --disable-gprofng \
  && make -j$J MAKEINFO=true && make install MAKEINFO=true && touch .done; }

stamp gcc-stage1
cd $B/gcc
[ -f .stage1 ] || { CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
  CXXFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" LDFLAGS_FOR_TARGET="" \
  $SRC/gcc-16.1.0/configure --target=$target --prefix=$prefix --enable-languages=c,c++,lto \
  --with-gnu-as --with-gnu-ld --with-gcc --enable-cxx-flags='-ffunction-sections' --disable-libstdcxx-verbose \
  --enable-poison-system-directories --enable-threads=posix --disable-win32-registry --disable-nls --disable-debug \
  --disable-libmudflap --disable-libssp --disable-libgomp --disable-libstdcxx-pch --enable-libstdcxx-time=yes \
  --enable-libstdcxx-filesystem-ts --with-newlib=yes --with-native-system-header-dir=/include \
  --with-sysroot=$prefix/$target --enable-lto --disable-tm-clone-registry --disable-__cxa_atexit \
  --with-bugurl="https://devkitpro.org" --with-march=armv8 --enable-multilib --with-pkgversion="devkitA64" \
  --disable-bootstrap \
  && make -j$J all-gcc MAKEINFO=true && make install-gcc MAKEINFO=true && touch .stage1; }

stamp newlib
cd $B/newlib
[ -f .done ] || { unset CFLAGS CC CXX; CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
  $SRC/newlib-dkp/configure --disable-newlib-supplied-syscalls --enable-newlib-mb --disable-newlib-wide-orient \
  --enable-newlib-register-fini --target=$target --prefix=$prefix \
  && make -j$J MAKEINFO=true && make install -j1 MAKEINFO=true && touch .done; }

stamp gcc-stage2
cd $B/gcc
[ -f .stage2 ] || { make -j$J MAKEINFO=true && make install-strip MAKEINFO=true && touch .stage2; }

# stage1 generated limits.h without the newlib syslimits chain (no PATH_MAX): regenerate it
GI=$prefix/lib/gcc/$target/16.1.0/include
cat $SRC/gcc-16.1.0/gcc/limitx.h $SRC/gcc-16.1.0/gcc/glimits.h $SRC/gcc-16.1.0/gcc/limity.h > $GI/limits.h

stamp rules
cd $SRC/devkita64-rules-1.1.1 && make install DEVKITPRO=/opt/devkitpro || cp -v *rules* /opt/devkitpro/devkitA64/ 
stamp host-tools
for t in switch-tools general-tools; do
  (cd $SRC/$t && ./autogen.sh && ./configure --prefix=/opt/devkitpro/tools && make -j$J && make install)
done

stamp cmake-meson-pkgconfig
D=/opt/devkitpro; PP=$SRC/pacman-packages
mkdir -p $D/cmake/Platform $D/portlibs/switch/bin $D/portlibs/switch/lib
cp $PP/cmake/common-utils/dkp-*.cmake $D/cmake/
cp $PP/cmake/common-utils/Generic-dkP.cmake $D/cmake/Platform/
cp $PP/cmake/devkita64/aarch64-none-elf-cmake $D/devkitA64/bin/
cp $PP/cmake/devkita64/devkitA64.cmake $PP/cmake/switch/Switch.cmake $D/cmake/
cp $PP/cmake/switch/NintendoSwitch.cmake $D/cmake/Platform/
cp $PP/cmake/switch/aarch64-none-elf-cmake $D/portlibs/switch/bin/
install -m755 $PP/dkp-meson-scripts/meson-cross.sh $PP/dkp-meson-scripts/meson-toolchain.sh $D/
install -m644 $PP/dkp-toolchain-vars/devkita64.sh $PP/dkp-toolchain-vars/switchvars.sh $D/
install -m755 $PP/dkp-toolchain-vars/portlibs_prefix.sh $D/
install -m755 $PP/switch/pkg-config/pkg-config.in $D/portlibs/switch/bin/aarch64-none-elf-pkg-config
chmod +x $D/devkitA64/bin/aarch64-none-elf-cmake $D/portlibs/switch/bin/aarch64-none-elf-cmake
stamp DONE
