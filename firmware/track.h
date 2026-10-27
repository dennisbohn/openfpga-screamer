/*
 * One track from the game files (tools/make_track.py, data slot 3), seen from a camera.
 * The CPU picks the pieces within reach and sorts them far to near (the game's painter's
 * algorithm); each piece then goes with its place in view space to the geometry unit
 * (geom.h), which reads the mesh, leaves out the faces turned away and hands the others to
 * the setup unit. Besides the drawing: what lies under a point (the drivable pieces), and
 * the game's records the car needs (markers, cars, the start).
 */
#pragma once
#include <stdint.h>
#include "raster.h"
#include "setup.h"
#include "geom.h"
#include "drive.h"
#include "blit.h"

/* Whether a track file is at file (its header); then its blocks can be checked with
   track_block_ok() before track_init() reads the rest. track_init() returns the number of
   placements, 0 if the file does not fit the limits. */
int track_open(const volatile uint8_t *file);
int track_init(void);

/* The track's palette, 256 x 0x00RRGGBB */
const uint32_t *track_palette(void);

/* Path points of the camera's lap; point i (modulo the length) */
uint32_t track_path_length(void);
void track_path_point(uint32_t i, float *x, float *y, float *h);

/* What lies under the point (x, y): 1 and, if h, the height of the drivable surface there and
   whether that piece is grass (its name's fourth letter is 'e'); 0 over nothing. The game's
   search as openscreamer's track_under() has it: the nine grid cells round the point, each
   cell's drivable pieces (record run 0) in file order, a piece only while the point is in its
   first block's circle; the point turned into the piece's frame and tried three times, 10
   units further along both axes each time; the faces in the order of the block's first face
   row; the first face the point is over (its corners all on one side) answers, its plane
   from the face's normal, the division cut off. */
int track_under(float x, float y, float *h, int *grass);

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

/* The animation clock (0x1fcc4): + 0.3 a second, one turn in 3.33 s; the track's textures
   play their frames by it */
void track_clock(uint32_t steps);

/* where the player starts: place and heading (forward = (sin a, -cos a)) */
void track_start(float *x, float *y, float *yaw);

/* TRACK_DATA of MAPnA.DTR: seconds added at the line and checkpoints, the bumper cam's height */
int track_bonus_time(void);
int track_camera_hi(void);

/* The race screen section (tools/make_track.py): its start, the image table, the fonts, the
   two pictures' boxes (IMPLET_L the labels, IMPRES_L the outside views' rev counter: first
   image, count), GAME.TXT's line k */
const uint8_t *track_hud(void);
const struct blit_image *track_hud_images(int *n);
const struct blit_font *track_hud_fonts(int *n);
void track_hud_picture(int k, int *first, int *count);
const char *track_hud_text(int k);

/* The game's DETAIL option, 0 LOW, 1 MID, 2 HI (the default): how deep faces are drawn.
   The track file goes with it: the L set at LOW, the H set from MID up. */
void track_detail(int level);

/* The file's size and its blocks of 256 KB from offset 0x100 on, each with a CRC-32 in the
   header: whether block k (0 .. track_blocks() - 1) arrived as written */
uint32_t track_size(void);
int track_blocks(void);
int track_block_ok(int k);

struct track_stats {
    uint32_t pieces;                            /* placements handed to the geometry unit */
    /* counted by the C model (with the hardware 0 here, its registers have them): faces
       drawn, triangles, faces of routine 224 left out for now, vertices into view space */
    uint32_t faces, triangles, see_through, vertices;
    int32_t cam[3];                             /* camera X, Y, height */
    uint32_t nearest;                           /* distance to the nearest placement */
    /* CPU cycles (0 on the PC): pieces in view and sorted, each piece's place in view
       space, handing the pieces on (the hook or the C models) */
    uint32_t cyc_cull, cyc_piece, cyc_out;
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

/* An object besides the track's pieces (a car): its blocks in the order they are drawn, each
   with its own place: world = o + m . local (m rows: world x, y, height; columns: the block's
   x, y up, z), and its texture phase (geom.h) */
struct track_block { const uint8_t *file, *blk; float m[9], o[3]; uint32_t phase; };
struct track_object { float x, y, h, radius; int n; const struct track_block *b; };

/* The view from the camera, clipped to clip, with the objects sorted in among the pieces as
   the game does it (0x83164, 0x8a6a8, 0x8a7b0, 0x832c0): the key is -(sqrt(dist^2 >> 2)
   rounded, >> 2), about the distance / 8; an object (a car) counts 800 nearer, 6400 units,
   so the road it stands on never covers it; a mesh's class (its header's last word, +0x32c)
   puts it further off: 3 (track 1's lawn under the road) 6000, 1 1500, 2 800, 4 200. Far
   first; on a tie in the order the game puts them in: the pieces as in the file (each cell's
   scenery before its road), the objects after them. */
void track_draw_cam(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                    const struct track_object *objs, int nobj, struct track_stats *st);

/* The view from path position pos (path points, 8 fraction bits), clipped to clip */
void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st);
