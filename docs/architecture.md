# Architecture

This is an overview of the Screamer core: the hardware partition, the memory layout, the game data on the
SD card, how the original's behaviour is reproduced, and the measured resource budget.

Other documents hold the details: the CPU and the bus are in `docs/soc.md`, the memory system in
`docs/memory.md`, the fill rule in `docs/original_filler.md`, and the geometry formats in
`docs/original_geometry.md`.

Addresses in backticks (`0x276a0`, ...) are code addresses in the original `STARTL.EXE`. A rule taken from
the original is followed by its address, written as (original: STARTL.EXE `0x83164`) or as the bare address.
"OpenScreamer `docs/NOTES.md`" is the notes file of the OpenScreamer port.

## Pocket hardware (main FPGA)

This section lists what the Pocket's FPGA offers and which of it the core uses.

The FPGA is a Cyclone V 5CEBA4F23C8 with 18,480 ALMs, 308 M10K blocks (10 Kbit each), 66 DSP blocks and
4 PLLs. The memory chips sit on separate buses, all 16 bits wide: SDRAM 64 MB, PSRAM `cram0` and `cram1`
16 MB each, and SRAM 256 KB.

The core uses only the SDRAM. It has no controllers for the SRAM and the PSRAM: only the memory test needed
them, and hints for adding them again are in `docs/memory.md`.

## Partition

This section shows which unit does which part of a frame, and which memory holds which data.

