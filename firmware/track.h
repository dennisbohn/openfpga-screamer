/*
 * One track from the game files, seen from a camera. The track is an image that track_build.c
 * builds at start-up (tools/make_track.py is the reference); the layout of the image is described
 * at the top of track.c.
 * Drawing: the CPU picks the pieces within reach and sorts them far to near (the game's painter's
 * algorithm). Each piece then goes with its place in view space to the geometry unit (geom.h). The
 * unit reads the mesh, leaves out the faces turned away and hands the others to the setup unit.
 * Besides the drawing, the file gives what lies under a point (the drivable pieces), and the game's
 * records that the car needs (markers, cars, the start).
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"
#include "geom.h"
#include "drive.h"
#include "blit.h"

/* track_open checks whether a track file is at file (its header). Then the file's blocks can be
   checked with track_block_ok() before track_init() reads the rest. track_init() returns the number
   of placements, or 0 if the file does not fit the limits. */
int track_open(const volatile uint8_t *file);
int track_init(void);

/* The track's palette, 256 x 0x00RRGGBB */
const uint32_t *track_palette(void);

/* Path points of the camera's lap: point i, counted modulo the length */
uint32_t track_path_length(void);
void track_path_point(uint32_t i, float *x, float *y, float *h);

/* What lies under the point (x, y). Returns 1 if the point is over a drivable surface, and 0 if it
   is over nothing. If h is not 0, *h gets the height of the surface. If grass is not 0, *grass
   tells whether the piece is grass (its name's fourth letter is 'e').
   The search is the game's, as openscreamer's track_under() has it:
   1. The nine grid cells round the point are searched. In each cell the drivable pieces (record
      run 0) are taken in file order, and a piece only while the point is in its first block's circle.
   2. The point is turned into the piece's frame and tried three times, 10 units further along
      both axes each time.
   3. The faces are taken in the order of the block's first face row.
   4. The first face that the point is over (the point is on one side of all its edges) answers.
      The height comes from the plane of the face, given by the face's normal. The division is cut off. */
int track_under(float x, float y, float *h, int *grass);
/* After track_under has found a height: the tilt that lays a thing heading yaw on that face, as
   the game stands an opponent (original: STARTL.EXE 0x1cb9c). It takes the thing's forward
   (fx, fy) = (sin yaw, -cos yaw). Pitch is + nose up, roll is + right side up. */
void track_under_tilt(float fx, float fy, float *pitch, float *roll);

/* MAPn.MRK: the lap's markers. slip is the cornering factor from that marker on (0 on the
   straights), flags 2 the line or a checkpoint */
struct track_marker { int32_t radius, kind; float pace, slip; int32_t flags, x, height, y; };
const struct track_marker *track_markers(int *n);

/* CARS.DTR's rows, and per car model (AUTOnn) its wheels' places and its .3DX file */
struct track_model { float wheelbase, axle_front, axle_rear, half_track; uint32_t mesh, mesh_size; };
const struct car_spec *track_cars(int *n);
const struct track_model *track_models(int *n);
const uint8_t *track_model_mesh(int k);

/* MAPn.MMP's road points (on the centre line, the road's heading there) and wall segments
   (middle, two ends, angle; the normal is (cos a, -sin a)) */
struct track_dir { float angle; int32_t x, y; };
struct track_wall { float angle; int32_t x, y, x1, y1, x2, y2; };
const struct track_dir *track_dirs(int *n);
const struct track_wall *track_walls(int *n);
/* The car's wall test and the radar use at most this many walls of the list; the rest are left out. */
#define MAX_WALLS   1024

/* The animation clock (original: STARTL.EXE 0x1fcc4) runs 0.3 a second, one turn in 3.33 s. The
   track's textures play their frames by it. track_clock advances it by steps (of 0.02 s each). */
void track_clock(uint32_t steps);
/* The texture phase that the clock gives (for a record whose +0x20 is -1: the pieces and the
   moving scenery). */
uint32_t track_phase(void);

/* Where the player starts: place and heading (forward = (sin a, -cos a)). */
void track_start(float *x, float *y, float *yaw);

/* TRACK_DATA of MAPnA.DTR: seconds added at the line and checkpoints, the bumper cam's height */
int track_bonus_time(void);
int track_camera_hi(void);

/* The opponents (MAPnOPP.SPL, OPPONENTS of the difficulty's MAPn?.DTR): opponent k drives
   spline k, the last spline is the player's line; a spline is nodes with a point (the start
   node's is a place on the grid), the next node and two handles. Returns the splines. */
