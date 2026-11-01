# Architecture

Overview of the Screamer core: the hardware partition, the memory layout, the game data on the
SD card, how the original's behaviour is reproduced, and the measured resource budget. The CPU
and bus are described in `docs/soc.md`, the memory system in `docs/memory.md`, the fill rule in
`docs/original_filler.md`, and the geometry formats in `docs/original_geometry.md`. Addresses
in backticks (`0x276a0`, ...) are code addresses in the original `STARTL.EXE`; "OpenScreamer
`docs/NOTES.md`" is the notes of the OpenScreamer port.

## Pocket hardware (main FPGA)

Cyclone V 5CEBA4F23C8: 18,480 ALMs, 308 M10K (10 Kbit each), 66 DSP blocks, 4 PLLs.
Separate memory buses: SDRAM 64 MB, PSRAM `cram0` and `cram1` 16 MB each, SRAM 256 KB (all
16 bits wide). Only the SDRAM is used; the SRAM and PSRAM controllers were removed (only the
memory test needed them; hints for re-adding them are in `docs/memory.md`).

## Partition

![Architecture: the CPU selects and sorts the parts and writes them into the geometry unit's buffer; geometry unit, clipper, setup unit and rasterizer draw the frame into the back framebuffer in block RAM while video_out shows the front one](architecture.svg)

Where the time of a frame goes, CPU sections and unit utilisation over 20 ms (drawn from the
log by `tools/make_timeline_svg.py`, also from the Pocket's `debug.txt`):
[frame_timeline.svg](frame_timeline.svg).

The CPU selects the parts in range, sorts them back to front and writes them into the geometry
unit's buffer. Geometry unit, clipper, setup unit and rasterizer work on them concurrently; the
rasterizer reads texels from the SDRAM through its texture cache and draws into the back
framebuffer while `video_out` shows the front one. The buffers are swapped in the vertical
blanking.

| Memory | Content |
|---|---|
| SDRAM | Program, geometry, game state, textures of the current track (`MAPn.MLB`, up to 3 MB, through the 16 KB texture cache) |
| SDRAM | Sound effects (`SOUND.DAT`, 1.9 MB, unpacked; the mixer reads the samples directly) |
| SDRAM | Music: the ring of the CD stream (256 KB), or the MIDI songs and instrument banks for the FM chip (`MUSIC.DAT`, 0.2 MB, read by the firmware) |
| Block RAM | Two framebuffers (2 x 64 KB) and two palettes, boot RAM, CPU caches, geometry unit buffer, texture and mesh caches, transparency table (`MAPn.SMG`), the mixer's lines (32 x 32 bytes), the FM chip's write queue (256 x 32 bits) and its ROMs and delay lines |
| SRAM, cram0, cram1 | not used |

One texture cache line costs about 30 clocks from the SDRAM and about 130 from the (asynchronous)
PSRAM; hence everything is in the SDRAM, including the sounds, which the mixer reads slowly and
evenly. For each voice the mixer keeps a 32-byte line around its position in block RAM and
requests the next one as soon as the position moves into it; a whole output sample (1547 clocks)
passes before it is read, which absorbs any wait.

## Memory needed by a race (measured, unpacked)

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
| Tables | `TABS/*.TAB` | about 410 KB |

## Game data on the SD card

The original files, unchanged, in their folders under `Assets/screamer/common/`: the game's
`DATA`, `GFX` and `TABS` folders as they are (the Pocket accepts a folder in a slot's file name,
e.g. `GFX/MAP1H.DAT`). One data slot per file, with `deferload`. The core reads the table of
contents at the end of a file and fetches the entries it needs. The firmware unpacks them
(`dat.c`, LZW) and builds the track image at start-up (`track_build.c`; byte for byte what
`tools/make_track.py` builds on the PC; in the simulation 3.2 s, of which 1 s unpacking, the rest
mostly the camera path in `double` and the checksums).

All files would be 33, one more than the 32 slots. The core runs the HI detail set, so the six
L tracks are dropped. `MUSIC.DAT` was left out for the CD image and came back as its fallback (the
songs and instruments of the FM music, which the original played when it found no CD). Result: 19
game files plus the CD image and two save slots:

- Track 1: `MAP1H.DAT`, `GAME.DAT`, `MAP1.DAT`, `GEN.DAT`, `LOCAL.DAT` (slots 3 to 7).
- `SOUND.DAT` (slot 8, unpacked; the mixer plays the samples from the container).
- Tracks 2 to 6: `MAP2H.DAT`-`MAP6H.DAT` (slots 9 to 13) and `MAP2.DAT`-`MAP6.DAT` (14 to 18), loaded
  to the same addresses as track 1's once the track is chosen.
- `GFX/MENU.DAT` (slot 19) and `GFX/GRAF.DAT` (slot 20) for the track and car selection. Of these
  two the core reads only the table of contents and the entries it needs, each at its place as
  in the file (`load_parts` in `main.c`): 2.4 MB instead of 5.9 MB, about 1.8 instead of 4.5 s at
  the Pocket's measured 1.3 MB/s (of which 1.4 MB `MENU.MLB`, whole, because the cars' textures
  are spread over the whole file; plus the six loading pictures `LOADnL.PCX`, 0.28 MB, about
  0.2 s, and the eight championship pictures `CHAMPnL.PCX`, 0.33 MB, about 0.25 s).
- The CD image at the top of the game folder: `SCREAM.DAT` (slot 21, the cue sheet) and
  `SCREAM.gog` (slot 22, 383 MB). The core reads only the music from it, 32 KB at a time while it
  draws (see "CD music").
- Two nonvolatile slots for the saves, `CHOICE.DAT` (23) and `ATTACK.DAT` (24), in the Pocket's
  save folder, not with the game files (see "Championship").
- `DATA/MUSIC.DAT` (slot 25, 200 KB): the MIDI songs `MUSIC1.HMP` .. `MUSIC7.HMP` and the instrument
  banks `MELODIC.BNK` and `DRUM.BNK`. It is read whole, once, when the FM music is first needed (see
  "FM music"): at start if there is no CD image or the Pocket menu's Music Source says FM Synth,
  else when that is chosen.

`GAME.DAT`, `GEN.DAT`, `LOCAL.DAT` and `SOUND.DAT` are loaded once. `SOUND.DAT` (1.9 MB, 1.3 s)
loads while the first track is built: the firmware only starts the read (`sound_start` in
`main.c`), the Pocket writes the file into the SDRAM while the CPU builds.

- `DATA/`: all 16 (`GEN`, `LOCAL`, `MUSIC`, `SOUND`, `MAP1`-`MAP6`, `MAP1I`-`MAP6I`)
- `GFX/`: `GAME`, `GRAF`, `MENU`, `MAP1L`-`MAP6L`, `MAP1H`-`MAP6H` (L and H are the sets of the
  DETAIL option: L at LOW, H from MID; not the resolution)
- `TABS/`: `PERSP01.TAB`, `VIS.TAB`

## Design decisions

- **Language:** own code in Verilog (like the template). VexRiscv is generated once from a Scala
  configuration as Verilog; custom CPU instructions go through its CFU plugin.
- **50 Hz, one game step per frame:** the original has no fixed rate; its physics runs with the
  measured frame time, limited to 0.02-0.2 s, so it never runs faster than 50 steps per second
  (OpenScreamer `docs/NOTES.md`, driving model). Video therefore runs at exactly 50 Hz
  (480 x 512 clocks at 12.288 MHz; the Pocket's screen takes 47-61 Hz) and each frame is one
  20 ms physics step: the game at its best rate, without intermediate frames or judder. The clock
  for countdowns and lap times ticks at 100 Hz in the original and has its own counter register.
  One frame is 1.485 million clocks.
- **VGA version:** 320x200 with an 8-bit palette like the game's VGA version. The original takes
  its tracks from both sets: `MAPnL` at DETAIL LOW, `MAPnH` from MID (`STARTL.EXE` like
  `STARTH.EXE`, OpenScreamer `docs/NOTES.md` section 27). How far is drawn is set by DETAIL as in the original:
  parts up to 52000 units, faces clipped at depth 27808 (LOW, MID) or 43808 (HI). The SVGA
  version draws just as far (same table at `ds:0x21d8` in `STARTH.EXE`, same comparison with
  52000^2/4 at `0x8340b`); it only doubles the pixels (focal length 128, result x 2, centre
  column 319). The core always uses the full H set and the HI distances; DETAIL is only displayed. Cockpit, race info and other static pictures are sprites from the L entries of
  `GAME.DAT` (`COCKnnAL`, `COCKnnBL`, `CDFRONTL`, `GAMEL`, `IMPLET_L`, `IMPRES_L`), copied 1:1
  pixel-exact into the framebuffer by the blitter. In `GAME.DAT`, L and H mean the 320 x 200 and
  the SVGA pictures, not the DETAIL sets of the `MAPn` files.
- **Two framebuffers in block RAM** instead of PSRAM: the rasterizer writes the back one at one
  pixel per clock, overdraw costs nothing, and no PSRAM controller or framebuffer arbitration is
  needed (saves logic, the scarce resource). Each buffer has its own palette, so palette changes
  (fades) match the frame exactly. The drawing side also reads (smoke and shadows blend with the
  frame content, routine 224): four explicit 8-bit memory lanes with a read/write port
  (`fb_lane.v`), because Quartus 18.1 otherwise silently builds memory with true byte enables
  from registers; `tools/check_budget.py` checks the 128 M10K on every build. The swap happens at
  the start of a line in the vertical blanking, at most once per blanking (`video_out.v`): a
  frame that takes slightly more than 20 ms does not immediately drop to 25 fps.
- **Exact per-pixel perspective correction:** a reciprocal unit (table plus one Newton step,
  2-3 DSP) delivers one reciprocal per clock, so it costs no time. OpenScreamer's software renderer
  computes only every eighth pixel exactly, hence its kinks in near textures. The fixed-point bit
  widths were determined on the C model against floating point.
- **Number formats:** rasterizer and geometry in fixed point (screen coordinates with at least
  4 fractional bits); the physics on the CPU in `float` like the original.

### Visibility without a depth buffer (painter's algorithm)

The CPU sorts whole objects by the distance of their centre from the camera and emits them back
to front (original `0x83164`: `dist^2 >> 2`). Within an object the order is in the data: each
mesh block has sixteen precomputed face lists (header +0x28..+0x64), each sorted back to front for
one viewing direction: a 45-degree sector around the block's y axis each, for an eye above
(lists 0..7) and below (8..15) its x-z plane. The CPU picks the list like the game (`0x8a598`);
the geometry unit does not sort and drops back faces by the normal. As in the game, the list is
chosen by the camera in the block's frame plus the block's centre (`0x83511` scales this by 1.2,
which does not change the list).

The game tests back faces with the same point (`0x835a0`), but its filler (`0xd55ff`) draws
nothing of a face whose corners run the other way on screen, i.e. one facing away from the lens
eye (128 behind the camera). The geometry unit's back-face test therefore uses this eye (with the
point alone, the nose of the first car in the car selection looked through the front tyre). The
track data rely on this order (grass faces below the road, two objects at one spot); OpenScreamer
needs special rules for it with its depth buffer. Clipping at the near plane remains necessary.
A depth buffer would cost about 125 M10K (320x200x16 bit fits only the 8-Kbit modes), and every
hidden pixel would be rewritten, which makes framebuffer bandwidth more important.

**Object order as in the original** (read from `STARTL.EXE`): `0x83164` builds the list from the
grid cells, per cell run 1 before run 0, with `dist^2 >> 2`; `0x8a6a8` appends the moving objects;
`0x8a7b0` turns this into the key -(sqrt(dist^2 >> 2) rounded >> 2), roughly distance/8; `0x832c0`
sorts; the farthest is drawn first. A moving object (car) counts **800 closer**, 6400 units;
otherwise the long road piece it stands on overpaints the car, on a lap with the near chase
camera at every fourth spot. A mesh with a class in the last word of its object header (+0x32c)
counts farther away: class 3 by 6000 (on track 1 only `akk1tap`, the grass under the road),
1 by 1500, 2 by 800, 4 by 200. OpenScreamer draws with a depth buffer and has its own rules for
this (grass "twice as far", objects above their road); the core draws like the original and needs
only its key.

### Geometry chain

Measured on the CPU (fly-through of track 1): about 3000 clocks per triangle, almost half of it
per-triangle preparation (the planes of Q, U, V with divisions), a quarter clipping and
projection, vertex transformation only 3 %. Hence the units, in this order:

- **Setup unit** (`setup.v`, C model `firmware/setup.c`): the CPU hands over polygons in camera
  view (five floats per corner); the hardware projects, splits into a fan and computes the
  rasterizer's values in fixed point while the rasterizer draws the previous triangle.
- **Clipper** (`clipper.v`, in front of the setup unit): with the setup unit alone the CPU time in
  the heaviest frame fell from 10.4 to 5.8 million clocks, 58 % of it clipping. Clipping is as in
  the original at the near plane (depth 128) and the far plane of the DETAIL level, plus a guard
  band at 14 x depth only so that screen coordinates fit 16 bits (the rasterizer clips at the
  screen edge). The lens is the original's: focal length 128, eye height at 65 % of the frame
  height, and the eye 128 units behind the camera (the division at `0x83420` reads
  `TABS\PERSP01.TAB`; screen point = centre + 128 * x / (128 + z), z from the camera).
- **Geometry unit** (`geom.v`, C model `firmware/geom.c`): the CPU selects and sorts the parts and
  writes 19 words per part (matrix, origin, eye, mesh, face list; layout in `docs/soc.md`) into
  the unit's buffer (2048 words, 107 parts, block RAM). The unit reads the mesh itself from the
  SDRAM, transforms the vertices, drops back faces, looks up the materials and hands the faces to
  the clipper. Because of the buffer the CPU does not wait for each part. It needs about 20 % of
  a frame; the rest is left for cars and game logic.
- **Texture prefetch:** counters in the rasterizer showed 76 % of its time waiting for texture
  lines, and a cache study with the C model's texel addresses showed half as many misses as
  counted: both caches fetched each missing line twice (fixed). Tiling the texture memory brings
  little (most misses are first accesses, about 7,800 lines per frame). So the rasterizer
  prefetches (after Igehy, Eldridge, Proudfoot 1998): tags are checked ahead of a queue of 64
  pixels, a missing line is requested at once (up to 16 in flight, into a line buffer), only the
  pixel that needs it waits at the queue head, and the line enters the cache once all pixels
  before it have read the old line. The next textured triangle starts as soon as the previous
  one's pixels are in the queue. In the heaviest frame the rasterizer is busy 0.49 instead of
  0.65 million clocks.
- **Cadence:** the geometry unit sets the pace: it is busy from the first part to the end of the
  frame (including waits for clipper and memory); frame time is roughly the CPU's selection plus
  the geometry unit.

**Faster chain at the start** (all ten cars in view: 248 parts, 2,473 faces; SoC simulation with
`SIM_DEFINES="-DGEOM_STATS"`): a frame took about 1.6 million clocks (25 fps) before the
following changes, with the geometry unit busy 1.15 million clocks (37 % waiting for reads that
cost 5 clocks even as cache hits, 23 % for SDRAM misses), the rasterizer waiting 289,000 clocks
for texture lines and the CPU 414,000 for the geometry unit.

1. A geometry-unit read puts the address straight to the cache RAMs and the next step takes the
   value (2 clocks on a hit; on a miss only that step waits); two more multipliers compute the
   three rows of a vertex at once (6 instead of 12 clocks).
2. The line port has up to two lines in the SDRAM controller; the texture loader requests two
   (`docs/memory.md`).
3. The firmware builds the cars only when they are due in the draw sequence (the near ones last,
   while the geometry unit works on the far scenery) and skips groups of 16 parts whose outline
   is out of range during culling.

Result: 996,000 clocks (67 %), geometry unit 812,000, rasterizer waits 193,000 clocks for
textures, CPU 28,000 for the geometry unit. Bigger caches bring little: a study with the start
frame's accesses (C model) gave for geometry 8,738 misses with 4 KB, 6,368 with 16 KB; for
textures 11,200 with 16 KB, 10,261 with 32 KB, and more (13,895) with 8x4 tiled textures. Most are
first accesses to about 360 KB of texture and 280 KB of mesh data per frame.

### Corners on whole pixels, fill rule

The original's projection (`0x83420` ff.) keeps x as 16.16 and uses only the upper half word, and
all fill routines (`0xd5350` textures, `0xd8844` one colour) read the corners as whole pixels from
the table at `0x54e94`. Corners of adjacent parts that are a few units apart in the world (the
stored angles are up to 2.5 units short) mostly land on the same pixel. The setup unit and the
rasterizer therefore truncate corners to whole pixels instead of rounding to 1/16 pixel.

The fillers run column by column from the smallest to the largest corner x, both columns
included (`0xda330`: each edge pushes N + 1 pairs, the pair at the vertex is overwritten), and
fill each column from the clipped top to the clipped bottom edge, both drawn (`0xda478`:
`sar 16`, then count + 1 pixels). A pixel (x, r) is thus drawn if the polygon touches the segment
from (x, r) to (x, r + 1), the lower end excluded. This makes every polygon about half a pixel
fuller all round than the "pixel centre inside the triangle" rule; neighbours overlap at the
seam, also where a corner lies on the other's edge (T junction), and asphalt stays closed. With
the centre rule and corners on whole pixels, 61 hole pixels remained at the four steepest slants
of track 1, with the original's rule 9, which are columns between parts that do not touch. The
rasterizer stays line by line (the framebuffer holds four pixels of a line in one word) and
computes the same set: per line both edges at the top and bottom edge. See
`docs/original_filler.md`.

### Handoff between frames (rules for CPU and graphics)

- The swap is requested only when the last pixel of the frame is written: an end marker runs
  through the whole graphics pipeline, only then does `swap_req_toggle` flip.
- Palette changes are in frame N's command list and are written into the back buffer's palette
  when frame N begins. The CPU never writes the palette directly, because it is already a frame
  ahead.
- The drawer knows its back buffer from its own toggle (back = not `req`) and waits only for the
  acknowledgement; it does not read `front` across the clock domain.
- A reset must not fake a swap: it sets request and acknowledgement to 0, after which buffer 0 is
  in front until the drawer swaps (checked in `sim/display` and `sim/soc`).

## Race: player, opponents, cameras, effects

### The player's car

The driving physics (engine, gearbox, brakes, steering, sliding) is OpenScreamer's transcription
of the game (`firmware/drive.c`), line by line. Ground contact over the four wheels, walls, markers
and camera follow OpenScreamer's glue code (`firmware/car.c`); the search for what lies under a
point is in `track.c`, working directly on the blocks in the SDRAM. The car itself is the twelve
blocks of `AUTOnn.3DX`, drawn as parts through the geometry unit in the game's order (tyres, hubs,
then the body); only the hubs rotate, the front blocks steer. The geometry unit selects a
texture's image by a per-part phase (paint, brake light, wheel image; the track textures by the
animation clock). Throttle is on B (down), brake on Y (left), as in classic racing games, camera
on X (up); not on the D-pad, so that steering diagonally does not brake.

