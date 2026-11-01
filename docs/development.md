# Development

How to build the core, run the simulations and use the tools. Run everything from the
repository root in a POSIX shell (Git Bash works on Windows). Docker is needed for Quartus,
the firmware toolchain and Verilator; Python 3 for the scripts.

## Build

```sh
tools/fw.sh          # firmware: build/firmware/firmware.bin and program.bin
tools/build.sh       # Quartus: core/output/bitstream.rbf_r, about 6 minutes
tools/package.sh     # SD card layout in build/sdcard/, copy its contents to the card's root
tools/deploy.sh /i   # package and copy to the SD card mounted at /i (or I:)
```

Quartus Prime Lite 18.1.1 runs in the image `raetro/quartus:pocket`, the version of Analogue's
core template. `tools/build.sh` ends with three summary lines (`tools/buildsum.py`): the
time and the resource use, the smallest slack of every timing check, and the logic of every
unit, each with the change against the previous build. The full log is `build/quartus.log`.
The Pocket needs the bitstream bit-reversed, `tools/reverse_rbf.py` does that.

The firmware is built by GCC 14 and picolibc from Debian trixie in Docker
(`firmware/docker/Dockerfile`); `tools/fw.sh` builds the image on first use. See
[firmware/README.md](../firmware/README.md).

A release holds the bitstream, the core's JSON files, the platform files and the firmware,
never game data. `tools/deploy.sh <sd card> [game folder]` additionally copies your own game
files (default folder `C:/GOG Games/Screamer`, or `$GAME_DIR`) to `Assets/screamer/common/`
and leaves everything else on the card alone.

## Game data

The core reads the container files unchanged. The firmware unpacks them and builds each
track's image at load time (placements, palette, camera path, the drivable parts per grid
cell, markers, walls, the cars from `CARS.DTR`, the race screen, the background, the
opponents' lines and values). Nothing is converted on the PC.

`tools/make_track.py` builds the same track image on the PC. It only serves as a reference
for `tools/check_track_build.sh <game folder>`, which compiles the firmware's builder natively
and compares the result byte for byte.

`L` and `H` in `MAPnL.DAT` / `MAPnH.DAT` are the game's DETAIL sets, not resolutions: the game
loads the `L` set only for LOW and the `H` set from MID on (more scenery, same textures). The
core uses the `H` sets.

Without the game files the core shows a small scene with test patterns.

## PC renderer

`tools/render.sh` runs the firmware's C code natively (`tools/host/render.c`) and writes one
frame or screen as raw RGB, in seconds instead of a simulation's minutes. It needs the game
files in `build/gamedata/`. The header of the script lists its switches (`TRACK`, `CAR`,
`MENU`, `CAM`, `PAUSE`, `CHAMP` and others). `tools/rgb2png.py` and `tools/cmpframe.py` turn
and compare the results.

## Simulation

Verilator runs in Docker (`verilator/verilator:v5.050`). Each testbench lives in
`sim/<name>/`: `files.f` names the top module and the sources, `tb.cpp` drives them.