struct track_opp_row { float max_spd, accel, brakes, skill, sband; };
struct track_spline_node { int32_t x, y, z, next, in_x, in_y, out_x, out_y; };
int track_opp_count(int *rows);
const struct track_opp_row *track_opp_rows(void);
const struct track_spline_node *track_opp_spline(int k, int *start, int *n);

/* The backdrop (MAPn.BCK): 512 x 256 palette indices, rows of 512 bytes, the last row twice.
   track_ground_colour is the palette index that fills the view below the backdrop (TRACK_DATA's
   color). */
const uint8_t *track_backdrop(void);
int track_ground_colour(void);

/* The race screen section (tools/make_track.py; the layout is at tb_hud_build in tb_hud.h):
   its start, the image table, the fonts, the two pictures' boxes (IMPLET_L the labels, IMPRES_L
   the outside views' rev counter; each as first image and count), and GAME.TXT's line k. */
const uint8_t *track_hud(void);
const struct blit_image *track_hud_images(int *n);
const struct blit_font *track_hud_fonts(int *n);
void track_hud_picture(int k, int *first, int *count);
const char *track_hud_text(int k);

/* How far the core draws, against the original (original: STARTL.EXE, DETAIL HI). There are two
   constants. Each is a factor on the original's distances; 1.0 gives the original's picture.
   FAR_SCALE scales these:
   - the far plane (faces are cut at 43,808 units from the camera);
   - the reach (a placement is looked at while its place is within 52,000 units);
   - the box of the game's cell window (57,433 sideways and 56,096 ahead, plus the 8192 round the
     camera), where only the cells that the window covers are looked at;
   - the reach of the props and the moving scenery.
   The window is also cut to the view box of the cell that the camera is in, which the map's data
   gives. Only the original's own distance, a factor of 1.0 or less, keeps this cut. Farther, the
   box's edge lies inside what is drawn, and buildings there come and go with the camera's cell.
   LOD_SCALE scales the distances at which a block takes the next lower level of detail. The
   original's distances are constants of its image that nothing writes (original: STARTL.EXE 0x8a504
   .. 0x8a554):
   - a placed block (its middle from the camera) takes level 1 at 12,000 units and level 2 at 16,000;
   - a car's or another moving object's block takes them at 4,000 and 7,000.
   A block with no mesh at its level is not drawn (the cars' level 2 is the body alone). 0 means no
   level of detail: every block is drawn at level 0. */
#ifndef FAR_SCALE                               /* -DFAR_SCALE=1.0f on the compiler's line, for tests, wins */
#define FAR_SCALE   2.0f
#endif
#ifndef LOD_SCALE
#define LOD_SCALE   1.2f
#endif

/* The two factors in force: the constants above, unless config.c (screamer.cfg) has set others.
   Call track_set_scales before the race's track_init. A lod_scale of 0 is no level of detail. */
void track_set_scales(float far_scale, float lod_scale);
float track_far_scale(void);
float track_lod_scale(void);

/* The game's DETAIL option, 0 LOW, 1 MID, 2 HI (the default): how deep faces are drawn.
   The track file goes with it: the L set at LOW, the H set from MID up. */
void track_detail(int level);
/* The far plane in force (units from the camera), as scenery.c needs it */
float track_far(void);

/* The block to draw for block b of a mesh (mesh points to the mesh's object header), at the level
   of detail rule `moving` (0 a placed block, 1 a car or another moving object), when the block's
   middle is dist units from the camera. The result is the block itself at level 0, or the block of
   a lower level, or 0 if the mesh has none at that level (the block is not drawn). Without the
   level of detail (LOD_SCALE 0) the result is always the block itself. */
const uint8_t *track_lod_block(const uint8_t *mesh, int b, float dist, int moving);

#define TRACK_CRC_BLOCK 0x40000         /* bytes per CRC block (tools/make_track.py) */

/* The file's blocks of 256 KB from offset 0x100 on, each with a CRC-32 in the header. track_block_ok
   tells whether block k (0 .. track_blocks() - 1) arrived as written. */
int track_blocks(void);
int track_block_ok(int k);

struct track_stats {
    uint32_t pieces;                            /* placements handed to the geometry unit */
    /* Counted by the C model. With the hardware they are 0 here, and the hardware's registers
       have them: faces drawn, triangles, faces of routine 224 left out, vertices into view
       space. */
    uint32_t faces, triangles, see_through, vertices;
    int32_t cam[3];                             /* camera X, Y, height */
    /* CPU cycles (0 on the PC): picking the pieces in view and sorting them, each piece's place
       in view space, and handing the pieces on (the hook or the C models). */
    uint32_t cyc_cull, cyc_piece, cyc_out;
    uint32_t objects;                           /* objects in view */
};

