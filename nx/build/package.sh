#!/bin/bash
# Assembles the SD card layout from a finished Switch build directory.
#   package.sh <build dir> <output dir>
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$1; P=$2
rm -rf "$P"
mkdir -p "$P/switch/gzdoom/temp" "$P/so-para-instalacao-do-zero/switch/gzdoom"
cp "$B/gzdoom_launcher.nro" "$P/switch/"
cp "$B/gzdoom.nro" "$B"/*.pk3 "$P/switch/gzdoom/"
cp -r "$B/soundfonts" "$B/fm_banks" "$P/switch/gzdoom/"
cp "$ROOT/nx/uzdoom.ini" "$ROOT/nx/shadervariants.txt" "$P/so-para-instalacao-do-zero/switch/gzdoom/"
cp "$ROOT/nx/LEIA-ME.txt" "$P/"
echo "package: $P"
