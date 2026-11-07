# Development

This document explains how to build the core, run the simulations and use the tools. Run everything from the
repository root in a POSIX shell (Git Bash works on Windows). Docker is needed for Quartus, the firmware
toolchain and Verilator; Python 3 is needed for the scripts.

## Build

This section answers how to build the bitstream and the firmware, and how to put them on the SD card.

```sh
tools/fw.sh          # the release firmware: build/firmware/firmware.bin and program.bin
tools/fw.sh DEBUG=1  # the firmware for development: build/firmware-debug/ (not what a release holds)
tools/build.sh       # Quartus: core/output/bitstream.rbf_r
tools/package.sh     # SD card layout in build/sdcard/, copy its contents to the card's root
tools/deploy.sh /i   # package and copy to the SD card mounted at /i (or I:)
```

Quartus Prime Lite 18.1.1 runs in the image `raetro/quartus:pocket`, the version of Analogue's core
template. `tools/build.sh` ends with three summary lines (`tools/buildsum.py`), each with the change against
the previous build:

- the time and the resource use,
- the smallest slack of every timing check,
- the logic of every unit.

The full log is `build/quartus.log`. The Pocket needs the bitstream bit-reversed; `tools/reverse_rbf.py` does
that.

The firmware is built by GCC 14 and picolibc from Debian trixie in Docker (`firmware/docker/Dockerfile`).
`tools/fw.sh` builds the image on first use. See [firmware/README.md](../firmware/README.md).

**Release and debug builds.** `firmware/debug.h` has the flag `DEBUG`, which is 0 unless the build says
`DEBUG=1`.

- A release runs none of the measuring and diagnostic code. It has no timeline marks and no counters read
  from the units every frame, no line of statistics rewritten in `debug.txt` every frame, no race status
  block, and none of the tools below that live in `ftime.c`, `bench.c` and `mark.c` (the frame time display
  on A + L + R, the benchmark `bench = 1`, the marks on A + X and the free camera on A + Y).
- What stays is the boot log, the files, the settings read from `screamer.cfg` (`far`, `lod`) and the
  simulation's own hooks, which run only in the simulation.
- The simulation (`tools/simcheck.sh`) and the PC tools that read the log use the debug build
  (`tools/simcheck.sh` takes `build/firmware-debug` unless `SIM_FW` names another).
- A release is made from `tools/fw.sh` alone, so a release cannot contain the debug code by accident.
- The host tests of `tools/perf` (`config_test.c`, `ftime_test.c`, `bench_test.c`, `mark_test.c`) compile the
  sources directly: `-DDEBUG=1` for all of them, and `config_test.c` also without it.

A release holds the bitstream, the core's JSON files, the platform files and the firmware, never game data.
`tools/deploy.sh <sd card> [game folder]` additionally copies your own game files (default folder
`C:/GOG Games/Screamer`, or `$GAME_DIR`) to `Assets/screamer/common/` and leaves everything else on the card
alone.

## Game data

This section answers how the core gets the game data and what happens when a file is missing.

The core reads the container files unchanged. The firmware unpacks them and builds each track's image at
load time. The image holds the placements, the palette, the camera path, the drivable parts per grid cell,
the markers, the walls, the cars from `CARS.DTR`, the race screen, the background, and the opponents' lines
and values. Nothing is converted on the PC.

`tools/make_track.py` builds the same track image on the PC. It serves only as a reference for
`tools/check_track_build.sh <game folder>`, which compiles the firmware's builder natively and compares the
result byte for byte.

`L` and `H` in `MAPnL.DAT` / `MAPnH.DAT` are the game's DETAIL sets, not resolutions. The game loads the `L`
set only for LOW and the `H` set from MID on (more scenery, same textures). The core uses the `H` sets.

If a file the game needs is missing, the core lists all its files with [OK] or [MISSING] and stops there.
`SCREAM.gog`, `SCREAM.DAT` and `MUSIC.DAT` are optional: they only bring the music.

## PC renderer

This section answers how to render a frame or a screen of the firmware on the PC.

`tools/render.sh` runs the firmware's C code natively (`tools/host/render.c`) and writes one frame or screen
as raw RGB, in seconds instead of a simulation's minutes. It needs the game files in `build/gamedata/`. The
header of the script lists its switches (`TRACK`, `CAR`, `MENU`, `CAM`, `PAUSE`, `CHAMP` and others).
`tools/rgb2png.py` and `tools/cmpframe.py` turn and compare the results.

## Simulation

This section answers how to run the hardware simulations and what each one checks.

Verilator runs in Docker (`verilator/verilator:v5.050`). Each testbench lives in `sim/<name>/`: `files.f`
names the top module and the sources, and `tb.cpp` drives them.

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

The SDRAM model in `sim/models/sdram_model.v` checks the datasheet timings in clocks. It can be built with
injected faults for the counter-checks (`SIM_DEFINES`, see `tools/sim.sh`).

`tools/simcheck.sh [--full] [--save] [name]` runs the SoC simulation with the game files from
`build/gamedata/`.

