# Chocolate Doom for the Datafrog SF3500

This branch (`sf3500`) is upstream Chocolate Doom 3.1.1 (tag `chocolate-doom-3.1.1`,
commit 410d968) plus a port to the Datafrog SF3500 handheld (MIPS32r2, no GPU).
The Doom engine itself is unchanged; the commits on top of the tag are:

1. **SF3500 platform backend** (`src/sf3500/`, small hooks in `i_video.c`, `i_sdlsound.c`,
   `i_sound.c`, `d_main.c`, the CMake files): software rendering handed to the device's
   display driver, SDL_mixer's output written to the device's audio driver, gamepad input,
   and Select + Start to save and quit. L1 / R1 are "No" / "Yes" in menus and previous /
   next weapon in the game.
2. **Music**: the OPL music is scaled by `SF3500_OPL_GAIN` (default set by the launcher),
   and an optional folder of `.wav` tracks (`SF3500_MUSIC_PACK`) can replace it.
3. **WAD loader** (`wadloader/`): a small program that lists the game files and add-on
   levels in `gamedata/` and saves the choice, using the game's own font and logo read from
   the WAD at run time.

## Building

```sh
SF3500_TC=/path/to/toolchain/host MIXER_SRC=/path/to/SDL2_mixer-2.6.3 ./sf3500-build.sh out
```

The toolchain is a MIPS32r2 little-endian GCC (tested with Codescape MIPS MTI GCC 6.3.0)
whose sysroot has SDL2 2.0.14, libpng and zlib. The script builds SDL2_mixer without
modplug into `out/mixer-prefix`, then Chocolate Doom at `-O2`, then the loader, and writes
`out/chocolate-doom.mipsel` and `out/wadloader.mipsel`.

The display and audio drivers are the device's own shared libraries, loaded at run time
(the path comes from `/tmp/tfdevice.env`, falling back to the SF3500 driver).
Licence: GPL-2.0-or-later like the rest of Chocolate Doom, see `COPYING.md`.