![Architecture: the CPU selects and sorts the parts and writes them into the geometry unit's buffer; geometry unit, clipper, setup unit and rasterizer draw the frame into the back framebuffer in block RAM while video_out shows the front one](architecture.svg)

[frame_timeline.svg](frame_timeline.svg) shows where the time of a frame goes: the CPU sections and the
utilisation of the units over 20 ms. `tools/make_timeline_svg.py` draws it from the log, also from the
Pocket's `debug.txt`.

The CPU selects the parts in range, sorts them back to front and writes them into the geometry unit's
buffer. The geometry unit, the clipper, the setup unit and the rasterizer work on the parts concurrently.
The rasterizer reads texels from the SDRAM through its texture cache and draws into the back framebuffer,
while `video_out` shows the front one. The buffers are swapped in the vertical blanking.

| Memory | Content |
|---|---|
| SDRAM | Program, geometry, game state, textures of the current track (`MAPn.MLB`, up to 3 MB, through the 16 KB texture cache) |
| SDRAM | Sound effects (`SOUND.DAT`, 1.9 MB, unpacked; the mixer reads the samples directly) |
| SDRAM | Music: the ring of the CD stream (256 KB), or the MIDI songs and instrument banks for the FM chip (`MUSIC.DAT`, 0.2 MB, read by the firmware) |
| Block RAM | Two framebuffers (2 x 64 KB) and two palettes, boot RAM, CPU caches, geometry unit buffer, texture and mesh caches, transparency table (`MAPn.SMG`), the mixer's lines (32 x 32 bytes), the FM chip's write queue (256 x 32 bits) and its ROMs and delay lines |
| SRAM, cram0, cram1 | not used |

One texture cache line costs about 30 clocks from the SDRAM and about 130 from the (asynchronous) PSRAM.
Therefore everything is in the SDRAM, including the sounds.

The mixer reads the sounds slowly and evenly. For each voice it keeps a 32-byte line around its position in
block RAM and requests the next line as soon as the position moves into it. A whole output sample (1547
clocks) passes before that line is read, so any wait is absorbed.

## Memory needed by a race (measured, unpacked)

This table shows how much memory each kind of data takes during a race, measured with the files unpacked.

| Data | File | Size |
|---|---|---|
| Textures | `GFX/MAPnL/H.DAT` -> `MAPn.MLB` | 2.3-2.9 MB |
| Track geometry | `MAPn.UNR` | 0.6-1.6 MB |
| Cars, props | `*.3DX` | about 270 KB |
| Map grid | `MAPn.MMP` | 100-240 KB |
| Sky | `MAPn.BCK` | about 70 KB |
| Racing lines, AI, cameras | `DATA/MAPn.DAT` | 17-41 KB |
| HUD, cockpit | `GFX/GAME.DAT` | 0.5 MB |
| Effects (8-bit PCM) | `DATA/SOUND.DAT` | 1.9 MB |

## Game data on the SD card

This section answers which files the core reads from the SD card, in which data slot each one is, and how
much of it is loaded.

The core reads the original files, unchanged, in their folders under `Assets/screamer/common/`: the game's
`DATA` and `GFX` folders as they are. The Pocket accepts a folder in a slot's file name, e.g.
`GFX/MAP1H.DAT`. Each file has one data slot, with `deferload`.

The core reads the table of contents at the end of a file and fetches the entries it needs. The firmware
unpacks them (`dat.c`, LZW) and builds the track image at start-up (`track_build.c`). The image is byte for
byte what `tools/make_track.py` builds on the PC. In the simulation the build takes 3.2 s: 1 s of unpacking,
and the rest mostly the camera path in `double` and the checksums.

The game has these files:

- `DATA/`: 16 files (`GEN`, `LOCAL`, `MUSIC`, `SOUND`, `MAP1`-`MAP6`, `MAP1I`-`MAP6I`).
- `GFX/`: `GAME`, `GRAF`, `MENU`, `MAP1L`-`MAP6L`, `MAP1H`-`MAP6H`. L and H are the two sets of the DETAIL
  option: L at LOW, H from MID. They are not the resolution.
- `TABS/`: `PERSP01.TAB` and `VIS.TAB`, the original's perspective divide table and its visibility table.
  The perspective table gives the same values as the formula the geometry unit computes. The core does not
  read these two files, and the folder is not needed.

All of these files together would be 33, one more than the 32 slots. The core always runs the HI detail
set, so it does not load the six L tracks. It loads `MUSIC.DAT` as the fallback for the CD music: the file
holds the songs and instruments of the FM music, which the original played when it found no CD. This leaves
19 game files, plus the CD image and two save slots.

The slots are:

- Track 1: `MAP1H.DAT`, `GAME.DAT`, `MAP1.DAT`, `GEN.DAT`, `LOCAL.DAT` (slots 3 to 7).
- `SOUND.DAT` (slot 8, unpacked; the mixer plays the samples from the container).
- Tracks 2 to 6: `MAP2H.DAT`-`MAP6H.DAT` (slots 9 to 13) and `MAP2.DAT`-`MAP6.DAT` (14 to 18). Once the
  track is chosen, they are loaded to the same addresses as track 1's.
- `GFX/MENU.DAT` (slot 19) and `GFX/GRAF.DAT` (slot 20) for the track and car selection. The core reads only
  the table of contents and the entries it needs, each at its place as in the file (`load_parts` in
  `main.c`). That is 2.4 MB instead of 5.9 MB, and about 1.8 s instead of 4.5 s at the Pocket's measured
  1.3 MB/s. The large entries are `MENU.MLB` (1.4 MB, read whole because the cars' textures are spread over
  the whole file), the six loading pictures `LOADnL.PCX` (0.28 MB, about 0.2 s) and the eight championship
  pictures `CHAMPnL.PCX` (0.33 MB, about 0.25 s).
- The CD image at the top of the game folder: `SCREAM.DAT` (slot 21, the cue sheet) and `SCREAM.gog` (slot 22,
  383 MB). The core reads only the music from it, 32 KB at a time while it draws (see "CD music").
- Two nonvolatile slots for the saves, `CHOICE.DAT` (23) and `ATTACK.DAT` (24). They are in the Pocket's save
  folder, not with the game files (see "Championship").
- `DATA/MUSIC.DAT` (slot 25, 200 KB): the MIDI songs `MUSIC1.HMP` .. `MUSIC7.HMP` and the instrument banks
  `MELODIC.BNK` and `DRUM.BNK`. The core reads it whole, once, when the FM music is first needed (see
  "FM music"). That is at start if there is no CD image or if the Pocket menu's Music Source says FM Synth,
  and otherwise when FM Synth is chosen.
- `screamer.cfg` (slot 26, up to 1 KB, optional, in the common folder): the factors of the draw distance and
  the level of detail (`firmware/config.c`). The core reads it once at the start into a buffer of the
  program's own and parses it. Without the file the constants of `track.h` stay.

`GAME.DAT`, `GEN.DAT`, `LOCAL.DAT` and `SOUND.DAT` are loaded once. `SOUND.DAT` (1.9 MB, 1.3 s) loads while
the first track is built: the firmware only starts the read (`sound_start` in `main.c`), and the Pocket
writes the file into the SDRAM while the CPU builds.

## Design decisions

This section lists the main design choices and the reason for each.

- **Language:** own code in Verilog (like the template). VexRiscv is generated once from a Scala
  configuration as Verilog. Custom CPU instructions go through its CFU plugin.
- **50 Hz, one game step per frame:** The original has no fixed rate. Its physics runs with the measured
  frame time, limited to 0.02-0.2 s, so it never runs faster than 50 steps per second (OpenScreamer
  `docs/NOTES.md`, driving model). Video therefore runs at exactly 50 Hz (480 x 512 clocks at 12.288 MHz;
  the Pocket's screen takes 47-61 Hz), and each frame is one 20 ms physics step. This is the game at its
  best rate, without intermediate frames or judder. The clock for countdowns and lap times ticks at 100 Hz
  in the original and has its own counter register. One frame is 1.485 million clocks.
- **VGA version:** The core draws 320x200 with an 8-bit palette, like the game's VGA version.
  - The original takes its tracks from both sets: `MAPnL` at DETAIL LOW and `MAPnH` from MID (`STARTL.EXE`
    like `STARTH.EXE`, OpenScreamer `docs/NOTES.md` section 27).
  - The original's distances are set by DETAIL. Parts are drawn up to 52000 units. This limit is three
    immediates of the code, at `0x831cc`, `0x83230` and `0x8a70f`, which nothing writes. Faces are clipped at
    depth 27808 (LOW, MID) or 43808 (HI).
  - The SVGA version draws just as far: it has the same table at `ds:0x21d8` in `STARTH.EXE` and the same
    comparison with 52000^2/4 at `0x8340b`. It only doubles the pixels (focal length 128, result x 2,
    centre column 319).
  - The core always uses the full H set and the HI distances times `FAR_SCALE` (default 2.0,
    `firmware/track.h`; see "Draw distance and level of detail" below). DETAIL is only displayed.
  - Cockpit, race info and other static pictures are sprites from the L entries of `GAME.DAT`
    (`COCKnnAL`, `COCKnnBL`, `CDFRONTL`, `GAMEL`, `IMPLET_L`, `IMPRES_L`). The blitter copies them 1:1,
    pixel-exact, into the framebuffer. In `GAME.DAT`, L and H mean the 320 x 200 and the SVGA pictures, not
    the DETAIL sets of the `MAPn` files.
- **Two framebuffers in block RAM** instead of PSRAM. The rasterizer writes the back buffer at one pixel
  per clock, so overdraw costs nothing. No PSRAM controller or framebuffer arbitration is needed, which
  saves logic, the scarce resource.
  - Each buffer has its own palette, so palette changes (fades) match the frame exactly.
  - The drawing side also reads, because smoke and shadows blend with the frame content (routine 224).
    The buffers are therefore four explicit 8-bit memory lanes with a read/write port (`fb_lane.v`).
    Otherwise Quartus 18.1 silently builds memory with true byte enables from registers.
    `tools/check_budget.py` checks the 128 M10K on every build.
  - The swap happens at the start of a line in the vertical blanking, at most once per blanking
    (`video_out.v`). A frame that takes slightly more than 20 ms therefore does not drop to 25 fps at once.
- **Exact per-pixel perspective correction:** A reciprocal unit (table plus one Newton step, 2-3 DSP)
  delivers one reciprocal per clock, so it costs no time. OpenScreamer's software renderer computes only
  every eighth pixel exactly, hence its kinks in near textures. The fixed-point bit widths were determined
  on the C model against floating point.
- **Number formats:** The rasterizer and the geometry use fixed point (screen coordinates with at least
  4 fractional bits). The physics on the CPU uses `float`, like the original.

### Visibility without a depth buffer (painter's algorithm)

This section answers how faces get into the right drawing order without a depth buffer.

The CPU sorts whole objects by the distance of their centre from the camera and emits them back to front
(original: STARTL.EXE `0x83164`, key `dist^2 >> 2`).

Within an object the order is in the data. Each mesh block has sixteen precomputed face lists (header
+0x28..+0x64). Each list is sorted back to front for one viewing direction: a 45-degree sector around the
block's y axis, for an eye above (lists 0..7) or below (8..15) the block's x-z plane. The CPU picks the list
like the game (original: STARTL.EXE `0x8a598`). It uses the camera in the block's frame plus the block's
centre. The game's `0x83511` scales this by 1.2, which does not change the list. The geometry unit does not
sort, and it drops back faces by the normal.

The game tests back faces with the same point (`0x835a0`). Its filler (`0xd55ff`), however, draws nothing of
a face whose corners run the other way on screen, that is, a face facing away from the lens eye (128 behind
the camera). The geometry unit's back-face test therefore uses this eye. With the point alone, the nose of
the first car in the car selection looked through the front tyre.

The track data rely on this order (grass faces below the road, two objects at one spot). OpenScreamer needs
special rules for it with its depth buffer. Clipping at the near plane remains necessary. A depth buffer
would cost about 125 M10K (320x200x16 bit fits only the 8-Kbit modes), and every hidden pixel would be
rewritten, which makes framebuffer bandwidth more important.

**Object order as in the original** (read from `STARTL.EXE`). The order of the objects is built in four
steps:

1. `0x83164` builds the list from the grid cells, per cell run 1 before run 0, with `dist^2 >> 2`.
2. `0x8a6a8` appends the moving objects.
3. `0x8a7b0` turns the distance into the key -(sqrt(dist^2 >> 2) rounded >> 2), roughly distance/8.
4. `0x832c0` sorts. The farthest object is drawn first.

A moving object (car) counts **800 closer**, that is 6400 units. Otherwise the long road piece it stands on
overpaints the car, on a lap with the near chase camera at every fourth spot.

A mesh with a class in the last word of its object header (+0x32c) counts farther away: class 3 by 6000
(on track 1 only `akk1tap`, the grass under the road), class 1 by 1500, class 2 by 800, class 4 by 200.

OpenScreamer draws with a depth buffer and has its own rules for this (grass "twice as far", objects above
their road). The core draws like the original and needs only its key.

### Geometry chain

This section answers which units process the geometry and what each one takes over from the CPU.

On the CPU alone (measured in a fly-through of track 1) a triangle takes about 3000 clocks. Almost half of
that is per-triangle preparation (the planes of Q, U, V with divisions), a quarter is clipping and
projection, and the vertex transformation is only 3 %. These units take over the work:

- **Setup unit** (`setup.v`, C model `firmware/setup.c`): The CPU hands over polygons in camera view (five
  floats per corner). The hardware projects them, splits them into a fan and computes the rasterizer's values
  in fixed point, while the rasterizer draws the previous triangle.
- **Clipper** (`clipper.v`, in front of the setup unit): With the setup unit alone, the CPU time in the
  heaviest frame drops from 10.4 to 5.8 million clocks, and 58 % of it is clipping. The clipper clips as the
  original does, at the near plane (depth 128) and at the far plane of the DETAIL level. A guard band at
  14 x depth is there only so that screen coordinates fit 16 bits (the rasterizer clips at the screen edge).
  The lens is the original's: focal length 128, eye height at 65 % of the frame height, and the eye 128
  units behind the camera. The division at `0x83420` reads `TABS\PERSP01.TAB`; the screen point is
  the centre + 128 * x / (128 + z), with z from the camera.
- **Geometry unit** (`geom.v`, C model `firmware/geom.c`): The CPU selects and sorts the parts and writes 19
  words per part (matrix, origin, eye, mesh, face list; layout in `docs/soc.md`) into the unit's buffer
  (2048 words, 107 parts, block RAM). The unit reads the mesh itself from the SDRAM, transforms the vertices,
  drops back faces, looks up the materials and hands the faces to the clipper. Because of the buffer the CPU
  does not wait for each part. The unit needs about 20 % of a frame; the rest is left for cars and game
  logic.
- **Texture prefetch:** Without the prefetch, counters in the rasterizer showed 76 % of its time waiting for
  texture lines. They were measured while both caches fetched each missing line twice, and a cache study with
  the C model's texel addresses showed half as many misses as counted. Both caches fetch each missing line
  once. Tiling the texture memory brings little, because most misses are first accesses (about 7,800 lines per
  frame). So the rasterizer prefetches (after Igehy, Eldridge, Proudfoot 1998):
  - Tags are checked ahead of a queue of 64 pixels.
  - A missing line is requested at once (up to 16 in flight, into a line buffer).
  - Only the pixel that needs the line waits at the queue head.
  - The line enters the cache once all pixels before it have read the old line.
  - The next textured triangle starts as soon as the previous one's pixels are in the queue.

  In the heaviest frame the rasterizer is busy 0.49 instead of 0.65 million clocks.
- **Cadence:** The geometry unit sets the pace. It is busy from the first part to the end of the frame
  (including waits for the clipper and for memory). The frame time is roughly the CPU's selection plus the
  geometry unit.

**Faster chain at the start.** The start frame is the heaviest one: all ten cars are in view, with 248 parts
and 2,473 faces. Three changes shorten it. They were measured in the SoC simulation with
`SIM_DEFINES="-DGEOM_STATS"`.

1. A geometry-unit read puts the address straight to the cache RAMs, and the next step takes the value (2
   clocks on a hit; on a miss only that step waits). Two more multipliers compute the three rows of a vertex
   at once (6 instead of 12 clocks).
2. The line port has up to two lines in the SDRAM controller, and the texture loader requests two
   (`docs/memory.md`).
3. The firmware builds the cars only when they are due in the draw sequence (the near ones last, while the
   geometry unit works on the far scenery). During culling it skips groups of 16 parts whose outline is out
   of range.

Without the changes a frame took about 1.6 million clocks (25 fps). The geometry unit was busy 1.15 million
clocks (37 % waiting for reads that cost 5 clocks even as cache hits, 23 % for SDRAM misses), the rasterizer
waited 289,000 clocks for texture lines, and the CPU 414,000 for the geometry unit.

With the changes a frame takes 996,000 clocks (67 % of the 1.485 million clocks of a frame). The geometry
unit is busy 812,000, the rasterizer waits 193,000 clocks for textures, and the CPU 28,000 for the geometry
unit.

Bigger caches bring little. A study with the start frame's accesses (C model) gave these numbers of misses:

- Geometry: 8,738 with 4 KB and 6,368 with 16 KB.
- Textures: 11,200 with 16 KB and 10,261 with 32 KB. With 8x4 tiled textures the number is higher (13,895).

Most of them are first accesses to about 360 KB of texture and 280 KB of mesh data per frame.

### Corners on whole pixels, fill rule

This section answers why the corners of a triangle are cut to whole pixels, and which pixels a polygon
covers.

The original's projection (`0x83420` ff.) keeps x as 16.16 and uses only the upper half word. All its fill
routines (`0xd5350` textures, `0xd8844` one colour) read the corners as whole pixels from the table at
`0x54e94`. Corners of adjacent parts that are a few units apart in the world (the stored angles are up to 2.5
units short) mostly land on the same pixel. The setup unit and the rasterizer therefore truncate corners to
whole pixels instead of rounding to 1/16 pixel.

The fillers run column by column from the smallest to the largest corner x, both columns included. In each
column they fill from the clipped top edge to the clipped bottom edge, both drawn:

- Each edge pushes N + 1 pairs, and the pair at the vertex is overwritten (`0xda330`).
- The fill takes `sar 16`, then draws count + 1 pixels (`0xda478`).

A pixel (x, r) is thus drawn if the polygon touches the segment from (x, r) to (x, r + 1), the lower end
excluded. This makes every polygon about half a pixel fuller all round than the "pixel centre inside the
triangle" rule. Neighbours overlap at the seam, also where a corner lies on the other's edge (T junction),
and asphalt stays closed.

Counting the hole pixels at the four steepest slants of track 1 compares the two rules. With the centre rule
and corners on whole pixels, 61 hole pixels remain. With the original's rule there are only 9, columns
between parts that do not touch.

The rasterizer stays line by line, because the framebuffer holds four pixels of a line in one word. It
computes the same set: per line both edges at the top and bottom edge. See `docs/original_filler.md`.

### Handoff between frames (rules for CPU and graphics)

These rules decide when a frame may be swapped and which buffer belongs to whom.

- The swap is requested only when the last pixel of the frame is written. An end marker runs through the
  whole graphics pipeline, and only then does `swap_req_toggle` flip.
- Palette changes are in frame N's command list. They are written into the back buffer's palette when
  frame N begins. The CPU never writes the palette directly, because it is already a frame ahead.
- The drawer knows its back buffer from its own toggle (back = not `req`) and waits only for the
  acknowledgement. It does not read `front` across the clock domain.
- A reset must not fake a swap. It sets request and acknowledgement to 0, after which buffer 0 is in front
  until the drawer swaps (checked in `sim/display` and `sim/soc`).

### Draw distance and level of detail

This section answers how far the core draws, how it chooses coarser meshes, what the original does, and what
this costs in frame time.

**What the original draws** (`STARTL.EXE`, DETAIL HI; `docs/original_geometry.md` 2.1, 2.2, 4.4):

1. The **window** is the set of cells whose placements are looked at. It is the box of two points 57,433
   units sideways and 56,096 units ahead of the camera, turned by its heading, with 8192 round it. The box is
   cut to the view box of the cell the camera is in, if the map gives that cell one. 41 to 63 % of a track's
   cells have one.
2. Of the placements in these cells, those within 52,000 units are kept (the reach).
3. Then follow the sphere test against the view and the far plane at 43,808 units.
4. For every block, a **level of detail** is chosen by the distance of its middle. A placed block takes
   level 1 from 12,000 units and level 2 from 16,000, and a car's or other moving block takes them from 4,000
   and 7,000 (a car's level 2 is its body alone).

The four constants are in the program image, and nothing writes them. They read -6000, -8000, -4000, -7000
in the running game at DETAIL HI. The reach is three immediates of the code.

**What the core does.** The core does the same with two factors, each a single constant in
`firmware/track.h`:

- `FAR_SCALE` (default 2.0) scales the far plane, the reach, the window's points and the props' reach.
- `LOD_SCALE` (default 1.2, 0 = no level of detail) scales the distances of the levels.

At 1.0 and 1.0 the picture is the original's.

The camera cell's view box is kept only at `FAR_SCALE` 1.0 or less. There are two reasons:

- The box cuts the window to what the original showed, however large the factor is. Tripling the factor
  changed the cells walked by only 3 % on track 5.
- If the box is scaled with the factor, its edge moves by a cell as the camera crosses cells, and by a row
  with the window's length. That put the far buildings of track 5, 50,000 units away, in and out of the
  picture from frame to frame. This was found with the marks of `docs/development.md`, "Marks and the free
  camera".

The track image carries the map's cell table (`tools/make_track.py`, `tb_mmp.c`: key, placements, view box of
every cell), so that the walk takes the game's cells in the game's order.

**Comparison with the running original** (DOSBox, track 1, two runs: the grid and three or four places along
the first 32 seconds of the race; `tools/dosram.py walk`, `tools/perf/walkcheck.py`):

- At all nine cameras the window's rows and columns and the 1,191 placements in reach are the same, in the
  same order.
- The rule of the levels comes from the disassembly, and its constants from RAM.
- The pictures were compared with raw DOSBox screenshots only roughly. 32 % of the pixels agree, with or
  without the levels, because the original draws with camera angles in 2048ths of a turn and the core in
  floating point.

**Cost.** The study on the PC (`tools/perf/study_sweeps.sh`, `study.py`) races the six tracks with a robot,
all cars driving, 3,600 frames per row. The frame time comes from the model of `tools/perf/perf_model.py`,
which predicts the start frames of the simulation to 4.4 % rms, 3 % low.

| FAR_SCALE | LOD_SCALE | pieces | faces looked at | pixels | mean ms | worst ms |
|---|---|---|---|---|---|---|
| without the factors | none | 149 | 2,230 | 107,108 | 11.6 | 17.4 |
| 1.0 | none | 145 | 2,175 | 106,715 | 11.5 | 17.1 |
| 1.0 | 1.0 | 128 | 956 | 103,670 | 8.9 | 11.8 |
| 1.0 | 1.5 | 131 | 1,162 | 104,738 | 9.3 | 12.9 |
| 1.25 | none | 167 | 2,490 | 108,498 | 12.4 | 18.2 |
| 1.25 | 1.25 | 149 | 1,118 | 105,764 | 9.4 | 12.7 |
| 1.5 | none | 180 | 2,671 | 109,144 | 12.8 | 19.4 |
| 1.5 | 1.0 | 157 | 1,047 | 105,669 | 9.4 | 12.7 |
| 1.5 | 1.5 | 160 | 1,252 | 106,736 | 9.7 | 13.4 |

- A longer view alone costs 0.9 ms (1.25) to 1.3 ms (1.5) on average and takes the worst frame to 19.4 ms.
- The level of detail more than pays for it: -2.2 to -3.4 ms, and -47 to -61 % faces looked at.
- With `LOD_SCALE` 1.5, the placed blocks beyond 18,000 units take level 1 and those beyond 24,000 take
  level 2. The cars' blocks switch beyond 6,000 and 10,500. At the default 1.2 the four distances are
  14,400, 19,200, 4,800 and 8,400.
- At `LOD_SCALE` 1.5, a step of the levels as the car drives (150 units a frame) changes 22 pixels of the
  picture on average (0.03 %; at `LOD_SCALE` 1.0: 57), and 1,151 at the most in a frame (a tree's crown).

The race in the simulation was measured with `FAR_SCALE` 1.5 and `LOD_SCALE` 1.5 on tracks 1, 4, 5 and 6.
The gas was held from the green light, the first 105 frames have all ten cars, and the countdown's frame is
left out. The numbers come from the first line of `debug.txt`.

| Firmware | Mean frame | Longest frame | Frames over 20 ms |
|---|---|---|---|
| with the factors | 13.4 to 14.5 ms | 18.3 to 19.4 ms | none |
| without the factors (original distances, no level of detail) | 17.0 to 17.6 ms | 21.9 to 22.7 ms | two on each track |

The longest frame has 7 ms before the scene: the physics catching up after the first frame.

At `FAR_SCALE` 1.5 the far plane is 65,840 units from the eye, beyond 2^16. The setup unit and the clipper
handle this: the simulation's start frames have up to 16 polygons cut at it, and 0 pixels differ from the C
models.

## Race: player, opponents, cameras, effects

This section answers how the core reproduces a race: the player's car, the opponents, the cameras, the race
display and the special drawing routines.

### The player's car

This section answers how the player's car is driven, drawn and viewed.

The driving physics (engine, gearbox, brakes, steering, sliding) is OpenScreamer's transcription of the
game, line by line (`firmware/drive.c`). Ground contact over the four wheels, walls, markers and camera
follow OpenScreamer's glue code (`firmware/car.c`). The search for what lies under a point is in `track.c`,
which works directly on the blocks in the SDRAM.

The car itself is the twelve blocks of `AUTOnn.3DX`. They are drawn as parts through the geometry unit in the
game's order: tyres, hubs, then the body. Only the hubs rotate, and the front blocks steer. The geometry unit
selects a texture's image by a per-part phase (paint, brake light, wheel image; the track textures by the
animation clock).

The throttle is on B (down), the brake on Y (left), as in classic racing games, and the camera on X (up).
They are not on the D-pad, so that steering diagonally does not brake.

**Analog controller in the dock.** A controller with an analog part (type 3 in `IN_KEY` bits 31..28,
stick != 0) steers with the left stick, accelerates with the right one and brakes with the left trigger.
This is the way the game takes joystick axes:

- `0x24d40` turns each axis into a word of +-`0x3ff`. Below `0x66` nothing counts (`0x24edc`).
- The throttle is |word| / 1023 (`0x1611c`).
- A brake axis is the brake, |word| / 1023 (`0x16174`). A key instead ramps the brake to full with
  stiffness `0x52`.
- The steering wants -word / 1024 of the lock (`0x1633b`). It is there at once at the normal stiffness
  `0x50`, and smoothed when sliding or starting (`0x163bb`).

Stick and trigger count only outside the dead zone; otherwise the keys count. In the menus the stick acts
like the D-pad beyond half deflection (`in_keys`, `hw.h`). The core controls (`input.json`) know only keys;
analog values need no entry.

**Suspension** (from the EXE: `0x168a4`, drawn by `0x15c0c`). Each wheel swings by up to 18 units.

- The frame's speed change x 0.00175 pushes the front wheels one way and the rear wheels threefold the other
  way.
- The turn rate x speed^2 x 1.5e-9 pushes the left wheels against the right ones.
- Both pushes are divided by the frame time.
- A larger push restarts the oscillation, cos(4 pi t + pi), which decays (stiffness 61 lateral, 71
  longitudinal, off below 2).

From the deflections, as from wheel heights, the game derives three values: pitch `+0x4e0` = atan((VR - HR) /
wheelbase), roll `+0x4e8` = -atan((VL - VR) / track) with the game's negative track, and heave `+0x4c0` =
mean.

Only the four body blocks (`MV_mauto`, `fauto`, `rauto`, `fai`) pitch by `+0x4e0` x 2, roll by `+0x4e8` x 4
and sink by `+0x4c0`. The wheels stay on the road. The body therefore leans outward in a curve, and the nose
dives when braking. Bumper and cockpit (`0x1a2ac`) pitch by `+0x4e0`, roll by 3 x `+0x4e8` and rise by
`+0x4c0`.

The lateral push goes through the frame time twice, and the threshold 2 applies per frame. The suspension
(`suspension_step` in `car.c`) therefore runs on the game's frames of 1/30 s, with the stiffnesses of the
game's table at 30 frames/s. At 140 km/h and full lock the body rolls about 3 degrees. Under full braking it
pitches about 3 degrees. Opponents (`0x1df24`) roll by v x 0.02 and pitch by -|v x 0.02|, so the nose goes
down in every curve.

**Crash** (decision `0x16c90` in the EXE, sequence `0x18518` per OpenScreamer's notes). A wall hit is a
crash if speed x (angle to the road / 90 degrees) > 0.3 x top speed, but only in the open (`+0x7b0`, the kind
of the last marker, is 0). In a tunnel one only bounces off. The wall reaction happens anyway; in the game
the crash only switches off the wall sound. OpenScreamer does not check the tunnel and omits the wall
reaction on a crash. The core follows the EXE.

The crash sequence is: speed x 0.6, direction of the road, thrown up at 3000/s, 12 rad/s about the lateral
and longitudinal axes, gravity 6000, four landings, the crash view (row 5 of the views), and then the
previous view again.

**Views** (table `0x1da0`). The views are bumper, cockpit, near chase camera and far chase camera. X cycles
through them in this order. The race starts with the near chase camera (option `0x1d2a4`).

The cockpit is the bumper camera over a 3D view of 132 lines (`0x4c610(320, 132, 85)`, `0x19cbe`), with the
car's dashboard below. The dashboard is `COCKnnAL.PKC` from `GAME.DAT` (`COCKnnBL` for the second paint,
`0x13725`), split into blitter boxes when loaded. The numbers come from the `COCKnnL` section of `CARS.DTR`
(`0x1545c`: needle length, pivots, lamps). Speed and rev needles are three lines each in colour 255, smoothed
like the original (`0x2367c`, `0x2390c`). The gear lamp is a sprite. The rev image, the speed digits and the
gear digits are omitted there.

The start direction is that of the nearest road point in the map grid (`0x120c0`, called at `0x13ea1`). It is
not the direction of the spline handles, which is 1.6 degrees off.

**Track-side camera at the race end** (row 3 of the table, `0x1a620`). The game switches to it when the race
is finished (flag 8 at `0x18e14`), or when time is up and the car stands (0x10). Its positions are the nodes
of `MAPnRPL.SPL`. The loader `0x14fb4` stores the file at `0xf90c`; the core reads it at race set-up from
`DATA/MAPn.DAT` (`car_trackside_init`). The camera works as follows:

- It takes the node nearest to the car on the ground (whole units, distance truncated). A node exactly above
  the car does not count.
- From there it looks straight at the car, down by the elevation angle (`2 pi - acos(flat/distance)`, even
  if the node is lower), without tilt.
- If the car is farther than 1500 away, the camera moves along its line of sight until it is 1500 away (each
  coordinate truncated).
- Car and shadow are drawn. Overlay 0 omits these parts of the race display: the labels (`IMPLET_L`),
  countdown, lap, position, clocks and radar, rev image, speed and gear (`0x2367c`).
- The switch is a cut; there are no camera flights.

Setting `TRACKSIDE=1` for `tools/render.sh` shows this camera after the steps. With `HUD=1` the race display
is included.

### Race display (blitter)

This section answers how the race display (the HUD) is drawn.

The original draws it in software after the scene. It uses the font from `GAMEL.FNT` (colour x 16 + shading),
the sprites from `GAMEL.SPR`, the pictures `IMPLET_L` and `IMPRES_L` (.PKC) and needles drawn as lines, at the
places of `0x23400` and `0x2367c`.

In the core a blitter (`blit.v`) copies it pixel by pixel 1:1 into the framebuffer, without geometry, setup
and rasterizer. The commands have x, y, w, h (copy, text) or describe a line. They wait in a queue behind the
frame's parts and run as soon as the 3D units are done. `make_track.py` splits the pictures into boxes around
their pixels, so that the whole HUD costs under 1 % of a frame. A separate HUD buffer (64 KB per buffer) or a
mask for the rasterizer would gain too little. Traffic light, countdown, laps and times are as in the game
(OpenScreamer's `markers_step`, notes section 12).

**Background** (`MAPn.BCK`, a 512x256 PCX, drawn before the scene with `0x83a68`). It is a pure copy:

- The column `(2047 - heading) / 2 mod 512` is at the left edge (twice around the image per turn).
- The top line is at eye height + 22 - 256 + 128 * sin(pitch).
- Each line has 320 bytes at a 512-byte line pitch. A line that reaches past the image end reads into the
  next.
- Below the image, up to the view's end, the colour comes from TRACK_DATA (`color`, times four at `0x1337d`).

The tilted version `0x83b44` (four textured quads, rotated by the roll angle) is called nowhere in
`STARTL.EXE`. When the camera rolls in a banked curve, the background stays upright, as in the core. Eye
height is 130 (`0x4c610(320, 200, 130)`), and the projection centre in x is column 159 (`0x83431`), not 160.

The blitter copies the image (opaque, one pixel per clock) and fills the rest (four pixels per clock) while
the CPU selects the scene. The first part for the geometry unit waits until the blitter is done.

**Moving parts of the track** (from the EXE: `0x8ad14`, `0x8acf0`, `0x8ac58`; format per OpenScreamer's
notes S2-5b, valid for both games).

- A mesh can have several blocks. A block with a position set (+0x0C) is moved from there or, if the mesh
  has images (+0x10), rotated per image.
- 12 words per image are its whole matrix (the stored corner as a row times matrix, 16383 = 1, then the
  offset). The image is images x clock >> 16, and one turn of the animation clock takes 3.33 s.
- On track 1 the `H` set has such a mesh at the start, `V220`: the plinth with the video wall (block 0, one
  fixed image) and the helmet on it (block 1, 80 images). The other tracks have more, for example the
  windmills of track 3.
- The firmware splits such a part into one job per block, the farthest first (original: STARTL.EXE `0x8ac58`).
  The key of a block is the half of the distance of its centre, turned by the image, rounded; a block whose
  name starts with a capital Z gets 0x2000 more (16384 units nearer). The sails of track 3's windmills are
  `Z1vela`, so they stay in front of the tower at every turn, while the gondolas of track 4's wheel
  (`z1ruot`, lower case) pass in front of and behind its frame as they go round. Measured in DOSBox by reading
  the original's table of keys while it draws (the tower's key is exactly half its distance, the sails' key is
  8192 above half the distance of the turned centre).
- The images turn z-up into y-up, which is a mirroring. The game takes the other side as the back face for
  such blocks (`0x8acf0`: `jge` instead of `jl`), and so does the geometry unit (bit 3 in word 18 of a job,
  `docs/soc.md`).

### Opponents

This section answers how the opponents' cars are driven and how they interact with the player. It follows
OpenScreamer's notes, section 11. Its addresses come from the EXE.

- `MAPnOPP.SPL` has ten splines. Opponent k drives spline k, and the tenth is the player's line. The start
  nodes are the grid (two columns, the player last, position 10).
- The curve (`0x1d54c`, `0x1e7a4`) is a uniform quadratic B-spline over the nodes' handles.
- An opponent neither steers nor slides. It drives at `max_spd` x PACE of the last marker reached (+ `skill`
  while the player is ahead of it; `0x1ced0`), accelerates with `accel`, brakes with `brakes`, and advances
  along its curve (`0x1d2fc`).
- It stands on a point, the road under its centre, tilted by the normal of that face x 1.117 (`0x1cb9c`). It
  turns its nose in curves and leans the body (`0x1df24`, `sband`).
- Its markers and laps count like the player's (`0x1db84`).
- The placement (`0x20418`) swaps at most one opponent with the player per step, as soon as the player is
  over the line. Opponents among themselves never swap.
- The cars are the rows of `CARS.DTR` after the player's (`0x142e8`).
- The values come from the difficulty file (`'A'` + level, `0x13251`). The intro uses NORMAL (`MAP1B.DTR`,
  bonus time 40 s instead of 50 s at ROOKIE; the level the intro sets, `0x2777f`).
- An opponent is drawn only if its sphere (radius 1200) is in the field of view.

**Contacts.** Each car is two circles on the centre line (`0x136c8`, `0x142e8`). The body runs from 250 in
front of the front axle to 150 behind the rear axle, length L. Each circle is L/4 from the end and L/4 in
size.

Between the player and an opponent (only within 1500, `0x12378`; `0x17818`, constants 0.5, 0.0001, pi/2,
0.21, 0.7 checked in the EXE):

- The opponent yields by the overlap, unless that would push it off the road. Then the player yields.
- If the player runs into the opponent, the opponent is pushed on at half the player's speed and turned. The
  player is pushed back with 0.21 x speed, turned about the nose, and loses 5 % speed per step (a wall hit).
- If the opponent runs into the player, it keeps 70 %.

Between two opponents (`0x1cffc`, `0x1d1f0`), the faster falls to the mean and the later is pushed aside.

Overtaking (`0x1e404`): if the slower player is three radii in front of an opponent, the opponent shifts its
line just enough to get past.

An opponent pushed off the road by more than 3000 stops at once (`0x1cb9c`). If it loses more than 5000
units/s in one step, it crashes (`0x1d824`). Depending on the spot, it rolls over like the player or spins
twice on the road. The offset from bumps decays after 3 s.

### Routine 224 (smoke, shadows, radar) and routine 160 (reflections)

This section answers how the see-through effects (smoke, shadows, radar) and the reflections on the cars are
drawn.

**Routine 224.** The game's filler (`0xcd9d0`) turns each pixel into `SMG[texel x 256 + pixel]`, without
light or depth. This is the only SMG lookup in the program. The game uses it for:

- Smoke (player only, `0x19818`, `0x19970`): eight places, every 0.05 s at the rear wheels. The smoke
  travels with the car for about 0.67 s and is turned to the screen. Each cloud is offset sideways by
  (x & 7) * (T & 63) and backwards by (z & 7) * (T & 63). Here x and z are the car's integer position, which
  the game truncates toward 0 each step, and T is its 100 Hz counter.
- Shadows. The player's shadow (`0x18cbc`) is 20 units away from the camera, ground - 2. The opponents'
  shadows are drawn at `0x1c83c` and `0x1cd34`. Class 4 sorts 200 farther back.
- Headlight cones and lamp glow (track 4), and a helicopter (tracks 3 and 6).

The game never uses it for glass.

The rasterizer reads the pixel from the framebuffer and looks it up in a 16 KB table (16 M10K, `RS_SMG`).
Texel 0 leaves the pixel alone (row 0 swaps only colours of equal value, which saves 23-75 % of the pixels
for smoke). In blend mode the original's point rule applies, because otherwise the shared edges of a
polygon's triangles would be blended twice.

The radar (`0x23a68`) is a 72x72 window at (4, 124). It is darkened through row 63 of the table (x 0.75,
nearest palette colour, `0x25870`) as one blended quad after the scene. On it are:

- the walls (MMP field 3), as blitter lines in colour 255, rotated with the nose up about (40, 160);
- the opponents as sprite 5, and the player as sprite 4 at (38, 158);
- one pixel is 32768 / 24 units (the game's default, `0x1bc3e`).

Track 5 places no props: the nodes of `MAP5OST.SPL` do not lie above its road (unresolved, to be checked
against the original).

**Routine 160 (reflections on the cars).** The windows show a reflection that moves with travel and view.

The original computes it for each vertex of a block with flag 0x2000 (the body, 84-128 vertices with a
normal). On the x87 it computes the reflection r = 2 (e.n) n - e of the view direction e at the normal n
(`0xc2230`). It takes u = round(127 r.x) + 127 - offx, v = round(127 r.z) + 127 - offz as the texel, without
material size, wrapping at 256.

In the core the geometry unit does this after the block's vertices, in integer arithmetic
(`firmware/geom.h`):

- The normal goes from the float bits to Q14, with the matrix into view space (the same step as for the
  vertices).
- The vertex is normalised to 17 bits.
- 1/|p| comes from a table of 128 and one Newton step on the unit's three multipliers (18x18).
- This takes about 26 clocks per vertex.
- The u, v values go into a RAM of 512 (1 M10K), from which the routine-160 faces take their texels. The
  rasterizer lets them wrap at 256 (size bit 29).

Compared with the floating-point form, about 1 % of the values differ by one.

What the original computes per object stays on the CPU (`track.c`):

- The offsets offx, offz come from three accumulators per object (`0xc1fc0`: the distance moved since the
  last draw, rotated into the view, times the factor at `0xc2428`).
- The image. `0x1813c` sets it for the player's car by the kind of the last marker reached: car +0x7b0 = 0 /
  0x8000 / 0xff00 (kind 0 / 1 / 2) and the factor 0.002 or 0.03. `0x1607c` copies it to the object set
  (+0x3c). The image drawn is n x phase >> 16.

In track 1's tunnel (markers 3 and 4, kind 1), image 1 of the texture, the tunnel ceiling lights, replaces
the sky. Opponents keep image 0, because they do not call `0x1813c`.

Measured against the original (DOSBox, start grid, near chase camera; `tools/render.sh` with `CAM` and the
accumulators from its memory, `REFL`): of the 128 body vertices 95 are equal, and the rest differ by one. In
the rear window 65 of 125 pixels are equal (84 % equal to a neighbour). With the stored u, v the number of
equal pixels was 9.

The cost in the simulation: geometry unit +36,000 clocks, start frame 1,338,092 clocks = 18.0 ms.

## Sound

This section answers how the core mixes the game's sounds and how it plays the CD music and the FM music.

**Mixer in hardware** (after OpenScreamer's notes section 17 and the EXE).

- The game's driver runs at 11025 Hz and takes the stored sample without smoothing (index += pitch 16.16).
- `audio.v` does the same with 48 kHz at the output. Per output value it makes one pass through 32 voices
  (four clocks per voice, one data path, one DSP). For each voice it takes the byte at the position times
  volume left and right. The sum is limited to 16 bits. Then the position advances by pitch x 11025 / 48000
  (the firmware converts).
- The registers are in MLABs. Each voice has a line of 32 bytes in one M10K, fetched through the line port.
  The output clock is the template's (MCLK 12.288 MHz); everything else runs on `clk_74a`.
- The C model `firmware/mixer.c` computes the same value by value (`tools/sim.sh audio`, 60,000 random
  values).

**Volume.** The volume is the row of `SFXSBASE.DTR` times master. In the game the master is `0x6000`
(`0x1bc2f`). The core uses `0x4000` (-3.5 dB), so that the player's engine and the three opponents' together
never clip. The simulation's peak before this setting was 27,355 after 1 s of start grid.

The sounds are:

- Engine: one voice, pitch rpm x 14 + 32768 every step, `autogall` in the tunnel.
- Sliding, with the smoke's condition (`0x17d04`, start and stop at `0x180d0`).
- Wall (only without a crash, `0x16f0b`), crash (`dentone`), props (`hit_0`), and contact with an opponent
  (`crash`, at most 30 times per second).
- Sounds with a location, heard at the camera (`docs/soc.md`, "Sound"): the engines of the three nearest
  opponents (they rev up on the start grid) and the track's vehicles (trucks, buses, jet; the fourth column
  of `ANIMATIONS`). They have Doppler and are the only ones in stereo.
- The speaker, firmware only (`speech.c`, `docs/soc.md`, "Sound"): 58 recordings from
  `SPFXBASE/SPFXFULL.DTR`, 27 events with the game's triggers, times and queue, one voice in the centre,
  master `0x3000`. With `0x4000` the countdown clipped over engines and music.

**CD music** (OpenScreamer's notes section 17, MUSIC).

- The game plays song n as CD track n + 1, with n from `CD_TRACKS` in `MENU.DTR` (GEN.DAT). The track
  selection screen plays track 3, and tracks 1-6 play tracks 6, 4, 5, 7, 8, 2. When the track ends, the drive
  restarts it.
- The core reads GOG's CD image from the SD card unchanged. `SCREAM.DAT` (cue sheet, slot 21) says where the
  tracks begin in `SCREAM.gog` (slot 22, raw, 2352 bytes per sector).
- The mixer takes one stream of 16-bit stereo at 44.1 kHz from a 256 KB ring in the SDRAM and interpolates it
  linearly to 48 kHz (`docs/soc.md`, "Sound").
- `firmware/music.c` fills the ring completely at start (224 KB, about 170 ms on the Pocket). After that it
  reads once per frame with a 32 KB read command. The CPU only starts the command; the Pocket does the rest
  through the bridge while the frame is drawn.
- The volume is `0x4000`. The game's CD master is `0x6000` (0.75); as with the effects, the core uses two
  thirds of it. This keeps the ratio of music to effects as in the game and the sum within 16 bits. With
  `0x6000`, the music and the start-grid engines together clipped 267 values in one second of simulation.

**FM music** (OpenScreamer's notes sections 17 and 32; `firmware/fm.c`, `core/src/fpga/core/opl.v`).

- When the machine has no CD, the original plays `MUSICn.HMP` (the same n) instead, through the sound card's
  FM chip, the OPL2 of the AdLib.
- The core's chip is JTOPL (Jose Tejada, GPL-3.0, `core/src/fpga/core/jtopl/`, with six corrections that
  make it match the real chip). A fractional divider from `clk_74a` clocks it at 3.579545 MHz, so that it
  makes 49,716 samples a second and the pitch is true (`sim/opl` measures a 875.4 Hz note to within 0.05 %).
- The chip's output is mono. It is interpolated linearly to 48 kHz and starts the mixer's sums, so effects,
  CD music and FM music add up before the limiter. Without FM the mixer output is unchanged, bit for bit
  (`tools/sim.sh audio`).
- The firmware is HMI's player and driver as the game has them. It steps the songs' tracks 120 times a
  second and sends their messages to the driver (nine voices, instruments from the banks, levels from
  channel volume and velocity, pitch bend, drums on channel 10, every rule and its address in the comments of
  `fm.c`). A song that has ended is started again.
- `tools/fmcheck.sh` runs the firmware on the PC against openscreamer's driver for all seven songs: the
  same 36,000 to 100,000 register writes each, tick by tick.
- The CPU runs once a frame, but notes are due to 8 ms. Each write therefore goes into a queue in hardware
  with the time at which the chip is to get it, about 60 ms ahead. The queue (256 writes, one M10K) releases
  the writes in time and spaces them 36 us apart, as the original's driver did.
- The game's music master (the CD player's MU) and the pause's fade reach the chip as the driver's note
  levels, as in the original. The CD master reaches only the CD.
- The source is chosen by the Pocket menu's Music Source: Auto (the CD image if its tracks are there, else FM
  if `MUSIC.DAT` is) or FM Synth (always FM). The game's own MUSIC option switches both off.
- A change of Music Source takes effect at once. The song that plays goes on from its start on the other
  source (a song paused meanwhile resumes that way).
- The CD player of the pause lists tracks 2 to 8 for the seven songs. Leaving it with another track plays
  that song from its start.
- Level: the gain `FM_GAIN` is 50 of 32 (1.56 times what the chip makes, `FM_GAIN_DEFAULT` in `fm.c`).
  Songs rendered through the real chip (`tools/sim.sh opl play`) have an RMS of 1,900 to 3,000 against 2,400
  to 2,900 for the CD tracks as the mixer plays them, and peaks of 11,000 and 14,900 against 16,400. The
  level has not been tuned by ear.

## Intro

This section answers what the core shows when it starts and when the main menu stays idle.

The intro is as in the original (`0x276a0`, OpenScreamer `docs/NOTES.md` sections 26 and 30). The core has
it in `firmware/intro.c` and `run_intro`/`intro` in `main.c`.

The original's main program (`0x10020`) alternates intro and menu. The intro runs at start before the main
menu, and again when the main menu (page 0, no page open) has stood 30 s without a key (`0x288bf` counts,
`0x2802f` with `0x100edc` = 30.0; `MENU_IDLE`).

- **The race:** a real race without a player (`0x27710`: mode `0x100`, flag `0x2001`: started at once,
  "attract"; difficulty NORMAL, option word 0), on track `CHOICE.DAT` +0x434 + 1. The counter advances after
  each intro and wraps to 0 after 5 (`0x27f06`). The race is set up like a normal one, but directly
  (`0x128dc`, no loading picture: the screen stays black), with the menu music.
  - At the core's start only, the track's loading picture is shown. This deviates from the original, so that
    start-up does not stay black for long (`first_picture` in `main.c`). The Pocket shows the core's video
    only about a second after the CPU starts (measured with a text that the boot loader held for 5 s), which
    is when this picture is up; a text drawn earlier is not seen. Right after `CHOICE.DAT`, the
    program loads `LOCAL.DAT` and from `GRAF.DAT` only the table of contents, `MENUL.SPR`, `MENUL.FNT` and
    `LOADnL.PCX` (about 100 KB). It shows the picture as before a race, loads the rest of the menus behind
    it, builds the track and loads the sounds. Then it fades out as before a race. The log gives the time
    since the start (`first picture: ... ms after the core's start`).
  - In each frame only the opponents, props, moving scenery and the camera run (`0x2780c`, the five routines
    per opponent). The player's car does not exist (`car_intro`: taken out of the world, its engine silent,
    `car_step` does not run).
  - The picture fades in like a race in half a second (`0x21e50`). The cars start as soon as it is fully
    there.
- **The camera** (row 4 of the view table, routine 8: `0x1aeb4`): each opponent has its own (`0x1a920`, set
  +0x4f8).
  - The post is the one from `MAPnCAM.SPL` (14-29 nodes) nearest to the car on the ground. The camera aims
    at the car like the camera at the track side.
  - `|height| % 10` of the post is a code: bit 1 always, bit 2 while the car moves away, bit 4 while it
    approaches. The camera moves up to 1500 from the car (at a new post the distance counts from the car's
    position one step earlier). If this changes at the same post, the camera flies over (`0x19ad8`,
    `0x19ef0`: the distance decays with damp[90] or 0x19 per step). A new post is a cut.
  - One car is shown (`0x110a14`, initially the first). Every 10 s (`0x27e64`, 1000 ticks) `0x19d82`
    prepares the next car at a different post. It takes over at the shown car's next post change (`0x1aef3`).
  - The sounds (opponents' engines, scenery) are heard from this camera.
- **Logos and credits** (`0x279d4`):
  - The text blocks come from `ATTR.TXT` (`LOCAL.DAT`, separated by `#`, 19 of them), in font 0 from
    `ATTRL.FNT`, colour 2, line i at y = 20*i.
  - Each block glides from +350 to the centre (stiffness 0x5a, whole pixels, `0x27b64`). After 4 s the lines
    go out to -400, and when a block is beyond -360 the next block comes.
  - The logos come from `LOGOL.SPR` (`0x279fc`). At the first block, sprite 0 (y 4) is shown. At the second,
    sprite 1 (y 28) glides in from -320 (0x50) and out with the block. From the third block, sprite 2
    (y 4) comes in with 0x5a and goes out to -340 at the last block (18).
  - All of it is in the track's palette.
- **End:** B, A, START or SELECT (`0x211d0`: ENTER, ESC, space, joystick button) or the end of the credits
  (state 3) fade picture and sound out in one second (`0x27d28`). Then the main menu follows, each page on
  its first row (`0x27eb4`).

## Main menu and options

This section answers how the main menu and the options page are built, and how the options take effect.

The menu is as in the original (its screen 0; the pages are tables of 16-byte entries `[row, id, value,
maximum]` behind `0x2de4`; OpenScreamer `docs/NOTES.md` sections 18 and 26), in `firmware/menu.c`.

- Page 0: NORMAL GAME, CHAMPIONSHIP, SPECIAL MODES, NETWORK GAME, OPTIONS.
- Page 1: DETAIL (LOW/MID/HI, initially HI), difficulty (ROOKIE/NORMAL/PRO, initially NORMAL), LAP NUMBER
  (3/5/10/25, initially 3), SPEECH and MUSIC (initially ON), VIEW RECORDS.

The words come from `MENU.TXT` (rows 0-36; an option shows its row + its value).

**The words are cubes in the menu scene, as in the original** (`0x2b33c`).

- Each letter is a cube `ALETTER.3DX` from `MENU.DAT` (516 units). Its texture (material 1 of `MENU.MLB`) has
  76 images, one per character.
- The cube's set has at +0x20 `0x35e * (character - 32) + 0x226`. A quarter of that is the geometry unit's
  texture phase. The front shows the bright upper half of the image, the back the dark lower one.
- Row i of n lies on row `8 - n + 2i`, y = `0x820 - 0x180 * row`. The letters are 640 apart, centred
  (`0x2af30`), spaces too.
- The camera stands at (0, 0, 4100) looking toward -z, with the lens as in the race (`0x29ea0`).
- Each step (`0x2b8b8`) the chosen row turns forward at 4 pi rad/s and the others backward. Each cube bobs in
  z by `150 * sin(2 pi * fx + pi * fy + t * k)` (t at 3 pi/s, back at 8 pi; chosen row fx = 0.5, k = 1/4).
- On entering, the letters fly in within a second. They start and swing from one of the eight rows of
  `0x2218`, in turn, the start turned by 0.09 rad per letter.
- When the page or an option changes, all rows turn away. When the chosen row is past the quarter turn, the
  new page stands there and its row finishes turning (`0x2b380`).
- The cubes go through the geometry unit like the models of the track and car selection, the farthest first.
- Behind them is `MENU1L.PCX` with its palette. Over it are the two SCREAMER logos (sprites 15 and 14 of
  `MENUL.SPR`, at y 0 and 16), gliding in from the left at 350 and 700 pixels/s (`0x2df38`).

**Keys** (`0x28080`, `0x28ac4`).

- Up/down moves from row to row (no wrap). Left/right changes an option. B or START does too (the original's
  ENTER counts up, `0x297d4`; only when all letters have landed).
- B or START on OPTIONS opens the options. A or SELECT leads back.
- NORMAL GAME: the camera moves back to z 20,000 (factor 1 -> 0 at 0.6/s, fourth power) and rolls from -4 pi
  at pi/s. The letters stand. The picture fades out at 2/s (`0x293e8`). Then follow ENTER YOUR NAME (below)
  and the track selection.
- NETWORK GAME shows `NETWORK NOT FOUND` like the original without a network (row 55, font 0, colour 2,
  y 190, blinking).
- SPECIAL MODES, CHAMPIONSHIP and VIEW RECORDS: see the sections below.
- The pages fade in at 0.6/s (`0x29ea0`): after the intro, after A or SELECT on the name, on a track's
  records and after every race (`0x2a114` -> `0x29ea0`; back from the name or the records to the page one was
  on, `0x28a7c`).

**ENTER YOUR NAME** (screen 4 of the original, before every game: all rows that start a game go there via
`0x293e8(4)`, `0x28b80` ... `0x28cba`; A or SELECT in the track selection leads back to the name, `0x281c2`).
The name appears in the records and is kept in `CHOICE.DAT` (+0x08, initially "GRA"). The original needs no
keyboard for it, and the core does the same (`0x2c320`, `0x2cd74`, `0x2b54c`, `0x2e4f8`):

- The 26 letters are cubes in a ring (radius 3500, every 2 pi/26). Each falls 0.04 s after the previous one
  from 2560 (odd 3060) to 160 and bounces back to 0.4 of the height (falling p to 1 at 2.5/s, rising at 2/s,
  height over p^2).
- The chosen letter's cube tilts at 4 pi rad/s to -36 degrees to the camera, the others to 126 degrees.
- The camera stands outside the ring (radius 4500, height 1800, 36 degrees down). It moves to the letter in
  1/8 s linearly (`0x29530`, the heading the shorter way).
- Behind the ring is `MENU2L.PCX`. Over it are the title (row 46, y 2, font 3), the name at (0x83, 0x4c) in
  font 2 with fixed width, and a bar 12 lower under the letter being set (21 pixels per letter).
- Keys (`0x28080`): down/up selects the next/previous letter (`smenu6`). Left/right turns the alphabet by one
  and sets its letter (silent; only when the camera is almost there, under 0.1). There is ONE alphabet for
  all three letters, as in the original (`0x1d238`). B or START continues, and A or SELECT goes back to the
  page (both silent).

**The options take effect** (stored in `CHOICE.DAT`, see "Championship").

- LAP NUMBER is the race's lap count (`car_laps` in `car.c`; the original takes `0x21e0` + 1: 3, 5, 10, 25).
  The end of the race and the "1/3" of the display follow it.
- The difficulty selects the file `MAPnA.DTR`, `MAPnB.DTR` or `MAPnC.DTR` (`0x13251`;
  `track_build_difficulty`), from which the track build reads opponents and `TRACK_DATA`.
- SPEECH OFF: the speaker says nothing (`speech_on`). MUSIC OFF: no music, CD or FM (`music_on`). Both take
  effect at once, also in the menu. The original takes both over only when the next race loads (`0x2a264`).
- DETAIL is only displayed: the core always draws the full H set.

## Special modes

This section answers how the special modes (time attack, cone carnage, slalom) differ from a normal race.

SPECIAL MODES opens page 4 of the original (`0x28eab`): TIME ATTACK, CONE CARNAGE, SLALOM (`MENU.TXT` rows
28-30, ids 28-30). A or SELECT leads back.

The choice sets the original's mode word (`0x1d25c`): 1 normal, 2 championship, 4 time attack, `0x10` network,
`0x20` cone carnage, `0x40` slalom. In the core this is `car_mode` in `car.c`, set from `menu_mode()`. Then,
as with NORMAL GAME, the name, track and car selection follow (`0x28bb0`). Page 3 (NEW/RESUME, ids 24/25)
belongs to time attack, page 5 (ids 26/27) to the championship.

In all three modes (mask `0x64`):

- The player is alone on the track (`0x1311c`, `0x1309c`, `0x12e14` create no opponents).
- The race has one lap (`0x2a216`).
- The start is the start place of the last opponent (`0x13ded`: spline opponent count - 1 instead of its own,
  direction that of the road there).
- There is no lap best time to beat (`0x2a264`: 0).
- After the finish there is no replay (only in the normal game, `0x1fe45`). The game goes straight to PLAY
  AGAIN?.
- The speaker says no "lead" there (mask `0x13`, `0x24308`). In cone carnage and slalom it says no "hurry"
  either (mask `0x17`, `0x242dc`).
- The original enters records only for the normal game and the championship (`0x2e5e4`, see "Records").

**CONE CARNAGE** (`0x20`).

- The obstacles come from `MAPnCNG.SPL` instead of `MAPnOST.SPL` (`0x14fb4`; track 1: 36 cones, track 5: 99).
- Finish line and checkpoints give the fourth number of `TRACK_DATA`, `carnage`, instead of the bonus time
  (`0x195fc`; track 1 NORMAL 15 s instead of 40). That is far too little for a lap. Each step in which more
  cones are knocked down than before gives 1 s (`0x18b50`; each cone counts once, `+0x3c`, `0x126a4`).
- At the finish, `YOU MADE IT!` is shown (`GAME.TXT` 14, y 90). With all cones, `PERFECT!` is shown too (15,
  y 120; `0x23fbc`, `0x24204`). Otherwise, as always, `TIME OVER` is shown.
- The speaker says "cones" (event 19) once after the line, "all" (26) as soon as all are knocked down
  (`0x24570`, `0x24386`), and "win" at the finish.
- `MODE=0x20` for `tools/render.sh` draws and drives (`REPLAYCHECK`) the mode on the PC.

**SLALOM** (`0x40`).

- The obstacles come from `MAPnSLA.SPL` (gate poles; each two are a road width apart).
- The gates come from `MAPnSLA.MRK`: the count, then 32 bytes per gate with radius 1024, `+0x10` seconds 2-4,
  `+0x14` x and `+0x1c` y, the middle between two poles (track 1: 14 gates).
- Finish line and checkpoints give only `carnage`, as in cone carnage.
- Each step (`0x18b98`) finds the first gate whose circle the car stands in (its whole position). The first
  time, the gate gives its seconds and the door sound (sample 9, `door`).
- At the finish, `YOU MADE IT!` is shown. `PERFECT!` comes only with all gates and without touching a pole
  (`0x23f68`, `0x241ac`), and so does "all".
- `MODE=0x40` for `tools/render.sh` draws the mode on the PC.

**TIME ATTACK** (`4`, `firmware/ghost.c`).

- TIME ATTACK opens page 3 (`0x28e0d`). A or SELECT leads back to page 4.
- NEW drives alone (`0x28c0f`: `0x1d260` = 0). RESUME drives against the track's ghost (`0x28c3c`; without
  one, alone, `0x2a1df`). The bonus time is the normal one, and "hurry" is said.

The original records the drive (`0x231d0`, one pose per frame) and plays the ghost as a second car that
nothing collides with (`0x12e14` -> `0x1ba3c`, `0x14d28`; `0x23018`, `0x23308`):

- The recorded car is on the start place it drove from, and the player on the other (`0x13ded`: recording 0
  or 1 + opponent count - 1, `0x1bb33` switches).
- The ghost's lap is the one to beat (`0x1d268`, the third clock; below it "new lap record").
- The player is second while the ghost is ahead (`0x20418`).
- At the finish (`0x23ef0`), a slower or equal time gives `TRY AGAIN` (`GAME.TXT` 13) and "lose". Otherwise
  (or without a ghost) it gives `YOU MADE IT!` and "win".
- The drive then becomes the new ghost after the race (`0x1fd64` -> `0x1b964`, `REPLAYS\MAPn.ATK`), and PLAY
  AGAIN drives against it.

The core does it this way:

- It records one pose per 4 steps (12.5/s) from the set-up and plays them back in the same step. The
  position goes through the original's cubic over four poses (`0x22724`), and the angles are interpolated
  between two poses. The ghost has no shadow and no engine.
- Per track the best ghost is stored in `ATTACK.DAT` (`ghost_track()` points into this slot's memory, see
  "Championship"), in the save file's format.
- `MODE=4 GHOST=1 REPLAYCHECK=9000 tools/render.sh` drives two robot races, the second against the first's
  ghost (track 1: the ghost within one unit of the first drive's position).

The ghost file has this format:

| Part | Content |
|---|---|
| Header, 16 bytes | "ATK1", track, car, count, lap in 1/50 s, start place |
| Each pose, 20 bytes | x, y, height as int32, heading, pitch, roll as int16 with 65536 = 2 pi, race progress x 64 and the brake light bits |

There are at most 3000 poses, so the file has at most 60,016 bytes.

## Records

This section answers where the best times are kept and how they are entered and shown.

Per track the game keeps five best laps and five best totals with names (`CHOICE.DAT` +0x6c and +0x24c, 0x50
bytes each: five times, five names, five flags, for the totals the game mode). Initially they are 180 s and
900 s under "GRA". The code is `choice_record` in `champ.c`.

- **Entering** (`0x2e5e4`, after every race of the normal game and the championship: `0x2a1a8`, `0x1febf`
  before PLAY AGAIN, `0x28f42`; not if time ran out or the race was left, flag `0x1020`):
  - The race's best lap is always entered (flag 1). The total is entered only at three laps (`0x1d2ac` = 2;
    flag 0, game mode).
  - Each is entered in front of the first time it does not beat, and the others move one place lower, under
    the name.
  - In the original the flag of a total is the number of a stored replay (`REPLAYS\REPLAY<track><n>.RPL`, an
    "R" after the time, ENTER plays it). The core stores none. It shows the "R" only for a `CHOICE.DAT` from
    the DOS game.
- **The lap to beat** (`0x1d268`, the third clock of the race display, `car_lap_record`):
  - Before the race it is the track's best lap (`0x2a264`; in the special modes 0, in time attack the
    ghost's).
  - A lap below it replaces it ("new lap record").
  - At the finish it is the race's best lap (`0x20eb8`). PLAY AGAIN starts with it.
- **VIEW RECORDS** (last row of the options, id 21) opens page 6 of the original (`0x28cd8`).
  - The six tracks are cube words (`MENU.TXT` rows 31-36, id 31, the track as maximum). A or SELECT goes back
    to the options.
  - A track leaves the page like a game (`0x28d0b`: `smenu4`, the camera moves back, fade out). Then comes
    its screen 6 (`0x2c844`, `0x2d348`): `LOADnL.PCX`, the box (48, 36, 226x160) darkened twice, the track
    name (y 2, font 3), `BEST TOTAL TIME` (y 0x28) and `BEST LAP TIME` (y 0x78, font 0). Below each are five
    lines "n. NAM mm.ss.hh" (font 1, fixed width, centred, 10 apart from y 0x3c and 0x8c). Everything is in
    colour 0, the chosen total in colour 1.
  - Up/down chooses a total (`smenu5`). A or SELECT goes back to the track page.
  - The original has no paging to the next track. `STARTL.EXE` does not read `SCORE.TXT` (`LOCAL.DAT`).

## Championship

This section answers how the championship works: its menu, its stages, its races, the standing and the saved
state.

The championship is as in the original (its mode 2; OpenScreamer `docs/NOTES.md` section 21, checked in the
EXE). The rules are in `firmware/champ.c`, screen 5 in `menu.c`, and the flow in `main.c` (`championship`).

- **NEW / RESUME:** CHAMPIONSHIP opens page 5 of the original (`MENU.TXT` rows 26/27, `0x28b3a`, A or SELECT
  back). Both leave the menu like NORMAL GAME. NEW (`0x28c6b`, `0x2c698`) starts at stage 0, with all places
  empty. RESUME (`0x28c9e`) goes back to the remembered stage.
- **Flow:** The stages are a table of 53 stages at `0x22b8` (44 bytes each: kind, two values, text block,
  next stage, stage on failure, picture `champN`, race, font). There are four leagues (ROOKIE, AMATEUR, PRO,
  BULLET) of 13 stages. Before each of the six races of a league a text picture is shown (WELCOME TO ...
  before the first, otherwise PREPARE FOR THE NEXT RACE!). After each race comes the standing, and after the
  sixth race CONGRATULATIONS!. Stage 51 is everything won, and stage 52 is LOSER!!. `champ_at` rebuilds this
  (regular, checked against the dump).
- **Car:** ENTER on the text picture opens the car selection (`0x291ae`), before *every* race, with the last
  chosen car. A or SELECT there leads back to the text picture (`0x28118`). The car is not fixed for the
  season in the original (OpenScreamer chooses it once at NEW).
- **Race:** The races are tracks 1-6 in order, 3 laps each, with the opponents from the league's file
  `MAPnA`-`MAPnD.DTR` (`0x28ef6`: `0x2a264(2, track, league)`). In the Bullet league (D: one opponent) the
  opponent drives the Bullet (`0x14373`: difficulty via PRO -> car 12, in the paint of its row; `opp_bullet`).
  A finished race continues by itself after the 5 s of the end picture, without replay and without PLAY
  AGAIN? (`0x1fd60`, `0x1fe86`). After time up or a left race (SELECT), PLAY AGAIN? is asked, and YES drives
  the race again.
- **Result** (`0x29acc`): A left race does not count (the stage stays). Otherwise position, total time and
  points (9/7/5 for place 1/2/3, `0x21ec`) go into its row, and the stage goes to the standing. Time up
  remembers 1, unreachable 2. Unreachable means that even wins in the remaining races no longer reach the
  league's score (42/46/50/54).
- **Standing** (`0x2ce58`):
  - `CHAMPnL.PCX` (n the league) is the picture. The box (16, 36, 288x136) is darkened twice (`0x25cc8` with
    the table for the picture's palette, 0.75, `0x25870`).
  - The league name is at y 2 (font 2, colour 2). The header TRACK/POS/TIME/SCORE (y 48, x 28/88/170/246,
    font 0, colour 15) follows.
  - There is one row per race, 14 high from y 62: `-n-`, position, `mm.ss.hh`, points. Dashes stand for a
    race not yet driven, and `10`, `00.00.00`, `0` for one with time up.
  - TOTAL SCORE is 4 lower (right-aligned to x 276), and QUALIFY:n is 24 lower, centred.
  - ENTER (`0x292c8`) continues only among the first three (Bullet league: as winner), in time and after the
    sixth race with the league's score. Otherwise LOSER!! follows, and RESUME drives the lost race again.
- **Text pictures:**
  - The lines of the block from `CHAMP.TXT` (blocks separated by lines with `#`) are centred from y 32, one
    font height apart, in font 3 (league won and end: 2), colour 2.
  - On all pictures PRESS ENTER TO CONTINUE blinks (row 56, y 190).
  - B or START is ENTER, and so is 10 s without a key (`0x2d301`). A or SELECT works only on the text picture
    before a race and leads back to the main menu (`0x2829d`).
  - There is no sound. The picture fades in at 1.5/s, and it is left with a cut to black like the car
    selection.
- **After the last race:**
  - After a league won, CONGRATULATIONS! follows, then WELCOME TO the next league (one more track unlocked,
    `0x290a5`, see "Unlocking").
  - After the Bullet league comes YOU'RE THE GREATEST DRIVER EVER ... NOW THE BULLET IS YOURS ! (`0x29141`:
    Bullet, the backward tracks and AFTERBURNER unlocked), and back to the main menu. LOSER!! also leads back
    to the main menu.
- **The saved state** is in the format of the original's settings file, `CONFIG\CHOICE.DAT`
  (`struct choice_dat` in `champ.h`: 0x460 bytes, read to `0x203c0` by `0x1bbb8`, written by `0x1be5c` before
  and after every race and on exit). It holds:
  - the car, the track, the name and the options;
  - from +0x18 the championship (stage, stage for RESUME, score, per race time, position, points);
  - from +0x6c the records (per track five best laps and totals);
  - from +0x42c the unlocks.

  The core holds exactly this file as nonvolatile data slot `CHOICE.DAT` (id 23) in the SDRAM. It is loaded
  at start; without a file the original's defaults apply (`0x1bbb8`). Every change (stage, result, options,
  last chosen track and car) is written there by the CPU at once, and the Pocket saves it on exit like
  `debug.txt` (`docs/soc.md`, "Log to the SD card"). A `CHOICE.DAT` from the DOS game works too.

  For time attack, the original's `REPLAYS\MAPn.ATK` has a 36-byte header with the best time at +0x1c, then
  per drawn frame a 40-byte record with position, attitude and clock (`0x1b964`, `0x1ba3c`, `0x231d0`). The
  slot `ATTACK.DAT` (id 24) holds the kept run per track (`struct ghost_file`, 60,016 bytes, 360,096 in all).
  `ghost_track(n)` points directly into its memory, so the ghost survives power-off like `CHOICE.DAT`.