- It summarizes the run in a few lines (`tools/simsum.py`): the result, the pixel comparison with the C
  models, and the counters of the first race frame with the change against the saved reference (`--save`).
- Without `--full` it is the quick run (no resets, and the finished track image as a testbench slot).
  `--full` behaves like the Pocket.
- The audio of the run (I2S output, 48 kHz stereo) goes to `build/sim/audio.wav`.
- It also needs `DATA/SOUND.DAT`.

## Frame statistics

This section answers how to count what a frame asks of the CPU and the units without a simulation, and how
to turn the counts into a frame time.

`tools/perf/framestats.sh` counts what a frame asks of the CPU and the units on the PC. It builds copies of
the firmware's `track.c`, `geom.c`, `setup.c` and `raster.c` with counters (`perf_patch.py`; the firmware is
never changed) and draws frames with the C models of the units. It draws either along a track's road with
the near chase camera and a number of cars (`sweep`) or in a race of a robot at the keys (`race`).

Each frame is a line of a CSV with:

- placements tested and pieces handed on,
- faces looked at,
- vertices,
- pixels,
- the misses of the texture cache (16 KB) and of the geometry unit's read cache (4 KB),
- a hash of the sorted list of pieces.

The counters agree with the simulation (pieces and faces equal, texture misses within 0.4 %).

`tools/perf/perf_model.py` turns the counters into a frame time. It is a model of the CPU phases and of the
geometry unit and rasterizer, calibrated on the six start frames of the simulation. Each start frame is
predicted from the other five. The error is:

| Firmware | rms error | worst error |
|---|---|---|
| with the original's distances, without the level of detail | 1.8 % | 3.4 % |
| with the cell window and the level of detail | 4.4 % (the model is 3 % low) | 8.2 % |

The frames of the firmware with the cell window and the level of detail are CPU-bound as often as
geometry-bound.

`tools/perf/check_cull.sh <commit>` checks that a change of the cull hands the same pieces to the geometry
unit, in the same order, in every frame of road sweeps and races of all six tracks, as the firmware of an
earlier commit does.

### Marks and the free camera

This section answers how to find out what the picture did at one place, for example a building that is gone
for a frame. These tools need the debug build, `tools/fw.sh DEBUG=1`.

- **Marks.** In a race, hold A and press X (`firmware/mark.c`). The firmware writes
  `mark 3 track 4 view 2 frame 812: x y h yaw pitch roll | ...` to `debug.txt` with the camera of that
  picture and of 15, 30 and 45 frames before (a press comes late; view 9 is the free camera). It also shows
  `MARK 3` on the screen. The log keeps 1,600 bytes for the marks, about ten. The race's long status block
  with the timeline goes to the log once a session on the Pocket, because it fills the 4 KB in the first
  race.
- **Free camera.** Hold A and press Y. The cars, the clock and the scenery stand still, and the camera
  flies: the D-pad moves forward / back and turns, L R go down / up, Y X tilt, and B is fast. A with X marks
  the place, and Start gives the car's camera back.
- `tools/perf/mark_test.c` tests the lines and the camera's directions on the host.

The marks are for the PC:

- `CAM="x y h yaw pitch 0" TRACK=n DEFS="-DFAR_SCALE=... -DLOD_SCALE=..." tools/render.sh out.rgb` draws the
  C model's picture from a mark, with the settings the Pocket had (the first lines of `debug.txt`).
- `tools/perf/framestats.sh drop <track image> <screen lines> <file of marks "x y h yaw pitch"> [radius]`
  (`dropcheck.c`, with `DROP_ALL=1`, `DROP_ROWS=1`, `DROP_DRAW=1` for more) says for each placement why it is
  drawn from one mark and not from the next: its cell is outside the window, it is out of reach, it is
  behind a view plane, or it draws something else. With this tool, the far buildings that came and went on
  track 5 were traced to the window's edge.

### Draw distance and level of detail

This section answers how to set the draw distance and the level of detail, and how to measure their effect.

`FAR_SCALE` and `LOD_SCALE` in `firmware/track.h` set how far the core draws and how far out its blocks keep
their detail, each a factor on the original's distances (`docs/architecture.md`, "Draw distance and level of
detail"). They are the defaults.

**`screamer.cfg`.** The optional file `screamer.cfg` (data slot 26, `firmware/config.c`; `config.h` has the
syntax: `far = 2.0`, `lod = 2.5`, `lod = off`) replaces the defaults at the start (`track_set_scales`).

- The values are kept within far 0.5 .. 3.0 and lod 0, 0.5 .. 4.0.
- The line of the file goes to `debug.txt` ("config: screamer.cfg: far 2.00 lod 2.50").
- The numbers in force are on the frame time display's second line.
- Values above 2.0 have not been measured. With 2.0 for both, 0 pixels differ from the C models on all six
  tracks, and a race's frames are 0.1 to 0.9 ms longer than at 1.5.