```sh
# Two framebuffers: load two pictures from the game data over the bridge, swap them by key,
# capture 13 frames and check 12 of them pixel by pixel (one is interrupted by a reset).
python tools/make_screen.py "C:/GOG Games/Screamer/GFX/GRAF.DAT" LOAD2L.PCX \
    build/sim/screen.bin build/preview/LOAD2L.png
python tools/make_screen.py "C:/GOG Games/Screamer/GFX/GRAF.DAT" LOAD1L.PCX \
    build/sim/screen2.bin build/preview/LOAD1L.png
tools/sim.sh display build/sim/screen.bin build/sim/screen2.bin build/sim/display_frame

# The CPU system with firmware and a model of the SDRAM: self-tests, memory test, test band,
# log over the bridge, then two resets with reloaded firmware.
tools/fw.sh
tools/sim.sh soc build/firmware/firmware.bin build/sim/soc
# Fault test: an instruction fetch behind the boot RAM, the trap handler has to report it.
tools/fw.sh FAULT=IFETCH
tools/sim.sh soc build/firmware-IFETCH/firmware.bin build/sim/soc_ifetch 80 c0000001
# Counter-checks: one fault in a memory model at a time, the memory test must fail exactly
# the expected tests.
tools/sim_faults.sh

# Random test of the SDRAM port and controller against the SDRAM model: seed, clocks.
tools/sim.sh sdram 1 2000000
# Self-test of the SDRAM model: one correct and one wrong sequence per rule.
tools/sim.sh sdram_model
# The mixer (audio.v) against its C model firmware/mixer.c: random voices, loops, steps and
# volumes; every output value as it leaves over I2S is compared. Values, seed.
tools/sim.sh audio 20000 1
# The FM music (opl.v with the real JTOPL chip): the write queue's timing, the chip's pitch
# against the register values, the gain, run = 0.
tools/sim.sh opl
# A song on the real chip into a WAV file (events from tools/host/fmdump.c, "events"), to
# listen to and to measure the level: tools/sim.sh opl play <events> <seconds> <out.wav> [gain]
```

The SDRAM model in `sim/models/sdram_model.v` checks the datasheet timings in clocks and can be
built with injected faults for the counter-checks (`SIM_DEFINES`, see `tools/sim.sh`).

`tools/simcheck.sh [--full] [--save] [name]` runs the SoC simulation with the game files from
`build/gamedata/` and summarizes it in a few lines (`tools/simsum.py`): the result, the pixel
comparison with the C models, and the counters of the first race frame with the change against
the saved reference (`--save`). Without `--full` it is the quick run (no resets, and the
finished track image as a testbench slot); `--full` behaves like the Pocket. The audio of the
run (I2S output, 48 kHz stereo) goes to `build/sim/audio.wav`. It also needs `DATA/SOUND.DAT`.

## Tools

| Tool | What it does |
|---|---|
| `tools/dat_sizes.py <file.DAT>...` | lists a game container with unpacked sizes (`-v`: every file) |
| `tools/datfile.py` | reads the containers (shared by the other scripts) |
| `tools/fmcheck.sh [song ...]`, `tools/fmsongs.py`, `tools/host/fmdump.c`, `tools/host/fmref.c` | the FM music on the PC: `fmsongs.py` reads the songs of `DATA/MUSIC.DAT` independently (tracks, messages, lengths); `fmdump.c` runs `firmware/fm.c` against a model of the hardware's write queue; `fmref.c` is openscreamer's driver with the chip replaced by a log; `fmcheck.sh` runs all of them (Docker) and compares the register writes tick by tick |
| `tools/dis.sh [-h] [-f] <from> <to>` | disassembles part of `STARTL.EXE` (`-h`: `STARTH.EXE`) at the addresses of OpenScreamer's notes; `tools/unle.py` (from OpenScreamer) unpacks the program into `build/exe/` on first use |
| `tools/dosram.py` | reads the memory of the original game running in DOSBox, to compare with the firmware |
| `tools/release_zip.py` | after `tools/package.sh`: the release archive `build/release/Chainy.Screamer_<version>.zip` (version from `core/core.json`), refusing to include game data |
| `tools/pocketlog.py [debug.txt]` | reads the Pocket's debug log (default: the SD card at I:) as a short table |
| `tools/platform_image.py` | makes the platform image (`.bin`) from a PNG and back |
| `tools/gen_pll_sdram.sh` | regenerates the SDRAM clock PLL (Intel `ip-generate` in the Quartus image) |
| `tools/sta_sdram.tcl`, `tools/check_budget.py` | after every build: the four SDRAM I/O paths, resource use and I/O registers (called by `tools/build.sh`) |
| `tools/make_budget_svg.py`, `tools/make_timeline_svg.py` | draw `docs/resources.svg` and `docs/frame_timeline.svg` |
| `tools/vexriscv/` | the CPU's configuration and generator, see [tools/vexriscv/README.md](../tools/vexriscv/README.md) |