## Track and car selection

This section answers how the track selection and the car selection screens work, and what is unlocked when.

The track selection is as in the original (its screen 3: `0x2e384`, `0x2c078`, `0x2cb88`, `0x29530`;
OpenScreamer `docs/NOTES.md` sections 18 and 26), in its simplest form in `firmware/menu.c`.

- The six models `AMAPn.3DX` from `MENU.DAT` stand in a row, 40,000 units apart, and rotate (pi/4 rad/s).
- The camera looks from (-40,000*n, 4800, 13,000) 36 degrees down. On left/right it glides to the neighbour
  (factor 1 -> 0 at 0.95/s, path with the fourth power; the next press counts only below 0.8).
- The models go through the geometry unit like objects in the race, with `MENU.MLB` as material file (all
  their faces are single-colour, routine 128). Each block is at its place from the position set, the
  farthest first.
- Behind them is the picture `MENU3L.PCX` from `GRAF.DAT` (its palette is the screen's). Over it are the
  title (`MENU.TXT` row 45, font 3), the track name (`TRACKDAT.TXT`, font 2, 2 font heights above the bottom
  edge) and the arrows from `MENUL.FNT` and `MENUL.SPR` (sprites 0 and 1 at x 8 and 238, y 100, sprites
  2 and 3 light up while the camera glides in their direction and its factor is above 0.6, `0x2e2f0`). All of
  it goes through the blitter.
- B or START takes the track.

**Unlocking** as in the original (`CHOICE.DAT` +0x42c Bullet, +0x454 level, +0x458 the level already shown; a
new game has all three 0).

- Tracks up to level + 3 are selectable (`0x287a8`: right only while the track is below `0x20814` + 2), that
  is three at the start.
- Each championship league won sets the level to 1, 2, 3 (`0x290e8`, tracks 4 to 6). Everything won sets it
  to 9 (`0x29141`; the original also unlocks the backward tracks 7 to 12, which the core does not have).
- The cars are rows 0 to 11, and the Bullet (row 12) is available only once it is won (`0x28726`:
  `0x207ec` + 11). AFTERBURNER as highest difficulty likewise (+0x45c).
- After the last selectable track the right arrow is missing (`0x2e32a`, `0x2e4a2`), so locked models never
  come into view.
- While the level is above the one shown, NEW TRACK AVAILABLE blinks in the track screen (`MENU.TXT` row 53,
  y 0x20, font 1, colour 2, 32 of 64 ticks, `0x2e424`). Leaving the screen counts as seen (`0x28201`,
  `0x28260`).
- The original's cheats are words from `MENU.DTR`, typed on the keyboard (`0x288dc`): TAZOR Bullet, VTELO
  level 3, INVER level 9, ABURN AFTERBURNER, MONTY/JOINT obstacles, CLOCK clock stands, MIRRO picture mirrored
  and steering swapped, UPDOW picture upside down. They do not exist in the core, because the Pocket has no
  keyboard.
- `MENU=1 LEVEL=n` and `MENU=2
  BULLET=1` for `tools/render.sh` draw these screens on the PC with the given unlock state.

**The car screen** follows the track selection, as in the original (its screen 2: `0x2bdcc`, `0x2ca10`,
`0x2e08c`).

- For each row of `CARS.DTR` (GEN.DAT) a car stands there: its model `AAUTOnn.3DX` from `MENU.DAT` in its
  paint (texture phase `0x6000`: not braking, slow), 4000 units apart at z 2900, rotating at pi/2 rad/s.
- The camera glides as in the track selection. It looks from (-4000*n, 500, 4100), 22.5 degrees down.
- The cars are drawn by `car_mesh_draw` (`car.c`) like the race draws a car, with the engine's block order.
  Their textures are spread over all of `MENU.MLB`, which is why the core loads the file whole.
- Behind them is `MENU4L.PCX` with its palette. Over it are `CAR SELECTION` (row 44), the name (font 0, 3 font
  heights deep), the gearbox (row 37/38, 5 heights above the edge), `MAX SPEED` with "%d KMH", and `GRIP` and
  `ACCELERATION` with their bars (sprites 4-8, 9-13). All of it comes from `CARDAT.TXT`.
- Each car exists twice, automatic in the first paint and manual in the second, like the rows of `CARS.DTR`.
  Rows 0 to 11 are selectable, the Bullet only after winning the championship (see "Unlocking").
- The side views 16-28 of `MENUL.SPR` are not drawn by `0x2e08c`.
- B or START takes the car, and A or SELECT leads back to the track selection.
- The row is the player's car (model, driving data, gearbox, dashboard in the cockpit). The opponents take
  the remaining rows in order (`0x142e8`).

**The loading picture.** After the car selection the core loads, unpacks and builds the track. This takes a
few seconds; for track 1 on the Pocket it is 1 s of loading, 3.2 s of building, and 0.6 s for effects,
scenery and dashboard.

Meanwhile the loading picture is shown, as in the original (`0x2a264`, before every race except after PLAY
AGAIN): `LOADnL.PCX` from `GRAF.DAT` with its own palette. Over it is `LOADING` (`MENU.TXT` row 47), centred
in row 0x50, font 3, colour 0 (`0x2e598`). It is shown at once with its palette. When everything is built, it
fades out in half a second (see "Fades"; `menu_loading` in `menu.c`, `loading` in `main.c`; `MENU=3` for
`tools/render.sh`).

The menu music stops when the picture appears: the CPU builds the track in one go and reads from the data
slots meanwhile, so nobody fills the music ring in that time.

## Pause, replay, play again

This section answers how the pause screen, the replay and the PLAY AGAIN? question work.

**Pause:** START in the race, while it runs. The original has the CD player for it (P, `0x211d0` ->
`0x21328`):

- The last frame is darkened twice (`0x25cc8`, the same table as the radar, 0.75), and the race stands.
- Over it are the car radio `CDFRONTL.PKC` from `GAME.DAT`, the CD's audio tracks (2-8, font 5, colour 9,
  from (64, 78) at spacing 13, the chosen one blinks) and the cursor on PLAY (sprite 7 at (59, 97)).