**Benchmark.** `bench = 1` (`firmware/bench.c`, `bench_run` in `racedebug.c`) races track 1 .. 6 in turn with the
first car, the options as set, and the gas held (`bench_frames`: the simulation's race on the Pocket, the
start lights' countdown at once, no keys read). 250 pictures are counted for a track (the first is not).

- It writes one line for each track to `debug.txt` and to a screen that waits for a button, for example
  `bench track 3: far 2.00 lod
  2.00, 250 frames avg 15.0 max 19.2 late 0 faces 1724 / 1961 pieces 324 / 377`. Faces and pieces are given
  as the mean a frame and as the longest frame had them.
- `dlog_keep` holds 1200 bytes of the log back for it, and the races write no timeline lines meanwhile.
- `tools/perf/bench_test.c` checks the lines on a known summary.

**Tests.**

- `tools/perf/config_test.c` runs the parser on texts a player might write (a comma as the decimal point, a
  trailing x, comments, out-of-range numbers, unknown names). `tools/perf/ftime_test.c` tests the display's
  mean and its third line. Both compile with the host's gcc and stub out the rest.
- For a test of one value without a file, `-DFAR_SCALE=1.25f` on the compiler's line overrides the defaults,
  e.g. `DEFS="-DFAR_SCALE=1.0f -DLOD_SCALE=0"
  tools/render.sh ...`. For the PC tools `FARSCALE` and `LODSCALE` in the environment do the same without
  compiling again.

**Study.**

- `tools/perf/study_sweeps.sh` runs the study's matrix: every combination of the far plane's factor 1.0 /
  1.25 / 1.5 and the level of detail's none / 1.0 / 1.25 / 1.5, over a robot's race on each track.
  `tools/perf/study.py` prints its tables.
- `POPIN=d` for `framestats.sh sweep` counts the pixels that change when the levels' distances move by d
  (`POPIN_DUMP=n` keeps the pictures of such frames).

**Comparison with the original.**

- `python -I tools/dosram.py walk 1
  14 6 6 6` starts a DOSBox of its own (it keys and reads no other), drives the original's race with the gas
  held, pauses it and reads it every few seconds. It reads the camera, the cell window, the list of pieces in
  reach and the level-of-detail constants, into `build/dosbox/walk1.json`.
- `python -I tools/perf/walkcheck.py build/dosbox/walk1.json
  build/check/track1.bin` compares them with the firmware's window and cell table. `dosram.py shots` takes
  the grid's picture in the game's four views (the camera of each as `CAM` for `tools/render.sh`).
- Set `GAME_DIR` to a private copy of the game for these: the game writes into its folder.

## Tools

This section lists the tools and what each one does.

| Tool | What it does |
|---|---|
| `tools/dat_sizes.py <file.DAT>...` | lists a game container with unpacked sizes (`-v`: every file) |
| `tools/datfile.py` | reads the containers (shared by the other scripts) |
| `tools/fmcheck.sh [song ...]`, `tools/fmsongs.py`, `tools/host/fmdump.c`, `tools/host/fmref.c` | the FM music on the PC: `fmsongs.py` reads the songs of `DATA/MUSIC.DAT` independently (tracks, messages, lengths); `fmdump.c` runs `firmware/fm.c` against a model of the hardware's write queue; `fmref.c` is openscreamer's driver with the chip replaced by a log; `fmcheck.sh` runs all of them (Docker) and compares the register writes tick by tick |
| `tools/dis.sh [-h] [-f] <from> <to>` | disassembles part of `STARTL.EXE` (`-h`: `STARTH.EXE`) at the addresses of OpenScreamer's notes; `tools/unle.py` (from OpenScreamer) unpacks the program into `build/exe/` on first use |
| `tools/dosram.py` | reads the memory of the original game running in DOSBox, to compare with the firmware (only a DOSBox it started itself, or `DOSBOX_PID`'s): `walk`, `shots`, see "Draw distance and level of detail" |
| `tools/release_zip.py` | after `tools/package.sh`: the release archive `build/release/Chainy.Screamer_<version>.zip` (version from `core/core.json`), refusing to include game data |
| `tools/perf/framestats.sh`, `perf_model.py`, `check_cull.sh`, `study_sweeps.sh`, `study.py`, `walkcheck.py` | frame statistics on the PC, the frame-time model, the check that a cull hands on the same pieces, the draw-distance study, the check of the cell window against the original; see "Frame statistics" and "Draw distance and level of detail" |
| `tools/pocketlog.py [debug.txt]` | reads the Pocket's debug log (default: the SD card at I:) as a short table |
| `tools/platform_image.py` | makes the platform image (`.bin`) from a PNG and back |
| `tools/gen_pll_sdram.sh` | regenerates the SDRAM clock PLL (Intel `ip-generate` in the Quartus image) |
| `tools/sta_sdram.tcl`, `tools/check_budget.py` | after every build: the four SDRAM I/O paths, resource use and I/O registers (called by `tools/build.sh`) |
| `tools/make_budget_svg.py`, `tools/make_timeline_svg.py` | draw `docs/resources.svg` and `docs/frame_timeline.svg` |
| `tools/vexriscv/` | the CPU's configuration and generator, see [tools/vexriscv/README.md](../tools/vexriscv/README.md) |