/* Where pieces go: the C models of the geometry unit, the setup unit and the rasterizer
   into fb (TRACK_MODEL), or the hook (TRACK_HW) */
#define TRACK_MODEL     0
#define TRACK_HW        1
extern int track_mode;
extern void (*track_hw_piece)(const struct gu_piece *p);

/* The track's material file (for the geometry unit's register) */
const uint8_t *track_mlb(void);

/* The projection for clip (for the setup unit's registers) */
void track_view(const struct rs_clip *clip, struct su_view *v);

/* A camera: place, heading as the car's (forward = (sin yaw, -cos yaw)), pitch (+ looks up)
   and roll (+ tips the camera's right-hand side up) */
struct track_cam { float x, y, h, yaw, pitch, roll; };

/* Adds the blitter's commands for the backdrop behind the view clip, seen from cam, to l (original:
   STARTL.EXE 0x83a68; the steps are described at the function in track.c). */
void track_backdrop_blit(struct blit_list *l, const struct rs_clip *clip, const struct track_cam *cam);

/* An object besides the track's pieces (a car, a shadow, a puff of smoke). It has its blocks in
   the order they are drawn. Each block has its own place: world = o + m . local, where the rows
   of m are the world x, y, height and its columns are the block's x, y up, z. Each block also
   has its texture phase (geom.h).
   - make: if it is set, the blocks are made only when the object comes up in the drawing.
     make(id, cam, object) fills n and b. So the CPU makes a near car while the geometry unit
     works on the scenery farther off.
   - key: what the class of its mesh adds to its key (track_class_key; a shadow -200).
   - refl: a moving object's reflection state (0: offsets 0, as a static object's).
   - refl_phase: its reflection picture (the record's +0x3c, geom.h). */
struct track_block { const uint8_t *file, *blk; float m[9], o[3]; uint32_t phase; };
struct track_cam;
/* The reflection state, from the original's record +0x24 .. +0x38: the place when last drawn
   (whole units: x, y, height), and the three float accumulators of the reflection's offsets along
   the view's axes (the original's x left, y up, z back). */
struct track_refl { int32_t prev[3]; float acc[3]; };
struct track_object {
    float x, y, h, radius;
    int n;
    const struct track_block *b;
    int (*make)(int id, const struct track_cam *cam, struct track_object *o);
    int id;
    int key;
    struct track_refl *refl;
    uint32_t refl_phase;
};

/* How fast the reflections slide. The offsets grow by k for each unit that an object moves across
   the view. The game's code has the constant at 0xc2428. It is 0.008 until the player's car sets
   it (original: STARTL.EXE 0x1813c): 0.002 in the open, 0.03 in a tunnel. */
void track_reflection_rate(float k);

/* Makes the geometry unit's piece g from block b, seen from cam with the race's lens. It is for a
   model drawn without a track (the menu's, menu.h). */
void track_block_piece(const struct track_cam *cam, const struct track_block *b, struct gu_piece *g);

/* The change of the drawing key for a mesh (an object of the UNR or a .3DX file) by its class, the
   header's last word at +0x32c (original: STARTL.EXE 0x8a8d7). The class 3 gives -6000, 1 gives
   -1500, 2 gives -800, 4 gives -200, any other class 0. */
int track_class_key(const uint8_t *obj);

/* The camera's frame as the drawing takes it: forward, right, up (unit vectors in world x, y,
   height); a block placed with the columns right, up, forward faces the screen */
void track_cam_axes(const struct track_cam *cam, float f[3], float r[3], float u[3]);

/* Draws the view from the camera, clipped to clip. The objects are sorted in among the pieces as the
   game does it (original: STARTL.EXE 0x83164, 0x8a6a8, 0x8a7b0, 0x832c0):
   - The key is -(sqrt(dist^2 >> 2) rounded, >> 2), about the distance / 8.
   - An object (a car) counts 800 nearer, which is 6400 units, so that the road it stands on never
     covers it.
   - The class of a mesh (its header's last word, +0x32c) puts it farther off: class 3 (track 1's
     lawn under the road) by 6000, class 1 by 1500, class 2 by 800, class 4 by 200 (a shadow: it is
     drawn before its car). This holds for an object as well (track_object.key).
   The order is far first. On a tie the order is that of the game: the pieces as in the file (each
   cell's scenery before its road), then the objects. Up to 64 objects (MAX_OBJS in track.c). */
void track_draw_cam(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                    struct track_object *objs, int nobj, struct track_stats *st);