- Effects and music fade from 0x6000 at 16384/s (1.5 s). Then the CD stops (`0x26458`, MSCDEX's STOP AUDIO: a
  pause).
- Left/right (`0x214e1`) moves the cursor over seven keys (table `0x1f1c`: x, sprite, pressed). ENTER
  (`0x21551`) presses the key under the cursor.

| Key | x | Effect of ENTER |
|---|---|---|
| PLAY | 59 | leaves the player |
| EJECT | 83 | shows GAME.TXT row 17 PLEASE EJECT DISK until the cursor moves on (DOSBox's image never ejects) |
| back / forward | 113, 137 | choose the track |
| - and + | 167, sprites 57/58 | change the displayed volume by 48000/s times the frame time (in DOSBox 480 per press) |
| the last | 217 | switches CD -> FX -> MU (`0x1f7c`: master volumes `0x1d2bc`, `0x1d2b8`, `0x1d2c0`; stored in `CHOICE.DAT` +0x448, +0x444, +0x44c) |

- On these three volumes `0x23dac` shows the name and value/1024 bars (colours 22, 214, 208, ten each).
- A pressed key looks pressed for 0.5 s. There are no sounds, and the music never plays in the pause.
- ESC leaves likewise. On leaving (`0x21982`) the volumes apply. The chosen track plays from the start
  (`0x2636c`, repeated) if back/forward changed it. Otherwise the song continues where it was (`0x26498`;
  the flag for this, ebp+0x4a, is set by nobody, in DOSBox a stack address stood there, not 0).
- The race keeps the track. The next start plays the track's song again.

**In the core** the pause works the same way. B is ENTER and START is ESC. A held key repeats after 0.5 s,
every frame, like the keyboard in DOSBox. The CPU darkens the shown frame once through the table and stores
it as a still in the SDRAM (`still` in `main.c`). The blitter copies the still every frame and puts radio,
cursor and displays over it (`hud_cd`, `pause_race`). The volumes go to effects and music (`snd_master`,
`music_master`); MU goes to the FM music (`music_master_mu`).

**The replay** (from the EXE):

- It starts 5 s after the end of a finished race, as soon as the commentator has said everything he intended
  (`0x21048` asks `0x24740`; until then the end screen stays over the track-side camera). The race then runs
  once more. There is no replay after time up or SELECT (`0x1fe28`).
- The game does not record the cars but its controls, each frame (`0x22f34`). It rebuilds the race
  (`0x1fd64`, `0x128dc`) and reads the controls back instead of the keyboard (`0x22e74`). Everything else it
  recomputes.
- The core records per physics step (1/50 s) the controls `car_step` receives (`car_keys`: B, Y, left, right
  held, L and R only in the step of pressing, plus the left stick lateral and both triggers of a dock
  controller as raw bytes; 6 bytes, up to 20 minutes). It rebuilds effects, sounds, scenery, car, opponents
  and music as at the start and runs the race from the recorded words, with as many steps per frame as in the
  race.

In the replay (`0x22d98` sets the flag `0x118e68`):

- The track-side camera is used from the start (`0x12afe`). There is no crash camera (`0x1857a`), and the
  view key does not work (`0x19b53`).
- Of the race display only an "R" is shown, at (8, 170), font 0, colour 6, while bit 7 of the 100 Hz counter
  is set (`0x2346c`). Labels, countdown, lap, position, clocks and radar are omitted (OpenScreamer's notes are
  wrong here). The traffic light (`0x20d10`) and the end screen (`0x21048`) are as in the race.
- Commentary and sounds come as in the race (the game does not query the flag there). The lap record stays as
  the race left it, hence no second "new lap record".
- When the words are used up, the replay ends (`0x22dd8`). B, START or SELECT skip it (ENTER and ESC in the
  original). Then PLAY AGAIN? follows.
- Checked on the PC (`REPLAYCHECK` for `tools/render.sh`): a whole race of a driving robot on track 1 (car 6,
  8391 steps, three laps) and its replay, at other frame rates and on the track-side camera, are
  bit-identical in every step (player and nine opponents).

**PLAY AGAIN?:** SELECT in the race, after time up (5 s after the end, as above) and after the replay.

- The original asks after every left race (`0x1fe86` -> `0x219d0`, except in the championship and network;
  after a finished race only after the replay). ESC stops the effects (`0x1f8cc`), and the music continues.
  `0x219d0` draws the scene once more (`0x81870`, 320x200, also from the cockpit) without race display and end
  screen, that is without position, total time, TIME OVER and the replay's "R".
- The core draws a last frame for it, without a physics step and without race display, radar and dashboard
  (`ask` in `run_track`), the same in all cases. This frame is darkened three times. Over it are `GAME.TXT`
  rows 19-21 (PLAY AGAIN?, YES, NO), centred in rows 52, 92 and 122 (font 0). The question is in colour 2,
  the answers in 14, and the chosen one blinks to 2.
- Up/left selects YES, and down/right selects NO, each press with the click (see "Menu sounds"). B or START
  takes the answer. A or SELECT is NO (ESC in the original).
- YES rebuilds the race (without rebuilding the track), and NO leads to the main menu.

## Fades and menu sounds

This section answers how the screen fades in and out, and which sounds the menus play.

**Fades** (from the EXE). All fades go through the palette: each value is multiplied by f toward black
(`0x27de8`; `pal_faded` in `main.c`, into the palette of the buffer shown next).

- When a menu screen is left (track -> car, car -> track, car -> race), it is black at once (`0x1d220` = 0:
  the palette black in the frame of the key press; `black`). The new screen fades in at 1.5/s, in 2/3 s
  (`0x20374` from `0x2c078` and `0x2bdcc`, `0x29738` every frame), with models and camera already running.
  The main menu pages do likewise, at 0.6/s (see "Main menu and options").
- There is no camera flight between the track and car screens (only when leaving the menu pages).
- The loading picture appears without a fade (`0x2a264`). It fades out in half a second when the race is
  built, the picture only (`0x128dc(1)` -> `0x21d8c(0)`; OpenScreamer's notes overlook this).
- The race fades in in half a second, picture, effects and music (`0x21e50` every frame, 2/s). Leaving (PLAY
  AGAIN? NO, `0x1feed`) or rebuilding (YES, the replay: `0x128dc(0)`) fades out likewise, with the sound
  (`0x21d8c(1)`).
- The game stands during the fade-out. The core keeps the frame loop running so that the music keeps getting
  data: the still picture is copied into both buffers, and each frame one gets the palette and is swapped
  (`fade_out`).
- The pause does not fade.

**Menu sounds.** While a menu is up, the game's sample table `SOUND_FX` comes from `MENU.DTR` (GEN.DAT:
`smenu1`-`smenu9`, `MENU1.RAW`-`MENU9.RAW`; `0x29e1d`, `0x1f2d0`).

- In the track and car screens only the step to the next track or car plays, row 6, `smenu7` (`0x2859e`,
  `0x28617`, `0x2874b`, `0x287cd`). Choosing and back are silent (the branches `0x28ef6`, `0x28172` and
  `0x2821e` play nothing; OpenScreamer plays row 0 there, by its own reading).
- On the pages, row 1 plays from row to row (`0x283ca`, `0x284ba`), row 2 for a changed option (`0x28df0`),
  row 0 on opening a page and on ESC back (`0x28b5f`, `0x280ec`), and row 3 for NORMAL GAME (`0x28ba2`).
  NETWORK GAME is silent.
- The core reads the nine samples at start at their places from `SOUND.DAT` (88 KB; the whole container
  follows with the first track) and resets the table after every race (`snd_menu` in `sound.c`).
- PLAY AGAIN? clicks on every press of up/left or down/right (`SFXSBASE.DTR` row 6, `sclick`; `0x21b0e`,
  `0x21b47`).
- The pause's CD player is silent (`0x21328`).

## Verification tools

This section answers how the graphics are checked against the C models and the original, and what the start
frames cost.

The simulation runs the track from `SIM_TRACK` (slot 28) with the first car at once. With `SIM_MENU=1` (slot
27) it shows the track screen instead, and with `SIM_MENU=2` the car screen. It checks the third frame
against the C models (both 0 pixels difference; the car screen needs 0.22 M clocks, 3 ms per frame). The
comparison of the race frame with the C models (`track_check`) is done only in the simulation. On the Pocket
the log says `track check: simulation only`.

`tools/render.sh` draws the same screens on the PC:

- `TRACK=n`, `CAR=r` and `MENU=1` or `2` draw the start with car r, the track screen or the car screen.
- `MENU=4` or `5` draws the main menu or the options (`MENUKEYS="DDDDB"` presses keys first).
- `PAUSE=1` or `2` draws the pause or the question over the start (`CD="4,1"`: cursor on key 4, pressed).
  Compared with DOSBox screenshots, the radio has 0 pixels difference except the blinking number.

The table shows the six start frames (simulation, fast-forward, near chase camera, frame 2, ten cars). Each
has 0 pixels difference to the C models. "Frame" is the whole time of frame 1; 20 ms = 1.485 M clocks.

- The columns "without the factors" belong to the firmware with the original's distances and without the
  level of detail.
- The columns "with the factors" were measured at `FAR_SCALE` 1.5 and `LOD_SCALE` 1.5. The defaults are 2.0
  and 1.2.
- Pieces, faces and pixels are those of the frame with the factors.
- The frame of track 5 without the factors ran no physics step in the simulation, which is about 0.09 M
  clocks less. The table adds this.

| Track | Placements | Pieces | Faces | Pixels | Frame without the factors | Frame with the factors |
|---|---|---|---|---|---|---|
| 1 Palm Town | 1164 | 255 | 1400 | 107,903 | 1,207,338 = 16.3 ms | 877,522 = 11.8 ms |
| 2 Lake Valley | 1408 | 247 | 1281 | 111,904 | 1,139,480 = 15.3 ms | 896,525 = 12.1 ms |
| 3 Lindburg | 1590 | 235 | 1334 | 96,583 | 1,054,694 = 14.2 ms | 884,449 = 11.9 ms |
| 4 Palm Town Night | 1764 | 264 | 1577 | 110,404 | 1,253,610 = 16.9 ms | 991,215 = 13.3 ms |
| 5 Sunbeach Hill | 2359 | 278 | 1673 | 118,817 | about 1,302,000 = 17.5 ms | 913,080 = 12.3 ms |
| 6 Sandrock | 2073 | 296 | 1415 | 132,644 | 1,265,507 = 17.0 ms | 915,198 = 12.3 ms |

Tracks 5 and 6 have more placements than 2048 (`track.c` holds 2560).

## Resource usage

This section answers how much of the FPGA the core uses, which parts use it, and how the logic was kept
small.

The Quartus build with the FM music uses (measured):

- 11,334 of 18,480 ALMs (61 %),
- 292 of 308 M10K (95 %, of which 16 are the table of routine 224),
- 34 of 66 DSP (52 %).

Timing holds at 74.25 MHz: setup +0.94 ns, hold +0.11 ns (five fitter seeds tried).

The FM music costs 646 ALMs, 10 M10K and 3 DSP. The 10 M10K are the write queue 1, JTOPL's shift registers 6,
its sine and exponential tables 2 and a register 1. They could go into MLABs for logic if block RAM runs
out.

From build to build the logic fluctuates by a few hundred ALMs. The placement packs differently densely, and
where timing is tight the fitter rebuilds logic and registers (physical synthesis).

![Space budget in the FPGA: logic 61 %, block RAM 95 %, DSP 52 %](resources.svg)

`tools/make_budget_svg.py` produces the picture from the measured rows of the fitter report
(`core/src/fpga/output_files/ap_core.fit.rpt`, table "Resource Utilization by Entity"). It should be
regenerated after every build; the script also writes the table below.

| Component | ALMs | M10K | DSP |
|---|---|---|---|
| Analogue frame (bridge, PLLs) | 421 | 2 | - |
| Framebuffer x2 + video | 120 | 130 | - |
| VexRiscv + 2x 8 KB cache | 1,262 | 19 | 4 |
| FPU (RV32F) | 1,458 | 5 | 4 |
| CFU (16.16 multiplication) | 9 | - | 3 |
| Boot RAM 32 KB + log | - | 36 | - |
| Geometry unit | 1,266 | 23 | 4 |
| Clipper | 951 | 6 | 2 |
| Setup unit | 1,506 | 7 | 4 |
| Rasterizer | 1,155 | 2 | 5 |
| Textures: pipeline + cache | 502 | 47 | 4 |
| Blitter (background, HUD) | 500 | 2 | - |
| SDRAM controller + ports | 309 | 2 | - |
| Audio: mixer 32 voices + music, I2S | 593 | 1 | 1 |
| FM music: OPL2 chip + write queue | 646 | 10 | 3 |
| Bus, registers, arbiter | 636 | - | - |
| **built** | **11,334 (61 %)** | **292 (95 %)** | **34 (52 %)** |

Block RAM is the scarce resource, and logic becomes tight too. From about 80 % utilisation it is hard on the
Cyclone V to hold the clock frequency. This shows at build time (timing analysis; `tools/build.sh` reports it
and tries further fitter seeds), not as instability on the Pocket, as long as timing holds.

The biggest items are the setup unit and the FPU (about 1,500 ALMs each). The setup unit is large for these
reasons:

- It computes bit-exactly like the C model in fixed point, hence wide: a corner is 144 bits, a polygon 576,
  and intermediate values and products go up to 65 bits.
- There are three arithmetic units in series so that they work simultaneously (clipper, corners, triangles).
  Each has one multiplier and a fixed sequence. In front of each multiplier sits a select (47 bits wide from
  28 steps in the triangle part). Behind it are register fields for the intermediate values (about 1,100
  bits), which are selected again.
- Variable shifters for normalising before the reciprocal table and scaling the results add to this (the
  setup unit has eight, 32 to 65 bits wide, the clipper three, a few hundred ALUTs each).
- Products wider than a DSP block (47x18, 38x18) add their partial products in logic.

The core has no depth buffer (see "Visibility without a depth buffer").

How the logic was kept small:

- Analogue's template project file sets synthesis to speed (`OPTIMIZATION_TECHNIQUE SPEED`,
  `MUX_RESTRUCTURE OFF`). Without restructuring, every wide register that is only held in many states of a
  sequence, and every partial select with too wide an index, becomes its own wide select. The core uses
  `BALANCED` and `MUX_RESTRUCTURE ON`: 14,046 instead of 15,646 ALMs, and better timing (measured on an
  earlier build).
