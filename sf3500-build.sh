#!/bin/bash
# Builds Chocolate Doom and the WAD loader for the Datafrog SF3500.
#
#   SF3500_TC=/path/to/toolchain/host MIXER_SRC=/path/to/SDL2_mixer-2.6.3 \
#       ./sf3500-build.sh [output-dir]
#
# The toolchain's sysroot needs SDL2 (2.0.14), libpng and zlib. SDL2_mixer is
# built here without modplug (the sysroot's own copy may include it, which adds
# libstdc++ and libgcc to the binary) into <output-dir>/mixer-prefix.
set -e
: "${SF3500_TC:?set SF3500_TC to the toolchain host directory}"
: "${MIXER_SRC:?set MIXER_SRC to an SDL2_mixer 2.6.x source tree}"
SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "${1:-$SRC/build-sf3500}" && cd "${1:-$SRC/build-sf3500}" && pwd)"
TOOLCHAIN="$SRC/cmake/sf3500-toolchain.cmake"
MIXER="$OUT/mixer-prefix"

mkdir -p "$OUT/mixer-build" && cd "$OUT/mixer-build"
cmake "$MIXER_SRC" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_INSTALL_PREFIX="$MIXER" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DSDL2MIXER_VENDORED=ON \
  -DSDL2MIXER_DEPS_SHARED=OFF -DSDL2MIXER_VORBIS=STB -DSDL2MIXER_FLAC=OFF \
  -DSDL2MIXER_MOD=OFF -DSDL2MIXER_MP3=OFF -DSDL2MIXER_MIDI=OFF -DSDL2MIXER_OPUS=OFF \
  -DSDL2MIXER_CMD=OFF -DSDL2MIXER_WAVE=ON -DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_INSTALL=ON
make -j"$(nproc)" && make install

mkdir -p "$OUT/doom-build" && cd "$OUT/doom-build"
cmake "$SRC" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_PREFIX_PATH="$MIXER;$SF3500_TC/mipsel-buildroot-linux-gnu/sysroot/usr" \
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=OFF \
  -DSDL2_MIXER_LIBRARY="$MIXER/lib/libSDL2_mixer.a" \
  -DSDL2_MIXER_INCLUDE_DIR="$MIXER/include/SDL2" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -fno-strict-aliasing -DNDEBUG" \
  -DENABLE_SDL2_NET=OFF -DENABLE_SDL2_MIXER=ON
make -j"$(nproc)" chocolate-doom
"$SF3500_TC/bin/mips-mti-linux-gnu-strip" -o "$OUT/chocolate-doom.mipsel" src/chocolate-doom

make -C "$SRC/wadloader" TC="$SF3500_TC" clean wadloader
"$SF3500_TC/bin/mips-mti-linux-gnu-strip" -o "$OUT/wadloader.mipsel" "$SRC/wadloader/wadloader"
echo "Built: $OUT/chocolate-doom.mipsel and $OUT/wadloader.mipsel"
