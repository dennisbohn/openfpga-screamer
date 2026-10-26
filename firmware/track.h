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

/* CARS.DTR's rows, and per car model (AUTOnn) its wheels' places */
struct track_model { float wheelbase, axle_front, axle_rear, half_track; };
const struct car_spec *track_cars(int *n);
const struct track_model *track_models(int *n);

/* where the player starts: place and heading (forward = (sin a, -cos a)) */
void track_start(float *x, float *y, float *yaw);

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
   and roll (+ lifts the right-hand side of the picture) */
struct track_cam { float x, y, h, yaw, pitch, roll; };

/* The view from the camera, clipped to clip */
void track_draw_cam(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                    struct track_stats *st);

/* The view from path position pos (path points, 8 fraction bits), clipped to clip */
void track_draw(volatile uint8_t *fb, const struct rs_clip *clip, uint32_t pos, struct track_stats *st);