**Analog controller in the dock:** a controller with an analog part (type 3 in `IN_KEY` bits
31..28, stick != 0) steers with the left stick, accelerates with the right one and brakes with the
left trigger, the way the game takes joystick axes: `0x24d40` turns each axis into a word
+-`0x3ff`, below `0x66` nothing counts (`0x24edc`); throttle = |word| / 1023 (`0x1611c`); a brake
axis IS the brake, |word| / 1023 (`0x16174`; a key instead ramps it to full with stiffness
`0x52`); the steering wants -word / 1024 of the lock (`0x1633b`) and is there at once at the
normal stiffness `0x50`, smoothed when sliding or starting (`0x163bb`). Stick and trigger count
only outside the dead zone, otherwise the keys. In the menus the stick acts like the D-pad beyond
half deflection (`in_keys`, `hw.h`). The core controls (`input.json`) know only keys; analog
values need no entry.

**Suspension** (from the EXE: `0x168a4`, drawn by `0x15c0c`): each wheel swings by up to 18 units.
The frame's speed change x 0.00175 pushes the front wheels one way and the rear wheels threefold
the other; the turn rate x speed^2 x 1.5e-9 the left against the right; both divided by the frame
time. A larger push restarts the oscillation, cos(4 pi t + pi), which decays (stiffness 61
lateral, 71 longitudinal, off below 2). From the deflections as from wheel heights: pitch
`+0x4e0` = atan((VR - HR) / wheelbase), roll `+0x4e8` = -atan((VL - VR) / track) with the game's
negative track, heave `+0x4c0` = mean. Only the four body blocks (`MV_mauto`, `fauto`, `rauto`,
`fai`) pitch by `+0x4e0` x 2, roll by `+0x4e8` x 4 and sink by `+0x4c0`; the wheels stay on the
road: the body leans outward in a curve, the nose dives when braking. Bumper and cockpit
(`0x1a2ac`) pitch by `+0x4e0`, roll by 3 x `+0x4e8` and rise by `+0x4c0`. Because the lateral push
goes through the frame time twice and the threshold 2 applies per frame, the suspension
(`suspension_step` in `car.c`) runs on the game's frames of 1/30 s, with the stiffnesses of the
game's table at 30 frames/s. At 140 km/h and full lock the body rolls about 3 degrees; under full
braking it pitches about 3 degrees. Opponents (`0x1df24`) roll by v x 0.02 and pitch by
-|v x 0.02|, so the nose goes down in every curve.