- The units are written so that wide registers have an enable and small selects instead of being held in
  every step. One shared adder serves all read accesses (or counters). The matrix, origin and eye of the part
  and the rasterizer registers 7 to 18 sit in small block RAMs. Values are narrower where corners lie on
  whole pixels.
- The clipper takes the edge end from the buffer. A 4-bit edge number selecting from the 576-bit input would
  act like a shifter over 16 places (1,101 ALUTs alone). The plane distance is one adder with selected
  operands, and the clipper has one shifter instead of two.
- Each rewrite was checked clock for clock against the version before it with 30,000 to 200,000 random cases
  (the clipper with 90,000 random polygons), and in the simulation it gave the same pixels in the same
  clocks.
- The setup unit's 576-bit input buffer stays in registers. In block RAM it would save 190 ALMs but cost 19
  M10K, the scarcer resource.

The figures for VexRiscv and the FPU come from a synthesis of the CPU alone for the 5CEBA4F23C8 with
Quartus 18.1.1:

| CPU | ALMs | M10K | DSP | Fmax |
|---|---|---|---|---|
| RV32IM with 2 x 8 KB cache | 1,268-1,278 | 19 | 4 | about 115 MHz |
| with FPU | 3,052-3,061 | 24 | 8 | 98-111 MHz (median 105) |

The FPU makes float calculations 13-32x faster than soft-float. Another sweep of `imfc` (with the CFU) gave
3,000-3,037 ALMs and 105-108.5 MHz (`tools/vexriscv/README.md`).
