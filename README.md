<img src="core/dist/platforms/_images/screamer.png" alt="Screamer" width="521">

# Screamer for the Analogue Pocket

An [openFPGA](https://www.analogue.co/developer) core that runs *Screamer* (Graffiti / Virgin
Interactive, 1995, MS-DOS) on the Analogue Pocket.

The game's logic runs as C firmware on a RISC-V soft core (VexRiscv, 74.25 MHz), and the 3D
work is done by dedicated hardware units: geometry, triangle setup, rasterizer, blitter and an
audio mixer. The core reads the **unchanged original game files** from the SD card. No game
data is part of this repository or of its releases.

<img src="docs/images/race.png" alt="A race on the Pocket: the start lights, a red car, the lap and position display" width="640">

Video: [Screamer running on the Analogue Pocket](https://youtu.be/lczQRB2F0cM)

## Features

- All six tracks, all cars, the opponents' cars, props and moving scenery
- Normal race, championship, time attack with ghost, cone carnage and slalom
- Replays with the game's trackside cameras, the pause screen with its working CD player
- The game's menus, options, records and name entry, saved between sessions
- Music from the game's CD image, or without it the MIDI songs through an FM chip (OPL2, the
  AdLib sound of the original), sound effects and speech
- 50 frames per second at the Pocket's native 320 x 200 picture, perspective-correct textures
- Pocket integration: Core Controls remapping, analog sticks and triggers in the dock

Not supported: network play (the menu entry is left out), the link port.

## Installation

You need an Analogue Pocket (firmware 1.1 or newer) and your own copy of the game.

So far the core has only been tested with the GOG release of the game. Other releases might
work if their files are the same.

### GOG version

1. Copy the contents of the release archive to the root of the Pocket's SD card. It adds
   `Cores/Chainy.Screamer/`, `Platforms/` and `Assets/screamer/Chainy.Screamer/`.
2. Copy these files from the game's install folder to `Assets/screamer/common/` on the SD
   card. Keep the folders and copy the files as they are, don't convert or rename anything:

   | In the install folder | Files |
   |---|---|
   | `GFX/` | `GAME.DAT`, `GRAF.DAT`, `MENU.DAT`, `MAP1H.DAT` ... `MAP6H.DAT` |
   | `DATA/` | `GEN.DAT`, `LOCAL.DAT`, `SOUND.DAT`, `MUSIC.DAT`, `MAP1.DAT` ... `MAP6.DAT` |
   | top level | `SCREAM.gog` and `SCREAM.DAT` |

   The last two are the music: the game's CD soundtrack, which GOG ships as a CD image
   (`SCREAM.gog`) with its cue sheet (`SCREAM.DAT`). They sit next to the `GFX` and `DATA`
   folders, so that for example `Assets/screamer/common/GFX/GAME.DAT` and
   `Assets/screamer/common/SCREAM.gog` exist. `MUSIC.DAT` holds the MIDI songs that the original
   played through an AdLib card when it found no CD; the core plays them on an FM chip. The
   Pocket menu's *Music Source* is *Auto* (the CD if its image is there, else FM) or *FM Synth*
   (always FM).
3. Start *Screamer* from the Pocket's core list.

Saves and the debug log go to `Saves/screamer/Chainy.Screamer/`. `debug.txt` helps when
something goes wrong; the Pocket writes it when the core is quit.

## Controls

The defaults can be changed in the Pocket menu under *Core Controls*.

| Action | Button |
|---|---|
| Accelerate / OK | B |
| Brake | Y |
| Change camera | X |
| Back | A |
| Gear up / down | R / L |
| Pause | Start |
| Quit race | Select |

## Building

Quartus Prime Lite 18.1 and the firmware toolchain run in Docker, so you need Docker, a shell
and Python 3. From the repository root:

```sh
tools/fw.sh          # firmware: build/firmware/firmware.bin, program.bin
tools/build.sh       # bitstream (about 6 minutes): core/output/bitstream.rbf_r
tools/package.sh     # the SD card layout in build/sdcard/
tools/deploy.sh /i   # package and copy to the SD card at /i, with your game files
```

## Credits

- [OpenScreamer](https://github.com/ItsDanik/openscreamer) (GPL-3.0): the reference for the
  game's logic and file formats
- [VexRiscv](https://github.com/SpinalHDL/VexRiscv) (MIT): the CPU
- Analogue's [core template](https://github.com/open-fpga/core-template): `core/src/fpga/apf/`
  and the JSON templates in `core/`
- [font8x8](https://github.com/dhepper/font8x8) (public domain): the firmware's test font

*Screamer* belongs to its respective owners. This project is not affiliated with them or
with Analogue.

## License

GPL-3.0, see [LICENSE](LICENSE).