**Crash** (decision `0x16c90` in the EXE, sequence `0x18518` per OpenScreamer's notes): a wall hit
is a crash if speed x (angle to the road / 90 degrees) > 0.3 x top speed, but only in the open
(`+0x7b0`, the kind of the last marker, is 0; in a tunnel one only bounces off), and the wall
reaction happens anyway (in the game the crash only switches off the wall sound). OpenScreamer
does not check the tunnel and omits the wall reaction on a crash; the core follows the EXE. Then
speed x 0.6, direction of the road, thrown up at 3000/s, 12 rad/s about the lateral and
longitudinal axes, gravity 6000, four landings, the crash view (row 5 of the views) and then the
old one.

**Views** (table `0x1da0`): bumper, cockpit, near and far chase camera (X cycles in this order;
the race starts near; option `0x1d2a4`). The cockpit is the bumper camera over a 3D view of
132 lines (`0x4c610(320, 132, 85)`, `0x19cbe`) with the car's dashboard below: `COCKnnAL.PKC`
(B for the second paint, `0x13725`) from `GAME.DAT`, split into blitter boxes when loaded; the
numbers come from the `COCKnnL` section of `CARS.DTR` (`0x1545c`: needle length, pivots, lamps).
Speed and rev needles are three lines each in colour 255, smoothed like the original (`0x2367c`,
`0x2390c`), the gear lamp is a sprite; rev image, speed and gear digits are omitted there. The
start direction is that of the nearest road point in the map grid (`0x120c0`, called at
`0x13ea1`), not the direction of the spline handles (which was 1.6 degrees off).

**Track-side camera at the race end** (row 3 of the table, `0x1a620`): when the race is finished
(flag 8 at `0x18e14`) or time is up and the car stands (0x10), the game switches to it. Its
positions are the nodes of `MAPnRPL.SPL` (the loader `0x14fb4` stores the file at `0xf90c`; the
core reads it at race set-up from `DATA/MAPn.DAT`, `car_trackside_init`). The node nearest to the
car on the ground is taken (whole units, distance truncated, a node exactly above the car does not
count); from there it looks straight at the car, down by the elevation angle
(`2 pi - acos(flat/distance)`, even if the node is lower), without tilt. If the car is farther
than 1500 away, the camera moves along its line of sight until it is 1500 (each coordinate
truncated). Car and shadow are drawn; overlay 0: of the race display the labels (`IMPLET_L`),
countdown, lap, position, clocks and radar, rev image, speed and gear are omitted (`0x2367c`). The
switch is a cut (no camera flights). `TRACKSIDE=1` for `tools/render.sh` (with `HUD=1` including
the race display) shows it after the steps.

### Race display (blitter)

The original draws it in software after the scene: font from `GAMEL.FNT` (colour x 16 + shading),
sprites from `GAMEL.SPR`, the pictures `IMPLET_L`/`IMPRES_L` (.PKC), needles as lines, at the
places of `0x23400`/`0x2367c`. Here a blitter (`blit.v`) copies it pixel by pixel 1:1 into the
framebuffer, without geometry, setup and rasterizer: commands with x, y, w, h (copy, text) or as a
line, in a queue behind the frame's parts, executed as soon as the 3D units are done. The pictures
are split by `make_track.py` into boxes around their pixels, so that the whole HUD costs under 1 %
of a frame; a separate HUD buffer (64 KB per buffer) or a mask for the rasterizer would gain too
little. Traffic light, countdown, laps and times as in the game (OpenScreamer's `markers_step`,
notes section 12).

**Background** (`MAPn.BCK`, a 512x256 PCX, drawn before the scene with `0x83a68`): a pure copy,
the column `(2047 - heading) / 2 mod 512` at the left edge (twice around the image per turn), the
top line at eye height + 22 - 256 + 128 * sin(pitch), 320 bytes per line at a 512-byte line pitch
(a line that reaches past the image end reads into the next); below, up to the view's end, the
colour from TRACK_DATA (`color`, times four at `0x1337d`). The tilted version `0x83b44` (four
textured quads, rotated by the roll angle) is called nowhere in `STARTL.EXE`: when the camera rolls
in a banked curve, the background stays upright, as here. Eye height is 130
(`0x4c610(320, 200, 130)`), the projection centre in x is column 159 (`0x83431`), not 160. The
blitter copies the image (opaque, one pixel per clock) and fills the rest (four pixels per clock)
while the CPU selects the scene; the first part for the geometry unit waits until it is done.

**Moving parts of the track** (from the EXE: `0x8ad14`, `0x8acf0`, `0x8ac58`; format per
OpenScreamer's notes S2-5b, valid for both games): a mesh can have several blocks, and a block with
a position set (+0x0C) is moved from there or, if the mesh has images (+0x10), rotated per image:
12 words per image are its whole matrix (the stored corner as a row times matrix, 16383 = 1, then
the offset), the image is images x clock >> 16, one turn of the animation clock in 3.33 s. On
track 1 only the `H` set has such a mesh, `V220` at the start: the plinth with the video wall
(block 0, one fixed image) and the helmet on it (block 1, 80 images). The firmware splits such a
part into one job per block, the farthest centre first (`0x8ac58`). The images turn z-up into
y-up, a mirroring; the game takes the other side as the back face for such blocks (`0x8acf0`:
`jge` instead of `jl`), and so does the geometry unit (bit 3 in word 18 of a job, `docs/soc.md`).

### Opponents

(After OpenScreamer, whose addresses come from the EXE; notes section 11.) `MAPnOPP.SPL` has ten
splines, opponent k drives spline k, the tenth is the player's line; the start nodes are the grid
(two columns, the player last, position 10). The curve (`0x1d54c`, `0x1e7a4`) is a uniform
quadratic B-spline over the nodes' handles. An opponent neither steers nor slides: it drives at
`max_spd` x PACE of the last marker reached (+ `skill` while the player is ahead of it;
`0x1ced0`), accelerates with `accel`, brakes with `brakes`, and advances along its curve
(`0x1d2fc`). It stands on a point, the road under its centre, tilted by the normal of that face
x 1.117 (`0x1cb9c`), turns its nose in curves and leans the body (`0x1df24`, `sband`). Its markers
and laps count like the player's (`0x1db84`); the placement (`0x20418`) swaps at most one
opponent with the player per step, as soon as the player is over the line, opponents among
themselves never. The cars are the rows of `CARS.DTR` after the player's (`0x142e8`). The values
come from the difficulty file (`'A'` + level, `0x13251`); the intro uses NORMAL (`MAP1B.DTR`,
bonus time 40 s instead of 50 s at ROOKIE; the level the intro sets, `0x2777f`). An opponent is
drawn only if its sphere (radius 1200) is in the field of view.

**Contacts:** each car is two circles on the centre line (`0x136c8`, `0x142e8`: the body from 250
in front of the front axle to 150 behind the rear axle, length L, each circle L/4 from the end
and L/4 in size). Player and opponent (only within 1500, `0x12378`; `0x17818`, constants 0.5,
0.0001, pi/2, 0.21, 0.7 checked in the EXE): the opponent yields by the overlap, unless that would
push it off the road, then the player yields; if the player runs into it, the opponent is pushed
on at half the player's speed and turned, the player is pushed back with 0.21 x speed, turned
about the nose and loses 5 % speed per step (a wall hit); if the opponent runs into the player,
it keeps 70 %. Two opponents (`0x1cffc`, `0x1d1f0`): the faster falls to the mean, the later is
pushed aside. Overtaking (`0x1e404`): if the slower player is three radii in front of an
opponent, the opponent shifts its line just enough to get past. An opponent pushed off the road by
more than 3000 stops at once (`0x1cb9c`); if it loses more than 5000 units/s in one step it
crashes (`0x1d824`): depending on the spot it rolls over like the player or spins twice on the
road. The offset from bumps decays after 3 s.

### Routine 224 (smoke, shadows, radar) and routine 160 (reflections)

**Routine 224:** the game's filler (`0xcd9d0`) turns each pixel into `SMG[texel x 256 + pixel]`,
without light or depth; this is the only SMG lookup in the program. It is used by smoke (player
only, `0x19818`, `0x19970`: eight places, every 0.05 s at the rear wheels, travels with the car,
about 0.67 s, turned to the screen; each cloud offset sideways by (x & 7) * (T & 63) and backwards
by (z & 7) * (T & 63), x and z the car's integer position, which the game truncates toward 0 each
step, T its 100 Hz counter), by shadows (player `0x18cbc`: 20 units away from the camera, ground
- 2; opponents `0x1c83c`, `0x1cd34`; class 4 sorts 200 farther back), on track 4 by headlight
cones and lamp glow, on tracks 3 and 6 by a helicopter; never by glass. The rasterizer reads the
pixel from the framebuffer and looks it up in a 16 KB table (16 M10K, `RS_SMG`); texel 0 leaves the
pixel alone (row 0 swaps only colours of equal value, which saves 23-75 % of the pixels for smoke).
In blend mode the original's point rule applies, otherwise the shared edges of a polygon's
triangles would be blended twice. The radar (`0x23a68`) is a 72x72 window at (4, 124), darkened
through row 63 of the table (x 0.75, nearest palette colour, `0x25870`) as one blended quad after
the scene, with the walls (MMP field 3) on it as blitter lines in colour 255, rotated with the nose
up about (40, 160), opponents as sprite 5, the player as sprite 4 at (38, 158), one pixel
32768 / 24 units (the game's default, `0x1bc3e`). Track 5 places no props: the nodes of
`MAP5OST.SPL` do not lie above its road (unresolved, to be checked against the original).

**Routine 160 (reflections on the cars):** the windows show a reflection that moves with travel
and view. For each vertex of a block with flag 0x2000 (the body, 84-128 vertices with a normal)
the original computes on the x87 the reflection r = 2 (e.n) n - e of the view direction e at the
normal n (`0xc2230`) and takes u = round(127 r.x) + 127 - offx, v = round(127 r.z) + 127 - offz as
the texel, without material size, wrapping at 256. In the core this is done by the geometry unit
after the block's vertices, in integer arithmetic (`firmware/geom.h`): the normal from the float
bits to Q14, with the matrix into view space (the same step as for the vertices), the vertex
normalised to 17 bits, 1/|p| from a table of 128 and one Newton step on its three multipliers
(now 18x18), about 26 clocks per vertex; u, v in a RAM of 512 (1 M10K), from which the routine-160
faces take their texels; the rasterizer lets them wrap at 256 (size bit 29). Compared with the
floating-point form about 1 % of the values differ by one. What the original computes per object
stays on the CPU (`track.c`): the offsets offx, offz from three accumulators per object (`0xc1fc0`:
the distance moved since the last draw, rotated into the view, times the factor at `0xc2428`) and
the image: `0x1813c` sets for the player's car, by the kind of the last marker reached, car
+0x7b0 = 0 / 0x8000 / 0xff00 (kind 0 / 1 / 2) and the factor 0.002 or 0.03, `0x1607c` copies it to
the object set (+0x3c), the image drawn is n x phase >> 16. In track 1's tunnel (markers 3 and 4,
kind 1) image 1 of the texture, the tunnel ceiling lights, replaces the sky; opponents keep image 0
(they do not call `0x1813c`). Measured against the original (DOSBox, start grid, near chase
camera; `tools/render.sh` with `CAM` and the accumulators from its memory, `REFL`): of the 128
body vertices 95 are equal, the rest differ by one; in the rear window 65 of 125 pixels are equal
(84 % equal to a neighbour); with the stored u, v it was 9. The cost in the simulation:
geometry unit +36,000 clocks, start frame 1,338,092 clocks = 18.0 ms.

## Sound

**Mixer in hardware** (after OpenScreamer's notes section 17 and the EXE): the game's driver runs
at 11025 Hz and takes the stored sample without smoothing (index += pitch 16.16). `audio.v` does
the same with 48 kHz at the output: per output value one pass through 32 voices (four clocks per
voice, one data path, one DSP), the byte at the position times volume left and right, the sum
limited to 16 bits, then the position advanced by pitch x 11025 / 48000 (the firmware converts).
The registers are in MLABs, per voice a line of 32 bytes in one M10K, fetched through the line port;
the output clock is the template's (MCLK 12.288 MHz), everything on `clk_74a`. The C model
`firmware/mixer.c` computes the same value by value (`tools/sim.sh audio`, 60,000 random values).

Volume: row of `SFXSBASE.DTR` times master; in the game `0x6000` (`0x1bc2f`), here `0x4000`
(-3.5 dB) so that the player's engine and the three opponents' together never clip (in the
simulation before the change peak 27,355 after 1 s of start grid). The sounds:

- Engine (one voice, pitch rpm x 14 + 32768 every step, `autogall` in the tunnel).
- Sliding, with the smoke's condition (`0x17d04`, start and stop at `0x180d0`).
- Wall (only without a crash, `0x16f0b`), crash (`dentone`), props (`hit_0`), contact with an
  opponent (`crash`, at most 30 times per second).
- Sounds with a location, heard at the camera (`docs/soc.md`, "Sound"): the engines of the three
  nearest opponents (they rev up on the start grid) and the track's vehicles (trucks, buses, jet;
  the fourth column of `ANIMATIONS`), with Doppler, and the only ones in stereo.
- The speaker, firmware only (`speech.c`, `docs/soc.md`, "Sound"): 58 recordings from
  `SPFXBASE/SPFXFULL.DTR`, 27 events with the game's triggers, times and queue, one voice in the
  centre, master `0x3000` (with `0x4000` the countdown clipped over engines and music).

**CD music** (OpenScreamer's notes section 17, MUSIC): the game plays song n as CD track n + 1,
n from `CD_TRACKS` in `MENU.DTR` (GEN.DAT): track selection screen track 3, tracks 1-6 tracks
6, 4, 5, 7, 8, 2; when the track ends the drive restarts it. The core reads GOG's CD image from the
SD card unchanged: `SCREAM.DAT` (cue sheet, slot 21) says where the tracks begin in `SCREAM.gog`
(slot 22, raw, 2352 bytes per sector). In the mixer one stream of 16-bit stereo at 44.1 kHz,
linearly interpolated to 48 kHz, from a 256 KB ring in the SDRAM (`docs/soc.md`, "Sound");
`firmware/music.c` fills it completely at start (224 KB, about 170 ms on the Pocket) and then once
per frame with a 32 KB read command that the CPU only starts: the Pocket does the rest through the
bridge while the frame is drawn. Volume `0x4000`: the game's CD master is `0x6000` (0.75); as with
the effects this is two thirds of it, which keeps the ratio of music to effects as in the game and
the sum within 16 bits (with `0x6000` music and the start-grid engines clipped, 267 values in one
second of simulation).

**FM music** (OpenScreamer's notes sections 17 and 32; `firmware/fm.c`, `core/src/fpga/core/opl.v`):
the original plays `MUSICn.HMP` (the same n) instead when the machine has no CD, through the
sound card's FM chip, the OPL2 of the AdLib. In the core the chip is JTOPL (Jose Tejada, GPL-3.0,
`core/src/fpga/core/jtopl/`, with six corrections that make it match the real chip), clocked at
3.579545 MHz by a fractional divider from `clk_74a`, so that it makes 49,716 samples a second and
pitch is true (`sim/opl` measures a 875.4 Hz note to within 0.05 %). Its output is mono; it is
interpolated linearly to 48 kHz and starts the mixer's sums, so effects, CD music and FM music add up
before the limiter, and without FM the mixer is bit for bit as before (`tools/sim.sh audio`).
The firmware is HMI's player and driver as the game has them: it steps the songs' tracks 120 times
a second, sends their messages to the driver (nine voices, instruments from the banks, levels from
channel volume and velocity, pitch bend, drums on channel 10, every rule and its address in the
comments of `fm.c`) and a song that has ended is started again. `tools/fmcheck.sh` runs it on the PC
against openscreamer's driver for all seven songs: the same 36,000 to 100,000 register writes each,
tick by tick. The CPU runs once a frame but notes are due to 8 ms, so each write goes into a queue
in hardware with the time at which the chip is to get it, about 60 ms ahead; the queue (256 writes,
one M10K) releases them in time and spaces them 36 us apart as the original's driver did. The game's
music master (the CD player's MU) and the pause's fade reach the chip as the driver's note levels,
as in the original; the CD master only the CD. Source: the Pocket menu's Music Source, Auto (the CD
image if its tracks are there, else FM if `MUSIC.DAT` is) or FM Synth (always FM); the game's own
MUSIC option switches both off. The CD player of the pause lists tracks 2 to 8 for the seven songs;
leaving it with another track plays that song from its start.

## Intro

As in the original (`0x276a0`, OpenScreamer `docs/NOTES.md` sections 26 and 30), in
`firmware/intro.c` and `run_intro`/`intro` in `main.c`. The original's main program (`0x10020`)
alternates intro and menu: the intro at start before the main menu and again when the main menu
(page 0, no page open) has stood 30 s without a key (`0x288bf` counts, `0x2802f` with `0x100edc` =
30.0; `MENU_IDLE`).

- **The race:** a real race without a player (`0x27710`: mode `0x100`, flag `0x2001`: started at
  once, "attract"; difficulty NORMAL, option word 0), on track `CHOICE.DAT` +0x434 + 1; the counter
  advances after each intro and wraps to 0 after 5 (`0x27f06`). Set up like a race, but directly
  (`0x128dc`, no loading picture: the screen stays black), with the menu music. Only at the core's
  start the track's loading picture is shown (a deviation so that start-up does not stay black for
  long; `first_picture` in `main.c`): right after `CHOICE.DAT` the program loads `LOCAL.DAT` and
  from `GRAF.DAT` only the table of contents, `MENUL.SPR`, `MENUL.FNT` and `LOADnL.PCX` (about
  100 KB), shows the picture as before a race, loads the rest of the menus behind it, builds the
  track and loads the sounds; then it fades out as before a race. The log gives the time since the
  start (`first picture: ... ms after the core's start`). Each frame only the opponents, props,
  moving scenery and the camera run (`0x2780c`, the five routines per opponent); the player's car
  does not exist (`car_intro`: taken out of the world, its engine silent, `car_step` does not run).
  The picture fades in like a race in half a second (`0x21e50`); the cars start as soon as it is
  fully there.
- **The camera** (row 4 of the view table, routine 8: `0x1aeb4`): each opponent has its own
  (`0x1a920`, set +0x4f8): the post from `MAPnCAM.SPL` (14-29 nodes) nearest to it on the ground,
  aimed at it like the camera at the track side; `|height| % 10` of the post is a code: bit 1
  always, bit 2 while the car moves away, bit 4 while it approaches, the camera moves up to 1500
  from the car (at a new post the distance counts from the car's position one step earlier). If
  this changes at the same post, it flies over (`0x19ad8`, `0x19ef0`: the distance decays with
  damp[90] or 0x19 per step), a new post is a cut. One car is shown (`0x110a14`, initially the
  first); every 10 s (`0x27e64`, 1000 ticks) `0x19d82` prepares the next at a different post, it
  takes over at the shown car's next post change (`0x1aef3`). The sounds (opponents' engines,
  scenery) are heard from this camera.
- **Logos and credits** (`0x279d4`): the blocks of `ATTR.TXT` (`LOCAL.DAT`, separated by `#`, 19
  of them), font 0 from `ATTRL.FNT`, colour 2, line i at y = 20*i, each glided from +350 to the
  centre (stiffness 0x5a, whole pixels, `0x27b64`); after 4 s they go out to -400, when one is
  beyond -360 the next block comes. Plus the logos from `LOGOL.SPR` (`0x279fc`): at the first block
  sprite 0 (y 4), at the second sprite 1 (y 28), glided in from -320 (0x50) and out with the block;
  from the third sprite 2 (y 4), in with 0x5a, out to -340 at the last block (18). All in the
  track's palette.
- **End:** B, A, START or SELECT (`0x211d0`: ENTER, ESC, space, joystick button) or the end of the
  credits (state 3) fade picture and sound out in one second (`0x27d28`), then the main menu, each
  page on its first row (`0x27eb4`).

## Main menu and options

As in the original (its screen 0; the pages are tables of 16-byte entries `[row, id, value,
maximum]` behind `0x2de4`; OpenScreamer `docs/NOTES.md` sections 18 and 26), in `firmware/menu.c`.
Page 0: NORMAL GAME, CHAMPIONSHIP, SPECIAL MODES, NETWORK GAME, OPTIONS; page 1: DETAIL
(LOW/MID/HI, initially HI), difficulty (ROOKIE/NORMAL/PRO, initially NORMAL), LAP NUMBER
(3/5/10/25, initially 3), SPEECH and MUSIC (initially ON), VIEW RECORDS; the words from `MENU.TXT`
(rows 0-36; an option shows its row + its value).

**The words are cubes in the menu scene, as in the original** (`0x2b33c`): each letter is a cube
`ALETTER.3DX` from `MENU.DAT` (516 units), its texture (material 1 of `MENU.MLB`) has 76 images,
one per character; the cube's set has at +0x20 `0x35e * (character - 32) + 0x226`, a quarter of
that is the geometry unit's texture phase. Front the bright upper half of the image, back the dark
lower one. Row i of n lies on row `8 - n + 2i`, y = `0x820 - 0x180 * row`, the letters 640 apart,
centred (`0x2af30`), spaces too; the camera stands at (0, 0, 4100) looking toward -z, lens as in
the race (`0x29ea0`). Each step (`0x2b8b8`) the chosen row turns forward at 4 pi rad/s, the others
backward; each cube bobs in z by `150 * sin(2 pi * fx + pi * fy + t * k)` (t at 3 pi/s, back at
8 pi; chosen row fx = 0.5, k = 1/4). On entering, the letters fly in within a second, start and
swing from one of the eight rows of `0x2218`, in turn, the start turned by 0.09 rad per letter.
Changing page or option: all turn away, when the chosen row is past the quarter turn the new
page stands there and its row finishes turning (`0x2b380`). The cubes go through the geometry unit
like the models of the track and car selection, the farthest first. Behind them `MENU1L.PCX` with
its palette, over it the two SCREAMER logos (sprites 15 and 14 of `MENUL.SPR`, at y 0 and 16)
gliding in from the left at 350 and 700 pixels/s (`0x2df38`).

**Keys** (`0x28080`, `0x28ac4`): up/down from row to row (no wrap), left/right changes an option,
B or START too (the original's ENTER counts up, `0x297d4`; only when all letters have landed). B or
START on OPTIONS opens the options, A or SELECT leads back. NORMAL GAME: the camera moves back to
z 20,000 (factor 1 -> 0 at 0.6/s, fourth power) and rolls from -4 pi at pi/s, the letters stand,
the picture fades out at 2/s (`0x293e8`), then ENTER YOUR NAME (below) and the track selection.
NETWORK GAME shows `NETWORK NOT FOUND` like the original without a network (row 55, font 0,
colour 2, y 190, blinking). SPECIAL MODES, CHAMPIONSHIP and VIEW RECORDS: see the sections below.
The pages fade in at 0.6/s (`0x29ea0`): after the intro, after A or SELECT on the name, on a track's
records and after every race (`0x2a114` -> `0x29ea0`; back from the name or the records to the
page one was on, `0x28a7c`).

**ENTER YOUR NAME** (screen 4 of the original, before every game: all rows that start a game go
there via `0x293e8(4)`, `0x28b80` ... `0x28cba`; A or SELECT in the track selection leads back to
the name, `0x281c2`). The name appears in the records and is kept in `CHOICE.DAT` (+0x08,
initially "GRA"). The original needs no keyboard for it, so as there (`0x2c320`, `0x2cd74`,
`0x2b54c`, `0x2e4f8`): the 26 letters as cubes in a ring (radius 3500, every 2 pi/26), each falls
0.04 s after the previous from 2560 (odd 3060) to 160 and bounces back to 0.4 of the height
(falling p to 1 at 2.5/s, rising at 2/s, height over p^2); the chosen letter's cube tilts at
4 pi rad/s to -36 degrees to the camera, the others to 126 degrees. The camera stands outside
(radius 4500, height 1800, 36 degrees down) and moves to the letter in 1/8 s linearly (`0x29530`,
the heading the shorter way). Behind it `MENU2L.PCX`, over it the title (row 46, y 2, font 3), the
name at (0x83, 0x4c) in font 2 with fixed width, a bar 12 lower under the letter being set (21
pixels per letter). Keys (`0x28080`): down/up the next/previous letter (`smenu6`), left/right turns
the alphabet by one and sets its letter (silent; only when the camera is almost there, under 0.1)
- ONE alphabet for all three letters, as in the original (`0x1d238`). B or START continues, A or
SELECT back to the page (both silent).

**The options take effect** (stored in `CHOICE.DAT`, see "Championship"): LAP NUMBER is the race's
lap count (`car_laps` in `car.c`; the original takes `0x21e0` + 1: 3, 5, 10, 25), the end of the race
and the "1/3" of the display follow it. The difficulty selects the file `MAPnA.DTR`, `MAPnB.DTR` or
`MAPnC.DTR` (`0x13251`; `track_build_difficulty`), from which the track build reads opponents and
`TRACK_DATA`. SPEECH OFF: the speaker says nothing (`speech_on`), MUSIC OFF: no music, CD or FM
(`music_on`), at once, also in the menu, unlike the original (which takes both over only when the
next race loads, `0x2a264`). DETAIL is only displayed: the core always draws the full H set.

## Special modes

SPECIAL MODES opens page 4 of the original (`0x28eab`): TIME ATTACK, CONE CARNAGE, SLALOM
(`MENU.TXT` rows 28-30, ids 28-30), A or SELECT leads back. The choice sets the original's mode word
(`0x1d25c`: 1 normal, 2 championship, 4 time attack, `0x10` network, `0x20` cone carnage, `0x40`
slalom; here `car_mode` in `car.c`, from `menu_mode()`), then as with NORMAL GAME the name, track
and car selection (`0x28bb0`). Page 3 (NEW/RESUME, ids 24/25) belongs to time attack, page 5
(ids 26/27) to the championship.

In all three modes (mask `0x64`): alone on the track (`0x1311c`, `0x1309c`, `0x12e14` create no
opponents), one lap (`0x2a216`), from the start place of the last opponent (`0x13ded`: spline
opponent count - 1 instead of its own, direction that of the road there), no lap best time to beat
(`0x2a264`: 0); after the finish no replay (only in the normal game, `0x1fe45`), straight to PLAY
AGAIN?. The speaker says no "lead" there (mask `0x13`, `0x24308`), in cone carnage and slalom no
"hurry" either (mask `0x17`, `0x242dc`). The original enters records only for the normal game and
the championship (`0x2e5e4`, see "Records").

**CONE CARNAGE** (`0x20`): the obstacles from `MAPnCNG.SPL` instead of `MAPnOST.SPL` (`0x14fb4`;
track 1: 36 cones, track 5: 99). Finish line and checkpoints give instead of the bonus time the
fourth number of `TRACK_DATA`, `carnage` (`0x195fc`; track 1 NORMAL 15 s instead of 40), far too
little for a lap: each step in which more cones are knocked down than before gives 1 s (`0x18b50`;
each counts once, `+0x3c`, `0x126a4`). At the finish `YOU MADE IT!` (`GAME.TXT` 14, y 90), with
all cones also `PERFECT!` (15, y 120; `0x23fbc`, `0x24204`), otherwise as always `TIME OVER`. The
speaker: "cones" (event 19) once after the line, "all" (26) as soon as all are knocked down
(`0x24570`, `0x24386`), at the finish "win". `MODE=0x20` for `tools/render.sh` draws and drives
(`REPLAYCHECK`) the mode on the PC.

**SLALOM** (`0x40`): the obstacles from `MAPnSLA.SPL` (gate poles, each two a road width apart),
the gates from `MAPnSLA.MRK` (count, then 32 bytes per gate: radius 1024, `+0x10` seconds 2-4,
`+0x14` x, `+0x1c` y, the middle between two poles; track 1: 14 gates). Finish line and checkpoints
give only `carnage`, as in cone carnage. Each step (`0x18b98`) the first gate whose circle the car
stands in (its whole position): the first time it gives its seconds and the door sound (sample 9,
`door`). At the finish `YOU MADE IT!`, `PERFECT!` only with all gates and without touching a pole
(`0x23f68`, `0x241ac`); "all" under the same condition. `MODE=0x40` for `tools/render.sh`.

**TIME ATTACK** (`4`, `firmware/ghost.c`): TIME ATTACK opens page 3 (`0x28e0d`), A or SELECT leads
back to page 4. NEW drives alone (`0x28c0f`: `0x1d260` = 0), RESUME against the track's ghost
(`0x28c3c`; without one, alone, `0x2a1df`). Normal bonus time, "hurry" yes. The original records
the drive (`0x231d0`, one pose per frame) and plays the ghost as a second car that nothing collides
with (`0x12e14` -> `0x1ba3c`, `0x14d28`; `0x23018`, `0x23308`): the recorded car on the start place
it drove from, the player on the other (`0x13ded`: recording 0 or 1 + opponent count - 1,
`0x1bb33` switches), its lap is the one to beat (`0x1d268`, the third clock; below it "new lap
record"), the player is second while the ghost is ahead (`0x20418`). At the finish (`0x23ef0`):
slower or equal `TRY AGAIN` (`GAME.TXT` 13) and "lose", otherwise (or without a ghost)
`YOU MADE IT!` and "win"; then the drive becomes the new ghost after the race (`0x1fd64` ->
`0x1b964`, `REPLAYS\MAPn.ATK`) and PLAY AGAIN drives against it.

In the core: one pose per 4 steps (12.5/s) from the set-up, played back in the same step, the
position through the original's cubic over four poses (`0x22724`), the angles interpolated between
two poses; without shadow and engine. Per track the best ghost, stored in `ATTACK.DAT`
(`ghost_track()` points into this slot's memory, see "Championship"), in the save file's format: a
16-byte header "ATK1", track, car, count, lap in 1/50 s, start place; then per pose 20 bytes: x, y,
height as int32, heading, pitch, roll as int16 with 65536 = 2 pi, race progress x 64 and the brake
light bits; at most 3000 poses, 60,016 bytes. `MODE=4 GHOST=1 REPLAYCHECK=9000 tools/render.sh`
drives two robot races, the second against the first's ghost (track 1: the ghost within one unit
of the first drive's position).

## Records

Per track five best laps and five best totals with names (`CHOICE.DAT` +0x6c and +0x24c, 0x50 bytes
each: five times, five names, five flags, for the totals the game mode; initially 180 s and 900 s
under "GRA"), `choice_record` in `champ.c`.

- **Entering** (`0x2e5e4`, after every race of the normal game and the championship: `0x2a1a8`,
  `0x1febf` before PLAY AGAIN, `0x28f42`; not if time ran out or the race was left, flag
  `0x1020`): the race's best lap always (flag 1), the total only at three laps (`0x1d2ac` = 2; flag
  0, game mode), each in front of the first time it does not beat, the others one place lower,
  under the name. In the original the flag of a total is the number of a stored replay
  (`REPLAYS\REPLAY<track><n>.RPL`, an "R" after the time, ENTER plays it); the core stores none, it
  shows the "R" only for a `CHOICE.DAT` from the DOS game.
- **The lap to beat** (`0x1d268`, the third clock of the race display, `car_lap_record`): before
  the race the track's best lap (`0x2a264`; in the special modes 0, in time attack the ghost's), a
  lap below it replaces it ("new lap record"), at the finish the race's best lap (`0x20eb8`);
  PLAY AGAIN starts with it.
- **VIEW RECORDS** (last row of the options, id 21) opens page 6 of the original (`0x28cd8`): the
  six tracks as cube words (`MENU.TXT` rows 31-36, id 31, the track as maximum), A or SELECT back to
  the options. A track leaves the page like a game (`0x28d0b`: `smenu4`, the camera moves back,
  fade out), then its screen 6 (`0x2c844`, `0x2d348`): `LOADnL.PCX`, the box (48, 36, 226x160)
  darkened twice, the track name (y 2, font 3), `BEST TOTAL TIME` (y 0x28) and `BEST LAP TIME`
  (y 0x78, font 0), below each five lines "n. NAM mm.ss.hh" (font 1, fixed width, centred, 10 apart
  from y 0x3c and 0x8c), everything in colour 0, the chosen total in colour 1. Up/down chooses a
  total (`smenu5`), A or SELECT back to the track page. The original has no paging to the next
  track. `STARTL.EXE` does not read `SCORE.TXT` (`LOCAL.DAT`).

## Championship

As in the original (its mode 2; OpenScreamer `docs/NOTES.md` section 21, checked in the EXE): rules
in `firmware/champ.c`, screen 5 in `menu.c`, the flow in `main.c` (`championship`).

- **NEW / RESUME:** CHAMPIONSHIP opens page 5 of the original (`MENU.TXT` rows 26/27, `0x28b3a`,
  A or SELECT back). Both leave the menu like NORMAL GAME. NEW (`0x28c6b`, `0x2c698`): stage 0, all
  places empty; RESUME (`0x28c9e`): back to the remembered stage.
- **Flow:** a table of 53 stages at `0x22b8` (44 bytes each: kind, two values, text block, next
  stage, stage on failure, picture `champN`, race, font), four leagues (ROOKIE, AMATEUR, PRO,
  BULLET) of 13 stages: before each of the six races a text picture (WELCOME TO ... before the
  first, otherwise PREPARE FOR THE NEXT RACE!), after each the standing, after the sixth
  CONGRATULATIONS!; stage 51 everything won, 52 LOSER!!. `champ_at` rebuilds it (regular, checked
  against the dump).
- **Car:** ENTER on the text picture opens the car selection (`0x291ae`), before *every* race, with
  the last chosen car; A or SELECT there leads back to the text picture (`0x28118`). The car is not
  fixed for the season in the original (OpenScreamer chooses it once at NEW).
- **Race:** tracks 1-6 in order, 3 laps, the opponents from the league's file `MAPnA`-`MAPnD.DTR`
  (`0x28ef6`: `0x2a264(2, track, league)`); in the Bullet league (D: one opponent) it drives the
  Bullet (`0x14373`: difficulty via PRO -> car 12, in the paint of its row; `opp_bullet`). A
  finished race continues by itself after the 5 s of the end picture, without replay and without
  PLAY AGAIN? (`0x1fd60`, `0x1fe86`); time up or left (SELECT): PLAY AGAIN?, YES drives it again.
- **Result** (`0x29acc`): a left race does not count (the stage stays); otherwise position, total
  time and points (9/7/5 for place 1/2/3, `0x21ec`) go into its row, and the stage goes to the
  standing. Time up remembers 1, unreachable 2: when even wins in the remaining races no longer
  reach the league's score (42/46/50/54).
- **Standing** (`0x2ce58`): `CHAMPnL.PCX` (n the league), the box (16, 36, 288x136) darkened twice
  (`0x25cc8` with the table for the picture's palette, 0.75, `0x25870`); the league name (font 2,
  colour 2, y 2), TRACK/POS/TIME/SCORE (y 48, x 28/88/170/246, font 0, colour 15), one row per
  race, 14 high from y 62: `-n-`, position, `mm.ss.hh`, points, dashes for a race not yet driven,
  `10`, `00.00.00`, `0` for one with time up; 4 lower TOTAL SCORE (right-aligned to x 276), 24 lower
  QUALIFY:n centred. ENTER (`0x292c8`): continue only among the first three (Bullet league: as
  winner), in time and after the sixth race with the league's score; otherwise LOSER!!, and RESUME
  drives the lost race again.
- **Text pictures:** the lines of the block from `CHAMP.TXT` (blocks separated by lines with `#`)
  centred from y 32, one font height apart, font 3 (league won and end: 2), colour 2. On all
  pictures PRESS ENTER TO CONTINUE blinks (row 56, y 190). B or START is ENTER, so is 10 s without
  a key (`0x2d301`); A or SELECT only on the text picture before a race, back to the main menu
  (`0x2829d`). No sound; fade in at 1.5/s, leaving with a cut to black like the car selection.
- **After the last race:** league won -> CONGRATULATIONS! -> WELCOME TO the next (one more track
  unlocked, `0x290a5`, see "Unlocking"); after the Bullet league YOU'RE THE GREATEST DRIVER EVER
  ... NOW THE BULLET IS YOURS ! (`0x29141`: Bullet, the backward tracks and AFTERBURNER unlocked)
  and back to the main menu, likewise after LOSER!!.
- **The saved state** is in the format of the original's settings file, `CONFIG\CHOICE.DAT`
  (`struct choice_dat` in `champ.h`: 0x460 bytes, read to `0x203c0` by `0x1bbb8`, written by
  `0x1be5c` before and after every race and on exit): car, track, name, options, from +0x18 the
  championship (stage, stage for RESUME, score, per race time, position, points), from +0x6c the
  records (per track five best laps and totals), from +0x42c unlocks. The core holds exactly this
  file as nonvolatile data slot `CHOICE.DAT` (id 23) in the SDRAM: loaded at start, without a file
  the original's defaults (`0x1bbb8`); every change (stage, result, options, last chosen track and
  car) is written there by the CPU at once, and the Pocket saves it on exit like `debug.txt`
  (`docs/soc.md`, "Log to the SD card"). A `CHOICE.DAT` from the DOS game works too. For time attack
  (the original's `REPLAYS\MAPn.ATK`: 36-byte header with the best time at +0x1c, then per drawn
  frame a 40-byte record with position, attitude and clock, `0x1b964`, `0x1ba3c`, `0x231d0`) the
  slot `ATTACK.DAT` (id 24) holds the kept run per track (`struct ghost_file`, 60,016 bytes, 360,096
  in all): `ghost_track(n)` points directly into its memory, so the ghost survives power-off like
  `CHOICE.DAT`.

## Track and car selection

As in the original (its screen 3: `0x2e384`, `0x2c078`, `0x2cb88`, `0x29530`; OpenScreamer
`docs/NOTES.md` sections 18 and 26), simplest form in `firmware/menu.c`: the six models `AMAPn.3DX`
from `MENU.DAT` stand in a row, 40,000 units apart, and rotate (pi/4 rad/s); the camera looks from
(-40,000*n, 4800, 13,000) 36 degrees down and glides to the neighbour on left/right (factor 1 -> 0 at
0.95/s, path with the fourth power, the next press counts only below 0.8). The models go through the
geometry unit like objects in the race, with `MENU.MLB` as material file (all their faces are
single-colour, routine 128); each block at its place from the position set, the farthest first.
Behind them the picture `MENU3L.PCX` from `GRAF.DAT` (its palette is the screen's), over it title
(`MENU.TXT` row 45, font 3), track name (`TRACKDAT.TXT`, font 2, 2 font heights above the bottom
edge) and the arrows (sprites 0 and 1 at x 8 and 238, y 100, 2 and 3 light up while the camera
glides in their direction and its factor is above 0.6, `0x2e2f0`) from `MENUL.FNT` and
`MENUL.SPR`, all through the blitter. B or START takes the track.

**Unlocking** as in the original (`CHOICE.DAT` +0x42c Bullet, +0x454 level, +0x458 the level already
shown; a new game has all three 0): tracks up to level + 3 are selectable (`0x287a8`: right only
while the track is below `0x20814` + 2), i.e. three at the start; each championship league won sets
the level to 1, 2, 3 (`0x290e8`, tracks 4 to 6), everything won to 9 (`0x29141`; the original also
unlocks the backward tracks 7 to 12, which the core does not have). Cars rows 0 to 11, the Bullet
(row 12) only once it is won (`0x28726`: `0x207ec` + 11); AFTERBURNER as highest difficulty likewise
(+0x45c). After the last selectable one the right arrow is missing (`0x2e32a`, `0x2e4a2`), locked
models never come into view. While the level is above the one shown, NEW TRACK AVAILABLE blinks in
the track screen (`MENU.TXT` row 53, y 0x20, font 1, colour 2, 32 of 64 ticks, `0x2e424`); leaving
the screen counts as seen (`0x28201`, `0x28260`). The original's cheats (words from `MENU.DTR`,
typed on the keyboard, `0x288dc`: TAZOR Bullet, VTELO level 3, INVER level 9, ABURN AFTERBURNER,
MONTY/JOINT obstacles, CLOCK clock stands, MIRRO picture mirrored and steering swapped, UPDOW
picture upside down) do not exist: the Pocket has no keyboard. `MENU=1 LEVEL=n` and `MENU=2
BULLET=1` for `tools/render.sh`.

Then, as in the original ("track selection, then car selection"), the car screen (its screen 2:
`0x2bdcc`, `0x2ca10`, `0x2e08c`): for each row of `CARS.DTR` (GEN.DAT) a car, its model
`AAUTOnn.3DX` from `MENU.DAT` in its paint (texture phase `0x6000`: not braking, slow), 4000 units
apart at z 2900, rotating at pi/2 rad/s; the camera from (-4000*n, 500, 4100), 22.5 degrees down,
glides as above. The cars are drawn by `car_mesh_draw` (`car.c`) like the race draws a car, with the
engine's block order; their textures are spread over all of `MENU.MLB`, which is why the core loads
the file whole. Behind them `MENU4L.PCX` with its palette, over it `CAR SELECTION` (row 44), name
(font 0, 3 font heights deep), gearbox (row 37/38, 5 heights above the edge), `MAX SPEED` with
"%d KMH", `GRIP` and `ACCELERATION` with their bars (sprites 4-8, 9-13), all from `CARDAT.TXT`.
Each car exists twice, automatic in the first paint and manual in the second, like the rows of
`CARS.DTR`; rows 0 to 11 are selectable, the Bullet only after winning the championship (see
"Unlocking"). The side views 16-28 of `MENUL.SPR` are not drawn by `0x2e08c`. B or START takes the
car, A or SELECT leads back to the track selection. The row is the player's car (model, driving
data, gearbox, dashboard in the cockpit), the opponents take the remaining rows in order
(`0x142e8`).

Then the core loads, unpacks and builds the track (a few seconds; track 1 on the Pocket: 1 s
loading, 3.2 s building, 0.6 s effects, scenery and dashboard). Meanwhile, as in the original
(`0x2a264`, before every race except after PLAY AGAIN), the loading picture stands: `LOADnL.PCX`
from `GRAF.DAT` with its own palette, over it `LOADING` (`MENU.TXT` row 47) centred in row 0x50,
font 3, colour 0 (`0x2e598`), shown at once with its palette; when everything is built it fades out
in half a second (see "Fades"; `menu_loading` in `menu.c`, `loading` in `main.c`; `MENU=3` for
`tools/render.sh`). The menu music stops when it appears: the CPU builds the track in one go and
reads from the data slots meanwhile; nobody fills the music ring in that time.

## Pause, replay, play again

**Pause:** START in the race, while it runs. The original has the CD player for it (P, `0x211d0` ->
`0x21328`): the last frame darkened twice (`0x25cc8`, the same table as the radar, 0.75), the race
stands, over it the car radio `CDFRONTL.PKC` from `GAME.DAT`, the CD's audio tracks (2-8, font 5,
colour 9, from (64, 78) at spacing 13, the chosen one blinks) and the cursor on PLAY (sprite 7 at
(59, 97)). Effects and music fade from 0x6000 at 16384/s (1.5 s), then the CD stops (`0x26458`,
MSCDEX's STOP AUDIO: a pause). Left/right (`0x214e1`) moves the cursor over seven keys (table
`0x1f1c`: x, sprite, pressed), ENTER (`0x21551`) presses: PLAY (59) leaves the player, EJECT (83)
shows GAME.TXT row 17 PLEASE EJECT DISK until the cursor moves on (DOSBox's image never ejects),
back/forward (113, 137) choose the track, - and + (167, sprites 57/58) change the displayed volume
by 48000/s times the frame time (in DOSBox 480 per press), the last (217) switches CD -> FX -> MU
(`0x1f7c`: master volumes `0x1d2bc`, `0x1d2b8`, `0x1d2c0`; stored in `CHOICE.DAT` +0x448, +0x444,
+0x44c). On these three `0x23dac` shows name and value/1024 bars (colours 22, 214, 208, ten each). A
pressed key looks pressed for 0.5 s, there are no sounds, the music never plays in the pause. ESC
leaves likewise; on leaving (`0x21982`) the volumes apply, and the chosen track plays from the start
(`0x2636c`, repeated) if back/forward changed it, otherwise the song continues where it was
(`0x26498`; the flag for this, ebp+0x4a, is set by nobody, in DOSBox a stack address stood there,
not 0). The race keeps the track; the next start plays the track's song again.

In the core likewise (B is ENTER, START is ESC, a held key repeats after 0.5 s every frame like the
keyboard in DOSBox): the CPU darkens the shown frame once through the table and stores it as a
still in the SDRAM (`still` in `main.c`), the blitter copies it every frame and puts radio, cursor
and displays over it (`hud_cd`, `pause_race`). The volumes go to effects and music (`snd_master`,
`music_master`, and MU to the FM music, `music_master_mu`).

**The replay** (from the EXE): 5 s after the end of a finished race, as soon as the commentator has
said everything he intended (`0x21048` asks `0x24740`; until then the end screen over the
track-side camera), the race runs once more; not after time up or SELECT (`0x1fe28`). The game does
not record the cars but its controls each frame (`0x22f34`), rebuilds the race (`0x1fd64`,
`0x128dc`) and reads them back instead of the keyboard (`0x22e74`); everything else it recomputes.
The core records per physics step (1/50 s) the controls `car_step` receives (`car_keys`: B, Y, left,
right held, L and R only in the step of pressing, plus the left stick lateral and both triggers of
a dock controller as raw bytes; 6 bytes, up to 20 minutes), rebuilds effects, sounds, scenery, car,
opponents and music as at the start and runs the race from the words, with as many steps per frame
as in the race.

In the replay (`0x22d98` sets the flag `0x118e68`): the track-side camera from the start
(`0x12afe`), no crash camera (`0x1857a`), the view key does not work (`0x19b53`); of the race
display only an "R" at (8, 170), font 0, colour 6, while bit 7 of the 100 Hz counter is set
(`0x2346c`; labels, countdown, lap, position, clocks and radar are omitted, OpenScreamer's notes
are wrong here), plus traffic light (`0x20d10`) and end screen (`0x21048`) as in the race.
Commentary and sounds come as in the race (the game does not query the flag there); the lap record
stays as the race left it, hence no second "new lap record". When the words are used up the replay
ends (`0x22dd8`), B, START or SELECT skip it (ENTER and ESC in the original); then PLAY AGAIN?.
Checked on the PC (`REPLAYCHECK` for `tools/render.sh`): a whole race of a driving robot on track 1
(car 6, 8391 steps, three laps) and its replay, at other frame rates and on the track-side camera,
are bit-identical in every step (player and nine opponents).

**PLAY AGAIN?:** SELECT in the race, after time up (5 s after the end, as above) and after the
replay. The original asks after every left race (`0x1fe86` -> `0x219d0`, except in the
championship and network; after a finished race only after the replay): ESC stops the effects
(`0x1f8cc`), the music continues; `0x219d0` draws the scene once more (`0x81870`, 320x200, also
from the cockpit) without race display and end screen, i.e. without position, total time, TIME OVER
and the replay's "R". The core draws a last frame for it without a physics step and without race
display, radar and dashboard (`ask` in `run_track`), the same in all cases. This frame darkened
three times, over it `GAME.TXT` rows 19-21 (PLAY AGAIN?, YES, NO) centred in rows 52, 92 and 122
(font 0), the question in colour 2, the answers in 14, the chosen one blinks to 2. Up/left YES,
down/right NO, each press with the click (see "Menu sounds"), B or START takes the answer, A or
SELECT is NO (ESC in the original). YES rebuilds the race (without rebuilding the track), NO leads
to the main menu.

## Fades and menu sounds

**Fades** (from the EXE): all through the palette, each value times f toward black (`0x27de8`;
`pal_faded` in `main.c`, into the palette of the buffer shown next). When a menu screen is left
(track -> car, car -> track, car -> race) it is black at once (`0x1d220` = 0: the palette black in
the frame of the key press; `black`), the new screen fades in at 1.5/s, in 2/3 s (`0x20374` from
`0x2c078` and `0x2bdcc`, `0x29738` every frame), models and camera already running; likewise the
main menu pages, at 0.6/s (see "Main menu and options"). There is no camera flight between track and
car screens (only when leaving the menu pages). The loading picture appears without a fade
(`0x2a264`) and fades out in half a second when the race is built, the picture only (`0x128dc(1)`
-> `0x21d8c(0)`; OpenScreamer's notes overlook this). The race fades in in half a second, picture,
effects and music (`0x21e50` every frame, 2/s); leaving (PLAY AGAIN? NO, `0x1feed`) or rebuilding
(YES, the replay: `0x128dc(0)`) fades out likewise, with the sound (`0x21d8c(1)`). The game stands
during the fade-out; the core keeps the frame loop running so that the music keeps getting data:
the still picture is copied into both buffers, each frame one gets the palette and is swapped
(`fade_out`). The pause does not fade.

**Menu sounds:** while a menu is up, the game's sample table `SOUND_FX` comes from `MENU.DTR`
(GEN.DAT: `smenu1`-`smenu9`, `MENU1.RAW`-`MENU9.RAW`; `0x29e1d`, `0x1f2d0`). In the track and car
screens only the step to the next track or car plays, row 6, `smenu7` (`0x2859e`, `0x28617`,
`0x2874b`, `0x287cd`); choosing and back are silent (the branches `0x28ef6`, `0x28172` and
`0x2821e` play nothing; OpenScreamer plays row 0 there, by its own reading). On the pages: row 1
from row to row (`0x283ca`, `0x284ba`), row 2 for a changed option (`0x28df0`), row 0 on opening a
page and on ESC back (`0x28b5f`, `0x280ec`), row 3 for NORMAL GAME (`0x28ba2`); NETWORK GAME is
silent. The core reads the nine samples at start at their places from `SOUND.DAT` (88 KB; the
whole container follows with the first track) and resets the table after every race (`snd_menu` in
`sound.c`). PLAY AGAIN? clicks on every press of up/left or down/right (`SFXSBASE.DTR` row 6,
`sclick`; `0x21b0e`, `0x21b47`). The pause's CD player is silent (`0x21328`).

## Verification tools

The simulation runs the track from `SIM_TRACK` (slot 28) with the first car at once; with
`SIM_MENU=1` (slot 27) it shows the track screen instead, with `SIM_MENU=2` the car screen, and
checks the third frame against the C models (both 0 pixels difference; the car screen needs 0.22 M
clocks, 3 ms per frame). The comparison of the race frame with the C models (`track_check`) is done
only in the simulation; on the Pocket the log says `track check: simulation only`. For
`tools/render.sh`: `TRACK=n`, `CAR=r` and `MENU=1` or `2` draw the start with car r, the track or
car screen on the PC, `MENU=4` or `5` main menu or options (`MENUKEYS="DDDDB"` presses keys
first), `PAUSE=1` or `2` the pause or the question over the start (`CD="4,1"`: cursor on key 4,
pressed; compared with DOSBox screenshots: in the radio 0 pixels difference except the blinking
number).

The six start frames (simulation, fast-forward, near chase camera, frame 2; each 0 pixels
difference to the C models; "Frame" is the whole time of frame 1, 20 ms = 1.485 M clocks):

| Track | Placements | Parts | Faces | Pixels | Frame |
|---|---|---|---|---|---|
| 1 Palm Town | 1164 | 260 | 2620 | 108,027 | 1,315,412 = 17.7 ms |
| 2 Lake Valley | 1408 | 243 | 2411 | 113,588 | 1,206,304 = 16.2 ms |
| 3 Lindburg | 1590 | 235 | 2283 | 98,100 | 1,151,696 = 15.5 ms |
| 4 Palm Town Night | 1764 | 260 | 2763 | 112,283 | 1,363,361 = 18.4 ms |
| 5 Sunbeach Hill | 2359 | 249 | 3166 | 120,056 | 1,295,826 = 17.5 ms |
| 6 Sandrock | 2073 | 265 | 2502 | 132,668 | 1,354,951 = 18.2 ms |

Tracks 5 and 6 have more placements than the 2048 `track.c` used to hold (now 2560). The table
predates routine 160; with it the track 1 start frame is 1,338,092 clocks = 18.0 ms.

## Resource usage

Measured (Quartus build with the FM music; timing holds at 74.25 MHz: setup +0.94 ns, hold
+0.11 ns; build time 10.5 min with five fitter seeds tried): 11,334 of 18,480 ALMs (61 %), 292 of
308 M10K (95 %, of which 16 the table of routine 224), 34 of 66 DSP (52 %). The FM music costs 646
ALMs, 10 M10K (the write queue 1, JTOPL's shift registers 6, its sine and exponential tables 2 and a
register 1; they could go into MLABs for logic if block RAM runs out) and 3 DSP. From build to build the
logic fluctuates by a few hundred ALMs: placement packs differently densely, and where timing is
tight the fitter rebuilds logic and registers (physical synthesis).

![Space budget in the FPGA: logic 61 %, block RAM 95 %, DSP 52 %](resources.svg)

The picture is produced by `tools/make_budget_svg.py`: the measured rows from the fitter report
(`core/src/fpga/output_files/ap_core.fit.rpt`, table "Resource Utilization by Entity"). Regenerate
after every build; the script also writes this table.

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

Block RAM is the scarce resource, and logic becomes tight too: from about 80 % utilisation it is
hard on the Cyclone V to hold the clock frequency. This shows at build time (timing analysis;
`tools/build.sh` reports it and tries further fitter seeds), not as instability on the Pocket, as
long as timing holds. The biggest items are setup unit and FPU (about 1,500 ALMs each). The setup
unit computes bit-exactly like the C model in fixed point, hence wide: a corner is 144 bits, a
polygon 576, intermediate values and products up to 65 bits. There are three arithmetic units in
series so that they work simultaneously (clipper, corners, triangles), each with one multiplier
and a fixed sequence: in front of each multiplier sits a select (47 bits wide from 28 steps in
the triangle part), behind it register fields for the intermediate values (about 1,100 bits) which
are selected again. Variable shifters for normalising before the reciprocal table and scaling the
results add to this (the setup unit has eight, 32 to 65 bits wide, the clipper three, a few hundred
ALUTs each), and products wider than a DSP block (47x18, 38x18) add their partial products in
logic. There is no depth buffer, see "Visibility without a depth buffer".

How the logic was kept small:

- The project file from Analogue's template set synthesis to speed (`OPTIMIZATION_TECHNIQUE SPEED`,
  `MUX_RESTRUCTURE OFF`); without restructuring, every wide register that is only held in many
  states of a sequence, and every partial select with too wide an index, became its own wide
  select. The core uses `BALANCED` and `MUX_RESTRUCTURE ON`: 14,046 instead of 15,646 ALMs, and
  better timing.
- Units are written so that wide registers have an enable and small selects instead of being held
  in every step, one shared adder serves all read accesses (or counters), matrix, origin and eye of
  the part and rasterizer registers 7 to 18 sit in small block RAMs, and values are narrower where
  corners lie on whole pixels. The clipper's 4-bit edge number selected from the 576-bit input like a
  shifter over 16 places (1,101 ALUTs alone); now the edge end comes from the buffer, the plane
  distance is one adder with selected operands, one shifter instead of two. Each rewrite was
  checked clock for clock against the old version with 30,000 to 200,000 random cases (the clipper
  with 90,000 random polygons) and in the simulation gave the same pixels in the same clocks.
- The setup unit's 576-bit input buffer stays in registers (in block RAM it would save 190 ALMs but
  cost 19 M10K, the scarcer resource).

VexRiscv and FPU come from a synthesis of the CPU alone for the 5CEBA4F23C8 with Quartus 18.1.1:
RV32IM with 2 x 8 KB cache 1,268-1,278 ALMs, 19 M10K, 4 DSP, Fmax about 115 MHz; with FPU
3,052-3,061 ALMs, 24 M10K, 8 DSP, Fmax 98-111 MHz (median 105). The FPU makes float calculations
13-32x faster than soft-float.
