# Compilando o port para Switch

## Com o devkitPro instalado (o normal)

Pacotes do devkitPro (`dkp-pacman -S ...`):

```
switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-openal-soft
switch-libvpx switch-libogg switch-libvorbis switch-libopus switch-flac
switch-mpg123 switch-zlib switch-pkg-config switch-cmake
```

O libsndfile não existe nos pacotes do devkitPro:

```
git clone --depth 1 --branch 1.2.2 https://github.com/libsndfile/libsndfile.git
nx/build/build-libsndfile.sh libsndfile
```

No PC também é preciso `cmake`, `ninja`, `python3` e um compilador C/C++ (para as
ferramentas zipdir/re2c/lemon usadas durante a compilação). Depois:

```
nx/build/build-switch.sh
```

O pacote para o cartão SD sai em `build/package/` (`switch/` + `so-para-instalacao-do-zero/` + `LEIA-ME.txt`).

## Sem acesso aos servidores do devkitPro

`nx/build/toolchain-from-source/` monta tudo a partir do GitHub (é assim que as
builds deste repositório foram feitas): devkitA64 (GCC 16.1.0, binutils 2.46,
newlib do devkitPro), libnx 4.12.0, switch-tools e os portlibs com as receitas e
patches do `devkitPro/pacman-packages`.

```
cd nx/build/toolchain-from-source
./fetch-sources.sh          # clona tudo em ./src e aplica os patches
./build-toolchain.sh        # instala em /opt/devkitpro
./build-portlibs.sh         # libnx, mesa, SDL2, OpenAL, codecs, zlib, libsndfile...
cd ../../.. && nx/build/build-switch.sh
```

## Checagem obrigatória

A libnx nunca configura o registrador de thread pointer (`tpidr_el0`): todo código
tem que ser compilado com `-mtp=soft`. O `build-switch.sh` falha se o executável
usar `tpidr_el0`. Nunca passe `-DCMAKE_C_FLAGS=`/`-DCMAKE_CXX_FLAGS=` para projetos
do Switch: isso apaga as flags do toolchain; flags extras vão por `CFLAGS`/`CXXFLAGS`
de ambiente.
