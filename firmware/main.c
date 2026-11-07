/*
 * The main program of the core (program.bin, data slot 2). The boot loader copies it into the SDRAM and
 * starts it with the boot results (docs/soc.md). Entry point: prog_main().
 *
 * With the game's original files in data slots 3-20 it plays the game: the intro, the main menu and its
 * pages, the track and car screens, the race with its pause, replay and PLAY AGAIN? question, and the
 * championship. For a race it builds the track from the game's files (track_build.c) and puts the player's
 * car on it (car.c). Each frame the CPU picks and sorts the pieces to draw (track.c); the geometry unit,
 * setup unit and rasterizer draw them. Without all the files it needs, the screen of files.c lists them
 * as [OK] or [MISSING].
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "memtest.h"
#include "raster.h"
#include "files.h"
#include "screen.h"
#include "slot.h"
#include "track.h"
#include "track_build.h"
#include "car.h"
#include "hud.h"
#include "opp.h"
#include "ghost.h"
#include "efx.h"
#include "scenery.h"
#include "geom.h"
#include "sound.h"
#include "music.h"
#include "menu.h"
#include "dat.h"
#include "speech.h"
#include "champ.h"
#include "save.h"
#include "intro.h"
#include "config.h"
#include "racedebug.h"

/*
 * Layout of this file:
 *   - the SDRAM map and the data slots, the stack helpers and log_head
 *   - hw_piece, hw_blit and the check of a picture against the C models
 *   - fades, the pause (pause_race) and the question PLAY AGAIN? (play_again)
 *   - run_track: the race loop, one picture per pass, with the steps of a pass (race_*) above it
 *   - loading the game's files and the set-up of what a race needs (load_*, effects, scenery, sounds)
 *   - menu_screen, the loading screen, track_ready and race (a whole race with its replays)
 *   - the intro, the championship and files_loop
 *   - prog_main: the order in which the game goes through its screens
 * What the debug build measures and logs in a race (the timeline, the statistics, the marks, the benchmark)
 * is in racedebug.c; main.c calls it through racedebug.h and has no code of its own for it.
 */

uint32_t frames_shown;            /* frames swapped in so far; trap.c prints it */

#define STACK_FILL  0x57AC57ACu /* pattern in unused stack; stack_used() finds how far it was overwritten */
#define STACK_LOW   0x6000u     /* the stack may grow down to here (link.ld keeps the boot loader below) */

/* A named part of the loop of one caller (a race, a menu screen). It is inlined, so that the loop compiles as
   if it were written out in one piece. The loops are hot, and the compiler allocates their registers
   differently when a part is a call or has a struct for its variables. */
#define INLINE_STEP static inline __attribute__((always_inline))

static uint32_t results;        /* SYS_SIM bits 20..0, from the boot loader */
static uint8_t col_bg = COL_BG, col_text = COL_TEXT, col_dim = COL_DIM, col_alert = COL_ALERT;

#define TEX_FILE    ((const volatile uint8_t *)(SDRAM_BASE + 0x2100000))  /* the track image, up to 7 MB */

/*
 * The game's containers are read into the SDRAM above the track image. Their addresses and size limits are
 * those of core/data.json. game_files[] lists them in the order of struct track_files:
 *     0  MAP#H.DAT   the track's GFX container   (data slot 3 for track 1, slots 9-13 for tracks 2-6)
 *     1  GAME.DAT    data slot 4
 *     2  MAP#.DAT    the track's DATA container  (data slot 5 for track 1, slots 14-18 for tracks 2-6)
 *     3  GEN.DAT     data slot 6
 *     4  LOCAL.DAT   data slot 7
 * Entries 0 and 2 change with the track and always use the same place. Entries 1, 3 and 4 are read once.
 * The scratch memory of the track build and of the unpackers follows them, up to the program.
 */
#define GAME_SLOT   3
#define TRACK_FILES 5u          /* mask over game_files: the two containers of a track (0 and 2) */
#define FIXED_FILES 0x1Au       /* mask: the containers that stay loaded (1, 3 and 4) */
#define LOCAL_FILES 0x10u       /* mask: LOCAL.DAT (4) alone; read early for LOADING in the first picture */
/* Data slots that only the simulation offers. The testbench provides them; core/data.json does not have them,
   so on the Pocket their size is 0. */
#define IMAGE_SLOT  30          /* the finished track image, used instead of building it (load_image) */
#define RACE_SLOT   29          /* its size: the frames after the green light to race (run_track) */
#define TRACK_SLOT  28          /* its size: the track to race; without it the track of CHOICE.DAT is raced */
#define MENU_CHECK_SLOT 27      /* its size: show a menu screen, not a race (1 track screen, 2 car screen) */
static const struct game_file { const char *name; uint32_t addr, max; } game_files[5] = {
    { "MAP#H.DAT", SDRAM_BASE + 0x2800000, 0x400000 },     /* '#' stands for the track's number */
    { "GAME.DAT",  SDRAM_BASE + 0x2C00000, 0x100000 },
    { "MAP#.DAT",  SDRAM_BASE + 0x2D00000, 0x40000 },
    { "GEN.DAT",   SDRAM_BASE + 0x2D40000, 0x10000 },
    { "LOCAL.DAT", SDRAM_BASE + 0x2D50000, 0x10000 },
};
#define BUILD_SCRATCH   (SDRAM_BASE + 0x2D60000)
static struct track_files game;         /* where the containers are, as loaded */
static int track_loaded;                /* the track whose two containers are loaded; 0 for none */

/* The menu files (menu.h). Of GFX/MENU.DAT and GFX/GRAF.DAT (data slots 19 and 20) only the parts the menu
   screens need are read, each at its offset in the file. Addresses and limits are those of core/data.json.
   MENU_MEM follows them and holds their pictures, sprites and fonts. This memory lies below the sound
   effects, where nothing else writes, so the files are loaded once. */
#define MENU_SLOT   19
#define GRAF_SLOT   20
#define MENU_FILE   (SDRAM_BASE + 0x200000)
#define GRAF_FILE   (SDRAM_BASE + 0x600000)
#define MENU_MAX    0x400000
#define MENU_MEM    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0xA00000))
#define MENU_SIZE   0x80000
/* The intro's logos and font (intro.h), after the menu's memory. */
#define INTRO_MEM   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0xA80000))
#define INTRO_SIZE  0x40000
static int intro_ok;
/* The effects of MAPn.SMG (efx.h) get 64 KB right below the track image. The radar's material is at the end
   of this block, so it is within a face's reach (32 KB) of the image's MLB (the material file). */
#define EFX_SIZE    0x10000
#define EFX_MEM     ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE))
/* The cockpit's dashboard as blitter boxes (hud.h): 64 KB below the effects. It is unpacked through the
   build's scratch memory, which is free by then. */
#define DASH_SIZE   0x10000
#define DASH_MEM    ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - DASH_SIZE))
/* The meshes of the props and the moving scenery (scenery.h): 64 KB below the dashboard. */
#define SCENERY_SIZE 0x10000
#define SCENERY_MEM ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x2100000 - EFX_SIZE - DASH_SIZE - SCENERY_SIZE))
/* The sound effects container SOUND.DAT (data slot 8; address and limit as in core/data.json) takes up to
   2 MB below the props memory, which lies below the track image. The mixer plays its samples from there. */
#define SOUND_SLOT  8
#define SOUND_MEM   (SDRAM_BASE + 0x1C00000)
#define SOUND_MAX   0x200000
/* Objects in one picture: the player's car, the opponents and their shadows, the player's shadow and smoke,
   and the props and vehicles in reach. track.c draws up to 64. */
#define MAX_OBJECTS 64

/* Fills the unused stack in the boot RAM with STACK_FILL, from STACK_LOW up to 256 bytes below the stack
   pointer. stack_used() later tells how deep the stack has been used. */
static void __attribute__((noinline)) stack_fill(void)
{
    uintptr_t sp;
    __asm__ volatile ("mv %0, sp" : "=r"(sp));
    for (uint32_t *p = (uint32_t *)STACK_LOW; (uintptr_t)p < sp - 256; p++)
        *p = STACK_FILL;
}

/* The most stack that has been used so far, in bytes (the part of the boot RAM from the lowest word that
   no longer holds STACK_FILL up to the end of the RAM). */
uint32_t stack_used(void)
{
    uint32_t *p = (uint32_t *)STACK_LOW;
    while (*p == STACK_FILL)
        p++;
    return BOOTRAM_SIZE - (uint32_t)(uintptr_t)p;
}

/* Rewrites the first line of debug.txt in place (dlog.h): the program's name, the text what and the number n.
   A loop that calls it every frame (files_loop) shows after a hang how many frames were drawn. */
static void log_head(const char *what, uint32_t n)
{
    char buf[DLOG_HEAD + 16];
    fmt_udec(fmt_str(fmt_str(buf, "Screamer Pocket step 3, "), what), n);
    dlog_head(buf);
}

static uint32_t draw_buf;               /* the back buffer (0 or 1) that the draw commands go to */
static int back_wait;                   /* the blitter still copies the backdrop: the first piece must wait */

#define CHECK_BUF   ((volatile uint8_t *)(SDRAM_BASE + 0x100000))  /* the picture of the C models */
#define STILL_MEM   ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x110000))    /* the still picture of the pause and
                                                                           of PLAY AGAIN? */
#define CD_MEM      ((uint8_t *)(uintptr_t)(SDRAM_BASE + 0x120000))    /* the CD player's front (hud.h) */
#define CD_SIZE     0x10000

/* Writes the projection of a view (centre, scale, far plane) to the setup unit's registers. */
static void su_view_regs(const struct su_view *v)
{
    SU_CENTRE = RS_XY(v->cx, v->cy);
    SU_SCALE = v->scale;
    SU_FAR = (uint32_t)v->far8;
}

/* Sends one piece to the geometry unit: its place in view space and the mesh block to draw (19 words, see
   GU_PUSH in docs/soc.md). A write waits only while the unit's buffer is full. track.c calls this function
   for every piece (track_hw_piece). The first piece of a frame waits until the blitter has finished the
   backdrop, because the scene is drawn over it. */
static void hw_piece(const struct gu_piece *g)
{
    piece_time_begin();
    if (back_wait) {
        while (RS_DRAW & RS_BUSY)
            ;
        piece_time_backdrop();
        back_wait = 0;
    }
    for (int k = 0; k < 9; k++) GU_PUSH = (uint32_t)g->m[k];        /* matrix, local to view space */
    for (int k = 0; k < 3; k++) GU_PUSH = (uint32_t)g->t[k];        /* origin of the piece in view space */
    for (int k = 0; k < 3; k++) GU_PUSH = (uint32_t)g->e[k];        /* the eye in the piece's frame */
    GU_PUSH = (uint32_t)((uintptr_t)g->obj - SDRAM_BASE);           /* mesh and block: SDRAM byte addresses */
    GU_PUSH = (uint32_t)((uintptr_t)g->blk - SDRAM_BASE);
    /* Bits 2..0 and 4 are the row (face list), bit 3 marks a mirrored block of a moving part, bits 29..16 are
       the texture phase. */
    GU_PUSH = ((uint32_t)g->row & 7) | (uint32_t)(g->mirror != 0) << 3 | ((uint32_t)g->row & 8) << 1 |
              (g->phase & 0x3FFF) << 16;
    GU_PUSH = g->refl;                                              /* phase and offsets of the reflection */
    piece_time_end();
}

/* Returns the index of the entry of the 256-colour palette pal that is nearest to the colour (r, g, b), by
   the squared distance in RGB. Used to find colours for text on the track's palette. */
static uint8_t nearest(const volatile uint32_t *pal, int r, int g, int b)
{
    uint32_t best = ~0u;
    uint8_t n = 0;
    for (int i = 0; i < 256; i++) {
        int dr = (int)(pal[i] >> 16 & 255) - r, dg = (int)(pal[i] >> 8 & 255) - g,
            db = (int)(pal[i] & 255) - b;
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best) { best = d; n = (uint8_t)i; }
    }
    return n;
}

/* The lists of blitter commands that the frame in progress draws from. They are also used for the check
   against the C models. */
static const struct rs_clip track_clip = { 0, 0, FB_W, FB_H };     /* the whole screen, as the game's views */
static struct blit_list hud;            /* this frame's race screen */
static struct blit_list back;           /* this frame's backdrop */
static struct blit_list dashboard;      /* the cockpit's dashboard */

/* Sends the commands of a blitter list to the blitter (blit.v; docs/soc.md, BL_PUSH), four words each. For a
   copy or a font command, the first word is the SDRAM byte address of the source, which is computed here from
   the list's source pointer. */
static void hw_blit(const struct blit_list *l)
{
    for (int i = 0; i < l->n; i++) {
        const uint32_t *c = l->c[i];
        uint32_t kind = c[1] >> 28 & 3;
        BL_PUSH = kind == BLIT_COPY || kind == BLIT_FONT ? (uint32_t)((uintptr_t)l->src[i] - SDRAM_BASE)
                                                         : c[0];
        BL_PUSH = c[1];
        BL_PUSH = c[2];
        BL_PUSH = c[3];
    }
}

/* Compares the whole screen fb with the picture m of the C models, pixel by pixel, and writes the result to
   debug.txt. The testbench looks for the text "track check: ok". t0 is the cycle count at which the drawing
   of m began; it gives the time shown in the message. */
static void compare_log(volatile uint8_t *fb, volatile uint8_t *m, uint32_t t0)
{
    uint32_t differ = 0, first = 0;
    for (int y = track_clip.y0; y < track_clip.y1; y++)
        for (int x = track_clip.x0; x < track_clip.x1; x++)
            if (fb[y * FB_W + x] != m[y * FB_W + x] && differ++ == 0) first = (uint32_t)(y * FB_W + x);
    dlog_str("track check: ");
    dlog_str(differ ? "FAIL, " : "ok, ");
    dlog_dec((int32_t)differ);
    dlog_str(" pixels differ from the C models (");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms)");
    if (differ) {
        dlog_str(", first at x ");
        dlog_dec((int32_t)(first % FB_W));
        dlog_str(" y ");
        dlog_dec((int32_t)(first / FB_W));
        dlog_str(": ");
        dlog_hex(fb[first]);
        dlog_str(" model ");
        dlog_hex(m[first]);
    }
    dlog_char('\n');
}

/* Once per race in the simulation: checks the frame just drawn in fb against the C models of the setup unit
   and the rasterizer. The models draw the same picture into CHECK_BUF in the same order: the backdrop bk,
   the 3D view in clip, the dashboard ds (0 if there is none), the radar's window and the race screen bl.
   Then the whole screens are compared pixel by pixel (compare_log). The check does not run on the Pocket:
   the CPU's redraw would stop the race for about a second. */
static void track_check(volatile uint8_t *fb, const struct rs_clip *clip, const struct track_cam *cam,
                        struct track_object *objs, int nobj, const struct blit_list *bk,
                        const struct blit_list *ds, const struct blit_list *bl)
{
    volatile uint8_t *m = CHECK_BUF;
    struct track_stats ts;
    uint32_t t0 = rdcycle();
    blit_model(m, bk);
    track_mode = TRACK_MODEL;
    track_draw_cam(m, clip, cam, objs, nobj, &ts);
    if (ds) blit_model(m, ds);
    struct su_view v;
    struct gu_piece radar;
    struct gu_stats gst = { 0, 0, 0, 0 };
    track_view(clip, &v);
    if (efx_radar_piece(&v, &radar)) gu_draw(m, &track_clip, &v, track_mlb(), &radar, &gst);
    blit_model(m, bl);
    track_mode = TRACK_HW;
    compare_log(fb, m, t0);
}

/* Copies the picture on the screen into STILL_MEM, where the blitter can use it as a source. st is
   VID_STATUS; it tells which buffer is in front. Every pixel is darkened `times` times through the radar's
   darkening table (efx.h). The original darkens its pictures the same way (original: STARTL.EXE 0x25cc8,
   0x15cbc). */
static void still(uint32_t st, int times)
{
    const volatile uint32_t *fb = (const volatile uint32_t *)FB(st & VID_FRONT);
    const uint8_t *d = efx_darken();
    uint32_t *out = (uint32_t *)STILL_MEM;
    for (int i = 0; i < FB_W * FB_H / 4; i++) {
        uint32_t w = fb[i], o = 0;
        for (int b = 0; b < 32; b += 8) {
            uint32_t c = w >> b & 255;
            for (int k = 0; d && k < times; k++) c = d[c];
            o |= c << b;
        }
        out[i] = o;
    }
    while (!(MEM_STATUS & MEM_WRITES_IDLE))
        ;
}

/* Returns the number of steps of 1/50 s since the video frame *last_frame and sets *last_frame to the
   current one (st is VID_STATUS). The original counts time in 1/100 s, so *ticks advances by two for every
   step. */
static uint32_t still_steps(uint32_t st, uint32_t *last_frame, uint32_t *ticks)
{
    uint32_t steps = (VID_FRAME(st) - *last_frame) & 0xFFFF;
    *last_frame = VID_FRAME(st);
    *ticks += 2 * steps;
    return steps;
}

/* Draws the list l (which sits on top of the still picture) with the blitter and swaps the buffers once the
   drawing is done. */
static void still_show(const struct blit_list *l)
{
    hw_blit(l);
    while (RS_DRAW & RS_BUSY)
        ;
    VID_SWAP = 1;
}

/* Fade speeds. A fade level runs from 0 (black) to 0x8000 (the full palette). These are the amounts that a
   level changes in one step of 1/50 s. The pause fades at the original's rate of 16384 a second on a scale of
   0x6000. The other speeds are in full fades a second: 1.5 for a menu screen (original: STARTL.EXE 0x20374,
   0x2c078, 0x29738), 0.6 for the pages (0x29ea0) and 2 for a race in and out (0x21e50, 0x21d8c). */
#define FADE_STEP   437         /* the pause: 1.5 s from 0x8000 to 0 */
#define FADE_MENU   983         /* a menu screen from black */
#define FADE_PAGES  393         /* the pages from black */
#define FADE_RACE   1311        /* a race in and out */

/* Sets the back buffer's palette to pal at the fade level f (0x8000 is the full palette, 0 is black), as
   the original does (original: STARTL.EXE 0x27de8). */
static void pal_faded(const uint32_t *pal, int32_t f)
{
    uint32_t k = f < 0 ? 0 : f > 0x8000 ? 0x8000 : (uint32_t)f;
    for (int i = 0; i < 256; i++) {
        uint32_t c = pal[i];
        VID_PAL[i] = ((c >> 16 & 255) * k >> 15) << 16 | ((c >> 8 & 255) * k >> 15) << 8 |
                     (c & 255) * k >> 15;
    }
}

/* Starts the next frame of any loop of the game (the menus, the intro, a race, its pause, PLAY AGAIN?, a
   fade). It waits until the swap of the previous picture is done and returns VID_STATUS. */
static uint32_t next_frame(void)
{
    return wait_swap_done();
}

/* A cut to black: swaps in the other buffer with an all-black palette. A menu screen is left this way: the
   original's palette is black in the frame of the key press (original: STARTL.EXE 0x1d220 = 0). */
static void black(void)
{
    wait_swap_done();
    for (int i = 0; i < 256; i++) VID_PAL[i] = 0;
    VID_SWAP = 1;
}

/* Fades the picture on the screen to black in half a second (original: STARTL.EXE 0x21d8c). pal is the
   palette of the picture. The picture is copied into the other buffer. Then each frame one of the two buffers
   gets the faded palette and is swapped in. If sound is set, the effects and the music fade down with the
   picture. The music is kept fed meanwhile. */
static void fade_out(const uint32_t *pal, int sound)
{
    uint32_t st = wait_swap_done(), last_frame = VID_FRAME(st);
    const volatile uint32_t *from = (const volatile uint32_t *)FB(st & VID_FRONT);
    volatile uint32_t *to = (volatile uint32_t *)FB(!(st & VID_FRONT));
    for (int i = 0; i < FB_W * FB_H / 4; i++) to[i] = from[i];
    for (int32_t f = 0x8000; f > 0;) {
        st = next_frame();
        music_frame();
        f -= (int32_t)((VID_FRAME(st) - last_frame) & 0xFFFF) * FADE_RACE;
        last_frame = VID_FRAME(st);
        pal_faded(pal, f);
        if (sound) {
            snd_fade(f);
            music_fade(f);
        }
        VID_SWAP = 1;
    }
}

/* Sets the master volumes of the effects and the music from the values that CHOICE.DAT keeps (original:
   STARTL.EXE 0x2774d): [0] digital (0x1d2b8) for the effects, [1] CD (0x1d2bc) for the CD music and
   [2] MIDI (0x1d2c0) for the FM music. */
static void volumes(void)
{
    snd_master(choice.volume[0]);
    music_master(choice.volume[1]);
    music_master_mu(choice.volume[2]);
}

/* Constants of the CD player's controls (pause_race). */
#define CD_HOLD     25          /* a key held for this many steps (0.5 s) repeats on every step after that */
#define CD_PRESSED  25          /* a button looks pressed for 0.5 s after B (original: STARTL.EXE 0x218f8) */
#define CD_VOL_STEP 480         /* the volume change of one press of - or + */
/* CD_HOLD: the original takes every repeat of the keyboard as a new press (original: STARTL.EXE 0x101b4
   clears the key). In DOSBox the repeats come over 30 times a second. Holding - for 2 s took the volume
   from 24096 to 0.
   CD_VOL_STEP: the original changes the volume by 48000 a second of the frame's time, but by at least
   0.01 s of it. The CD player's frames are so quick in DOSBox that this minimum applies, and one press
   changes the volume by 480 there. */

/*
 * The pause: the game's CD player (original: STARTL.EXE 0x21328; it is opened by P in a race, 0x211d0). It is
 * drawn over the picture on the screen, darkened twice (hud.h). The race stands still.
 *
 * The effects and the music fade out at the original's rate of 16384 a second on its scale of 0x6000. Then
 * the music stops (0x26458, a pause). Left and right move the cursor over the seven buttons (0x214e1). B
 * presses the button under the cursor (the original's ENTER, 0x21551):
 *     0  PLAY      leaves, if the disc has more than one track
 *     1  EJECT     shows GAME.TXT's PLEASE EJECT DISK until the cursor moves (DOSBox's disc never leaves)
 *     2, 3  previous and next: choose the track of the disc, from the first to the last
 *     4, 5  - and +: turn the volume that is shown down and up
 *     6     shows the next of the volumes CD, FX and MU (0x1f7c)
 * The volumes are those of CHOICE.DAT. They are heard once the race goes on. START leaves too (the original's
 * ESC).
 *
 * On leaving (0x21982) the volumes come back. If previous or next has changed the track, the chosen track
 * plays from its start, looped (0x2636c). Otherwise the song goes on where it stopped (0x26498). The flag
 * that chooses between the two (ebp+0x4a) is never set in the original, but it held a stack address in
 * DOSBox, not 0. The race keeps the chosen track; the start of the next race plays its own song again.
 * st is VID_STATUS of the frame in which the pause begins.
 */
static void pause_race(uint32_t st)
{
    static const uint8_t vol_of[3] = { 1, 0, 2 };   /* CD, FX, MU: their index in choice.volume (0x1f7c) */
    struct cd_player c = { 0 };
    c.track = music_disc(&c.first, &c.last);
    if (c.track < c.first || c.track > c.last) c.track = c.first;     /* 0x21398 */
    uint32_t keys_before = in_keys(), last_frame = VID_FRAME(st), ticks = 0, held = 0;
    int32_t fade = 0x8000;
    int changed = 0;
    still(st, 2);
    for (;;) {
        st = next_frame();
        uint32_t keys = in_keys(), press = keys & ~keys_before;
        music_frame();
        uint32_t steps = still_steps(st, &last_frame, &ticks);

        /* A key that stays down for CD_HOLD steps repeats like the keyboard's auto-repeat. */
        held = keys == keys_before ? held + steps : 0;
        if (held >= CD_HOLD) press |= keys & (KEY_LEFT | KEY_RIGHT | KEY_B);
        keys_before = keys;
        if (press & KEY_START) break;

        /* Left and right move the cursor along the buttons; a new button is not pressed. */
        if (press & (KEY_LEFT | KEY_RIGHT)) {
            int b = c.button + ((press & KEY_RIGHT) && c.button < 6) - ((press & KEY_LEFT) && c.button > 0);
            if (b != c.button) c.pressed = c.eject = 0;
            c.button = b;
        }

        /* B presses the button under the cursor. v is the volume that the buttons - and + change. */
        if (press & KEY_B) {
            int32_t *v = &choice.volume[vol_of[c.chan]];
            c.pressed = CD_PRESSED;
            if (c.button == 0 && c.last > c.first) break;
            if (c.button == 1) c.eject = 1;
            if ((c.button == 2 && c.track > c.first) || (c.button == 3 && c.track < c.last)) {
                c.track += c.button == 2 ? -1 : 1;
                changed = 1;
            }
            if (c.button == 4) *v = *v > CD_VOL_STEP ? *v - CD_VOL_STEP : 0;
            if (c.button == 5) *v = *v < 0x7FFF - CD_VOL_STEP ? *v + CD_VOL_STEP : 0x7FFF;
            if (c.button == 6) c.chan = (c.chan + 1) % 3;
        }

        /* Draw the player over the still picture. */
        c.blink = (ticks & 0x20) != 0;
        c.vol = choice.volume[vol_of[c.chan]];
        draw_buf = !(st & VID_FRONT);
        hud_cd(&hud, (int)draw_buf, STILL_MEM, &c);
        still_show(&hud);
        c.pressed = c.pressed > (int)steps ? c.pressed - (int)steps : 0;

        /* The sound fades out first; at silence the music is paused. */
        if (fade > 0) {
            fade -= (int32_t)steps * FADE_STEP;
            if (fade <= 0) {
                fade = 0;
                music_pause();
            }
            snd_fade(fade);
            music_fade(fade);
        }
    }
    volumes();
    snd_fade(0x8000);
    music_fade(0x8000);
    if (changed) music_cd(c.track);
    else music_resume();
}

/*
 * The question PLAY AGAIN?, as the original asks it when a race is left (original: STARTL.EXE 0x219d0, called
 * from 0x1fe86). The effects stop (0x1f8cc); the music goes on. The question is drawn over the picture on the
 * screen, darkened three times (hud.h). run_track has drawn that picture without the race screen and the
 * end's texts, because the original draws the scene anew (0x81870) and nothing over it.
 * Up or left means YES, down or right means NO. Each press plays a click (SFXSBASE.DTR's row 6, 0x21b0e,
 * 0x21b47). B or START takes the answer, A or SELECT means NO (the original's ESC).
 * st is VID_STATUS of the frame in which the question starts. Returns 1 for YES.
 */
static int play_again(uint32_t st)
{
    uint32_t keys_before = in_keys(), last_frame = VID_FRAME(st), ticks = 0;
    int no = 0;
    AU_STOP = 0xFFFFFFFFu;
    still(st, 3);
    for (;;) {
        st = next_frame();
        uint32_t keys = in_keys(), press = keys & ~keys_before;
        music_frame();
        if (press & (KEY_UP | KEY_LEFT | KEY_DOWN | KEY_RIGHT)) snd_play(SND_CLICK);
        if (press & (KEY_UP | KEY_LEFT)) no = 0;
        if (press & (KEY_DOWN | KEY_RIGHT)) no = 1;
        if (press & (KEY_B | KEY_START)) return !no;
        if (press & (KEY_A | KEY_SELECT)) return 0;
        keys_before = keys;
        still_steps(st, &last_frame, &ticks);
        draw_buf = !(st & VID_FRONT);
        hud_question(&hud, (int)draw_buf, STILL_MEM, no, (ticks & 0x10) != 0);
        still_show(&hud);
    }
}

static int dashboard_init(void);

/* The end screen shows for 5 s, then the replay or PLAY AGAIN? follows (original: STARTL.EXE 0x21048). */
#define OVER_STEPS  250
#define REC_MAX     60000       /* the most controls the replay can hold: 20 minutes of steps */

enum { RACE_LEAVE, RACE_AGAIN, RACE_REPLAY };   /* how a race ends: NO, YES, its replay next */

/* The controls of the race, one record for each step (from car_keys()). The original records its controls in
   every frame (original: STARTL.EXE 0x22f34). rec_n records are stored. rec_full is set when one more did not
   fit; such a race has no replay. */
static struct car_input rec[REC_MAX];
static uint32_t rec_n;
static int rec_full;

/*
 * The steps of a pass of the race loop. They are inlined into run_track (INLINE_STEP). The parts that make
 * decisions (the keys of the pause and of the end, the physics steps, the set-up of the units) stay in
 * run_track itself.
 */

/*
 * What the steps of a pass hand to one another. The members of the first group are set before the first
 * pass, those of the second group are made in each pass.
 *
 * The state that lives from one pass to the next (fade, drawn, ask and the other variables of run_track) is
 * not part of it. It stays in local variables, because as members of a struct it changes how the compiler
 * allocates the registers of the loop.
 */
struct race_frame {
    const uint32_t *pal;                /* the track's palette */
    int dash_ok;                        /* the player's car has a dashboard for the cockpit view */
    uint32_t sim;                       /* the simulation runs the core */
    /* The simulation's race (tools/sim.sh: SIM_RACE_FRAMES, and the keys in SIM_KEYS). The testbench offers
       data slot RACE_SLOT with the size sim_race. core/data.json does not have this slot, so on the Pocket
       sim_race is 0 and the race is an ordinary one. With sim_race set, the countdown of the start lights
       runs at once before the first picture, and the sim_race-th frame after the green light is checked and
       reported instead of the third. */
    uint32_t sim_race;
    uint32_t check_at;                  /* the picture that is checked against the C models (on the Pocket:
                                           only logged) */
    uint32_t keys, joy, trig;           /* the buttons, the stick and the triggers */
    uint32_t press;                     /* the buttons that went down since the last physics step */
    uint32_t t_frame;                   /* the cycle count at the start of the pass */
    int nobj;                           /* the objects in view */
    int cockpit;                        /* the 3D view stops short of the dashboard */
    volatile uint8_t *fb;               /* the buffer that is drawn */
};

/* The simulation ends one frame after the checked one. It reports its result, then the camera and the car of
   the checked frame, which are set against the original's (tools/dosram.py cam). Places are in game units,
   angles in 1/10000 radian. */
INLINE_STEP void sim_report(void)
{
    SYS_SIM = SIM_DONE | results;
    dlog_str("SYS_SIM=");
    dlog_hex(SIM_DONE | results);
    struct track_cam c;
    const struct car_state *s = car_state();
    car_camera(&c);
    const float v[10] = { c.x, c.y, c.h, c.yaw * 1e4f, c.pitch * 1e4f, c.roll * 1e4f,
                          s->x, s->y, s->z, s->yaw * 1e4f };
    dlog_str("\ncamera x y h yaw pitch roll, car x y z yaw:");
    for (int i = 0; i < 10; i++) {
        dlog_char(' ');
        dlog_dec((int32_t)v[i]);
    }
    dlog_char('\n');
}

/* Sets up the race on track n with the car of CARS.DTR row car: the ghost, the cars, the view of the car, the
   dashboard, and the hook that hands the pieces to the geometry unit. Returns 1 if the dashboard is there. */
INLINE_STEP int race_enter(int n, int car, int replay, int car_v)
{
    ghost_init(n, car);
    car_init(car, replay);
    opp_init(car);
    car_view(car_v);
    int dash_ok = dashboard_init();
    track_mode = TRACK_HW;
    track_hw_piece = hw_piece;
    GU_MLB = (uint32_t)((uintptr_t)track_mlb() - SDRAM_BASE);
    GU_INVAL = 1;
    return dash_ok;
}

/* Stage 1, input: waits for the previous swap, reads the keys and keeps the music fed. Returns VID_STATUS of
   the new pass. In the benchmark (debug build) the gas is held and no controller is read. */
INLINE_STEP uint32_t race_input(struct race_frame *f, uint32_t drawn)
{
    uint32_t st = next_frame();
    f->keys = race_dbg.bench_frames ? KEY_B : IN_KEY;
    f->t_frame = rdcycle();
    f->joy = race_dbg.bench_frames ? 0 : IN_JOY;
    f->trig = race_dbg.bench_frames ? 0 : IN_TRIG;
    race_debug_frame(f->t_frame, drawn);
    music_frame();                          /* the music ring is kept filled (music.h) */
    tl_mark(TL_MUSIC);
    frames_shown = drawn;
    return st;
}

/* Stage 2, physics: the race fades in from black, picture and sound. fade is the level, 0 to 0x8000, and
   steps_now the physics steps of this pass. Returns the new level. */
INLINE_STEP int32_t race_fade_in(int32_t fade, uint32_t steps_now)
{
    if (fade < 0x8000) {
        fade += (int32_t)steps_now * FADE_RACE;
        snd_fade(fade);
        music_fade(fade);
    }
    return fade;
}

/* Stage 3, camera: places the camera in cam and collects the objects (cars, shadows, smoke, props) to draw
   in objs. */
INLINE_STEP void race_objects(struct race_frame *f, struct track_cam *cam, struct track_object *objs)
{
    car_camera(cam);
    race_debug_camera(cam, f->keys);
    int nobj = car_object(cam, &objs[0]);
    nobj += opp_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += ghost_objects(objs + nobj, MAX_OBJECTS - nobj);
    nobj += car_effects(cam, objs + nobj, MAX_OBJECTS - nobj);
    nobj += scenery_objects(cam, objs + nobj, MAX_OBJECTS - nobj);
    f->nobj = nobj;
}

/* Stage 4, scene, first part: the backdrop (original: STARTL.EXE 0x83a68) and the palette. The blitter copies
   the backdrop while the CPU culls the scene. The first piece for the geometry unit waits until the blitter
   is done (hw_piece). st is VID_STATUS of the pass, fade the fade level. */
INLINE_STEP void race_backdrop(struct race_frame *f, uint32_t st, int32_t fade, const struct rs_clip *clip,
                             struct track_cam *cam)
{
    f->fb = FB(!(st & VID_FRONT));
    draw_buf = !(st & VID_FRONT);
    blit_start(&back, (int)draw_buf);
    track_backdrop_blit(&back, clip, cam);
    race_debug_backdrop();
    back_wait = 1;
    hw_blit(&back);
    pal_faded(f->pal, fade);
}

/* Stage 4, scene, second part: the pieces go from the CPU to the geometry unit, their faces to the setup unit
   and the triangles to the rasterizer. ts gets what the scene came to. */
INLINE_STEP void race_draw(struct race_frame *f, struct track_object *objs, struct track_cam *cam,
                         const struct rs_clip *clip, struct track_stats *ts)
{
    race_debug_scene_begin();
    track_draw_cam(0, clip, cam, objs, f->nobj, ts);
    race_debug_scene_end();
}

/* Stage 5, race screen with the cockpit: the dashboard comes before the radar window, which darkens it too.
   The blitter draws the dashboard once the scene is done, and the CPU makes the race screen's list
   meanwhile. Then the CPU waits for both and widens the clip to the whole screen, so that the window is
   clipped to it. radar is the radar's piece; it is drawn if has_radar is set. */
INLINE_STEP void race_cockpit_screen(int has_radar, struct gu_piece *radar)
{
    hud_dash(&dashboard, (int)draw_buf);
    hw_blit(&dashboard);
    hud_build(&hud, (int)draw_buf, 1);
    while (RS_DRAW & RS_BUSY)
        ;
    RS_CLIP1 = RS_XY(track_clip.x1, track_clip.y1);
    if (has_radar) hw_piece(radar);
}

/* Stage 5, race screen without the cockpit: the radar window, then the list of the instruments. The question
   PLAY AGAIN? (ask) has no race screen. */
INLINE_STEP void race_plain_screen(int ask, int has_radar, struct gu_piece *radar)
{
    if (has_radar) hw_piece(radar);
    if (ask) blit_start(&hud, (int)draw_buf);                   /* the question has no race screen */
    else hud_build(&hud, (int)draw_buf, car_overlay() ? 2 : 0); /* no dashboard: the instruments */
}

/* Stage 6, frame end: waits until the rasterizer has drawn everything, lets the simulation check the picture
   against the C models, and hands the picture to the statistics of the debug build. */
INLINE_STEP void race_finish(struct race_frame *f, uint32_t st, int ask, uint32_t drawn, uint32_t steps_now,
                           const struct rs_clip *clip, struct track_cam *cam, struct track_object *objs,
                           struct track_stats *ts)
{
    while (RS_DRAW & RS_BUSY)
        ;
    race_debug_measure(ts);
    if (f->sim && drawn == f->check_at)
        track_check(f->fb, clip, cam, objs, f->nobj, &back, f->cockpit ? &dashboard : 0, &hud);
    race_debug_report(st, f->fb, ask, steps_now, ts);
}

/*
 * The race on track n with the car of CARS.DTR row car. The opponents take the other rows. replay is 0 for a
 * race and 1 for the replay of a finished race. Returns how the race ended: RACE_LEAVE, RACE_AGAIN (YES to
 * PLAY AGAIN?) or RACE_REPLAY (run the race again from its recorded controls).
 *
 * How a race goes on:
 *   - START pauses it (pause_race). SELECT asks PLAY AGAIN?.
 *   - When the race has ended, the trackside camera takes over (car.h). OVER_STEPS steps later, and once the
 *     commentary has said all it queued, the race is over (original: STARTL.EXE 0x21048 asks 0x24740). This
 *     never happens in the simulation, which stops after the frame it checks.
 *   - A finished race of the normal game then runs again from its controls (RACE_REPLAY, 0x1fe45). Time over
 *     and SELECT have no replay (0x1fe28).
 *   - The replay (replay set) is the game's replay (0x1fd64, 0x22e74). It runs until its controls run out
 *     (0x22dd8) or B, START or SELECT skip it (the original's ENTER and ESC).
 *   - Then PLAY AGAIN? is asked, over one more picture of the scene alone, on the whole screen (ask):
 *     RACE_AGAIN for YES, RACE_LEAVE for NO.
 * The race fades in from black, picture and sound, in half a second (0x21e50 in each frame).
 *
 * Each pass of the loop makes one picture, in six stages that are marked in the code (the steps above are
 * called race_...):
 *   1. input        wait for the previous swap, read the keys, handle the pause, the replay and the end
 *   2. physics      run the car, the opponents and the scenery for the steps since the last picture
 *   3. camera       place the camera and collect the objects to draw
 *   4. scene        draw the backdrop, then the 3D view piece by piece
 *   5. race screen  the radar, the dashboard and the instruments (for the question: nothing)
 *   6. frame end    wait for the rasterizer, check or log (debug build), swap
 */
static int run_track(int n, int car, int replay)
{
    struct race_frame f;
    f.pal = track_palette();
    int32_t fade = 0;                           /* fade-in level, 0 to 0x8000 */
    snd_fade(0);
    music_fade(0);
    uint32_t drawn = 0, keys_before = replay ? IN_KEY : 0, last_frame = 0, over = 0;
    int ask = 0;                                /* the question is due: this pass draws, the next asks */
    uint32_t rec_at = 0;                        /* the replay's next record */
    int car_v = CAR_VIEW_START;
    if (!replay) {
        rec_n = 0;
        rec_full = 0;
    }
    f.dash_ok = race_enter(n, car, replay, car_v);
    f.sim = SYS_FLAGS & FLAG_SIM;
    f.sim_race = f.sim ? slot_size(RACE_SLOT) : 0;
    f.check_at = 2 + f.sim_race;
    race_debug_start(n, f.sim != 0, f.sim_race, f.check_at, col_text, col_bg);
    for (;;) {
        uint32_t st = race_input(&f, drawn);
        if (race_dbg.bench_frames && drawn > race_dbg.bench_frames)
            return RACE_LEAVE;                  /* the benchmark has its pictures */
        if (f.sim && drawn == f.check_at + 1) sim_report();
        f.press = f.keys & ~keys_before;        /* the keys that went down since the last step */
        f.press = race_debug_keys(f.keys, f.press, car_v);
        /* The question PLAY AGAIN? was announced in the last pass, which drew its picture. */
        if (ask) return play_again(st);
        if (replay) {
            /* The replay ends when B, START or SELECT skip it, or when its controls run out. */
            if ((f.press & (KEY_B | KEY_START | KEY_SELECT)) || rec_at >= rec_n) ask = 1;
        } else {
            /* START pauses the race, but not before its first picture or after its end. The keys held at the
               end of the pause do not count as presses, and the time of the pause is not made up by physics
               steps. The pass ends at once, without a picture. */
            if ((f.press & KEY_START) && drawn && !car_race()->finished && !car_race()->game_over) {
                pause_race(st);
                keys_before = IN_KEY;
                last_frame = VID_FRAME(VID_STATUS);
                continue;
            }
            if (f.press & KEY_SELECT) ask = 1;
            /* The end screen has been up for OVER_STEPS steps and the commentary has finished. A finished
               race of the normal game is replayed (original: STARTL.EXE 0x1fe45). A finished race of the
               championship goes on by itself (0x1fe86). In all other cases the question follows. */
            if (over >= OVER_STEPS && !speech_busy()) {
                if (car_race()->finished && !rec_full && (car_mode & CAR_NORMAL)) return RACE_REPLAY;
                if (car_race()->finished && car_mode == CAR_CHAMP) return RACE_LEAVE;
                ask = 1;
            }
        }

        /* Stage 2, physics. It runs at 50 steps a second: one step for each video frame since the last
           picture, but none for the picture of the question, one for the first picture, and at most 4 at
           once. The race stands still while the free camera is on. */
        uint32_t steps_now = ask ? 0 : drawn ? (VID_FRAME(st) - last_frame) & 0xFFFF : 1;
        last_frame = VID_FRAME(st);
        if (steps_now > 4) steps_now = 4;
        if (race_dbg.free_cam) steps_now = 0;
        fade = race_fade_in(fade, steps_now);
        if (f.press & KEY_X) car_view(++car_v);                         /* X changes the view */
        efx_cyc_objects = efx_cyc_radar = 0;    /* efx.c adds its cycles here; the debug log reads them */
        race_debug_physics_begin();
        for (uint32_t k = 0; k < steps_now && (!replay || rec_at < rec_n); k++) {
            struct car_input w;                 /* the controls of this step */
            if (replay)
                w = rec[rec_at++];              /* taken from the recording */
            else {
                w = car_keys(f.keys & ~race_dbg.swallow, keys_before & ~race_dbg.swallow, f.joy, f.trig);
                if (rec_n < REC_MAX) rec[rec_n++] = w;                  /* and recorded for the replay */
                else rec_full = 1;
            }
            car_step(&w);
            opp_step();
            scenery_step();
            keys_before = f.keys;
            /* In the simulation's race and in the benchmark the countdown of the start lights runs at once:
               one more step is added until the race is on. */
            if ((f.sim_race || race_dbg.bench_frames) && !car_race()->on && k == steps_now - 1) steps_now++;
            if (car_race()->finished || car_race()->game_over) over++;  /* steps since the end of the race */
        }
        if (race_dbg.free_cam) keys_before = f.keys;    /* the loop keeps keys_before, but it did not run */
        race_debug_physics_end(steps_now);
        track_clock(steps_now);                 /* the animation clock of the track's textures */

        /* Stage 3, camera: the objects to draw, then the 3D view. */
        struct track_cam cam;
        static struct track_object objs[MAX_OBJECTS];
        race_objects(&f, &cam, objs);
        /* The 3D view is the whole screen. In the cockpit it is the first 132 lines (car_view_lines), with
           the eye line 85 lines down (track_view), and the dashboard covers the rest. Without a dashboard,
           and for the question, the view is the whole screen of 320 x 200 (original: STARTL.EXE 0x219d0). */
        f.cockpit = f.dash_ok && car_cockpit() && !ask;
        const struct rs_clip clip = { 0, 0, FB_W, f.cockpit ? car_view_lines() : FB_H };
        struct su_view view;
        track_view(&clip, &view);

        /* Stage 4, scene: the backdrop, the set-up of the units, the pieces. */
        race_backdrop(&f, st, fade, &clip, &cam);
        struct track_stats ts;
        GU_MODE = draw_buf << 8;                /* the units draw into the back buffer */
        RS_CLIP0 = RS_XY(clip.x0, clip.y0);
        RS_CLIP1 = RS_XY(clip.x1, clip.y1);
        su_view_regs(&view);
        race_draw(&f, objs, &cam, &clip, &ts);

        /* Stage 5, race screen. The radar is a darkened window over the scene (efx.h). The replay and the
           question have none (original: STARTL.EXE 0x2346c). The race screen is a list for the blitter, which
           draws it once the scene is done. */
        struct gu_piece radar;
        int has_radar = !replay && !ask && efx_radar_piece(&view, &radar);
        race_debug_screen_begin();
        if (f.cockpit)
            race_cockpit_screen(has_radar, &radar);
        else
            race_plain_screen(ask, has_radar, &radar);
        hw_blit(&hud);
        race_debug_screen_end();

        /* Stage 6, frame end. */
        race_finish(&f, st, ask, drawn, steps_now, &clip, &cam, objs, &ts);
        VID_SWAP = 1;                           /* show the finished picture */
        log_race_head(++drawn);
    }
}

/* Reads the whole file of data slot id to addr, if it is at most max bytes. Returns its size from the slot
   table, or 0 if the slot has no file, the file is too big or a read failed. *err gets the APF's result. The
   reads stay inside the file: first its whole words, then the last 1 to 3 bytes as the rest of the file. */
static uint32_t load_file(uint32_t id, uint32_t addr, uint32_t max, int *err)
{
    uint32_t size = slot_size(id), body = size & ~3u;
    *err = 0;
    if (size == 0 || size > max) return 0;
    if (body && (*err = slot_read(id, 0, addr, body)) != 0) return 0;
    if (size > body && (*err = slot_read(id, body, addr + body, SLOT_REST)) != 0) return 0;
    return size;
}

/* Copies name into buf and replaces '#' by the digit of track n. Returns buf. */
static const char *track_name(const char *name, int n, char buf[16])
{
    int i = 0;
    for (; name[i] && i < 15; i++)
        buf[i] = name[i] == '#' ? (char)('0' + n) : name[i];
    buf[i] = 0;
    return buf;
}

/* Loads the containers of game_files into the SDRAM. Bit k of which selects game_files[k]. The track's
   containers (0 and 2) are those of track n. The data slots are GAME_SLOT + k, except for tracks 2 to 6:
   MAP2H to MAP6H are in slots 9 to 13 and MAP2 to MAP6 in slots 14 to 18. Fills in the struct game and
   writes one line to debug.txt. Returns 1 if all selected containers are there. */
static int load_containers(int n, uint32_t which)
{
    const uint8_t **at[5] = { &game.gfx, &game.game, &game.data, &game.gen, &game.local };
    uint32_t *size[5] = { &game.gfx_size, &game.game_size, &game.data_size, &game.gen_size,
                          &game.local_size };
    uint32_t t0 = rdcycle();
    char name[16];
    dlog_str("game files:");
    for (int k = 0; k < 5; k++) {
        if (!(which >> k & 1)) continue;
        const struct game_file *g = &game_files[k];
        uint32_t id = n > 1 && k == 0 ? 7u + (uint32_t)n
                    : n > 1 && k == 2 ? 12u + (uint32_t)n
                    : GAME_SLOT + (uint32_t)k;
        int err;
        *size[k] = load_file(id, g->addr, g->max, &err);
        *at[k] = (const uint8_t *)(uintptr_t)g->addr;
        dlog_char(' ');
        dlog_str(track_name(g->name, n, name));
        if (!*size[k]) {
            dlog_str(" not loaded (data slot ");
            dlog_dec((int32_t)id);
            dlog_str(", size ");
            dlog_dec((int32_t)slot_size(id));
            dlog_str(", result ");
            dlog_dec(err);
            dlog_str(")\n");
            return 0;
        }
        dlog_char(' ');
        dlog_dec((int32_t)*size[k]);
    }
    dlog_str(" bytes loaded in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return 1;
}

/* Reads the bytes from..to of the file of data slot id (size bytes) to the same offsets from addr. It reads
   whole words, and the last 1 to 3 bytes of the file as the rest of the file, if to reaches them. Returns 0
   if that worked, else the APF's result. */
static int load_range(uint32_t id, uint32_t size, uint32_t from, uint32_t to, uint32_t addr)
{
    uint32_t body = size & ~3u, a = from & ~3u, b = (to + 3) & ~3u;
    if (b > body) b = body;
    int err = a < b ? slot_read(id, a, addr + a, b - a) : 0;
    if (!err && to > body) err = slot_read(id, body, addr + body, SLOT_REST);
    return err;
}

/* Reads parts of the container of data slot id (at most max bytes): its directory and the parts that the
   list p names. Each is read to its own offset from addr, as if the whole file were there. Of each part at
   most p->most bytes are read. The list ends with an entry without a name. Returns the file's size, or 0 if
   the file or a part is missing or a read failed. */
static uint32_t load_parts(uint32_t id, uint32_t addr, uint32_t max, const struct menu_part *p)
{
    const uint8_t *f = (const uint8_t *)(uintptr_t)addr;
    uint32_t size = slot_size(id);
    if (size < 8 || size > max || load_range(id, size, size - 4, size, addr)) return 0;
    const uint8_t *q = f + size - 4;            /* the last word is the size of the directory before it */
    uint32_t dir = q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    if (dir > size - 4 || load_range(id, size, size - 4 - dir, size - 4, addr)) return 0;
    for (; p->name; p++) {
        struct dat_entry e;
        if (!dat_find(f, size, p->name, &e) || e.offset > size || e.stored > size - e.offset) return 0;
        if (load_range(id, size, e.offset, e.offset + (e.stored < p->most ? e.stored : p->most), addr))
            return 0;
    }
    return size;
}

/* Loads the files of the menu screens (menu.h) and of the intro (intro.h) and sets both up. The needed parts
   of GFX/MENU.DAT and GFX/GRAF.DAT are read here; DATA/LOCAL.DAT and DATA/GEN.DAT are loaded already.
   Returns 1 if the menus are there. intro_ok tells whether the intro is there too. */
static int menus(void)
{
    uint32_t t0 = rdcycle();
    uint32_t menu_size = load_parts(MENU_SLOT, MENU_FILE, MENU_MAX, menu_dat_parts);
    uint32_t graf_size = load_parts(GRAF_SLOT, GRAF_FILE, MENU_MAX, menu_graf_parts);
    int ok = menu_size && graf_size &&
             menu_init((const uint8_t *)(uintptr_t)MENU_FILE, menu_size,
                       (const uint8_t *)(uintptr_t)GRAF_FILE, graf_size, game.local, game.local_size,
                       game.gen, game.gen_size, MENU_MEM, MENU_SIZE,
                       (uint8_t *)BUILD_SCRATCH, PROG_BASE - BUILD_SCRATCH);
    intro_ok = ok && intro_init((const uint8_t *)(uintptr_t)GRAF_FILE, graf_size, game.local,
                                game.local_size, INTRO_MEM, INTRO_SIZE, (uint8_t *)BUILD_SCRATCH,
                                PROG_BASE - BUILD_SCRATCH);
    while (!(MEM_STATUS & MEM_WRITES_IDLE))     /* wait until the pictures are in the SDRAM for the blitter */
        ;
    if (!intro_ok) dlog_str("intro: LOGOL.SPR, ATTRL.FNT or ATTR.TXT missing (no intro)\n");
    dlog_str("menu: MENU.DAT ");
    dlog_dec((int32_t)menu_size);
    dlog_str(", GRAF.DAT ");
    dlog_dec((int32_t)graf_size);
    dlog_str(", ");
    dlog_dec(menu_cars());
    dlog_str(ok ? " cars, their parts loaded in " : " cars - not all there (no menu: track 1, car 1), in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return ok;
}

/* The simulation's short cut (tools/simcheck.sh, SIM_IMAGE). The testbench offers the finished track image
   as data slot IMAGE_SLOT. core/data.json does not have this slot, so on the Pocket its size is 0 and the
   image is built instead. The image build itself is checked by tools/check_track_build.sh. Returns 1 if the
   image was loaded and opened. */
static int load_image(void)
{
    int err;
    uint32_t t0 = rdcycle(), size = load_file(IMAGE_SLOT, (uint32_t)(uintptr_t)TEX_FILE,
                                              game_files[0].addr - (uint32_t)(uintptr_t)TEX_FILE, &err);
    dcache_invalidate();
    if (!size || !track_open(TEX_FILE)) return 0;
    dlog_str("track image: ");
    dlog_dec((int32_t)size);
    dlog_str(" bytes from the testbench (data slot 30) in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return 1;
}

/* SOUND.DAT (sound.h) is read whole into the SDRAM once after the boot. Its read is started before the first
   track is built, so that the APF writes its 1.9 MB (1.3 s) while the CPU builds the track. sound_size is its
   size once it is there; sounds() reads it itself if it is not. sound_reading is its size while the read
   runs. */
static uint32_t sound_size, sound_reading;

/* Starts the read of SOUND.DAT in the background, unless it is there already or does not fit. */
static void sound_start(void)
{
    uint32_t size = slot_size(SOUND_SLOT);
    if (sound_size || size < 4 || size > SOUND_MAX) return;
    slot_start(SOUND_SLOT, 0, SOUND_MEM, size & ~3u);
    sound_reading = size;
}

/* Waits for the end of the read that sound_start began (it must end before any other slot call; the wait is
   limited to 20 s). Then it reads the last 1 to 3 bytes of the file. sound_size is set if all is there. */
static void sound_wait(void)
{
    uint32_t size = sound_reading, body = size & ~3u, t0 = rdcycle();
    if (!size) return;
    sound_reading = 0;
    while (slot_busy())
        if (rdcycle() - t0 > 20u * CPU_HZ) return;
    if (slot_result() == 0 &&
        (size == body || slot_read(SOUND_SLOT, body, SOUND_MEM + body, SLOT_REST) == 0))
        sound_size = size;
}

/* Builds the image of track n from the loaded containers at TEX_FILE and opens its header. The read of
   SOUND.DAT runs during the build (sound_start). Returns 1 if that worked. */
static int build_track(int n)
{
    struct track_build_info info;
    uint8_t *out = (uint8_t *)(uintptr_t)TEX_FILE;
    sound_start();
    uint32_t size = track_build(&game, n, out, game_files[0].addr - (uint32_t)(uintptr_t)out,
                                (uint8_t *)BUILD_SCRATCH, PROG_BASE - BUILD_SCRATCH, &info);
    sound_wait();
    while (!(MEM_STATUS & MEM_WRITES_IDLE))     /* wait until all of it is in the SDRAM for the 3D units */
        ;
    dcache_invalidate();                        /* the block check reads the SDRAM */
    if (!size || !track_open(TEX_FILE)) {
        dlog_str("track image: not built, stopped at ");
        dlog_str(size ? "its header" : info.fail);
        dlog_char('\n');
        return 0;
    }
    dlog_str("track image: ");
    dlog_dec((int32_t)size);
    dlog_str(" bytes built in ");
    dlog_dec((int32_t)(info.cycles / (CPU_HZ / 1000)));
    dlog_str(" ms, ");
    dlog_dec((int32_t)info.cycles);
    dlog_str(" cycles (unpacking ");
    dlog_dec((int32_t)info.unpack_cycles);
    dlog_str(")\n");
    return 1;
}

/* Sets up the effects of track n (efx.h) from MAPn.SMG, SHADOW.3DX and SMOKE.3DX in its GFX container, and
   logs which of them are there. */
static void effects(int n)
{
    uint32_t t0 = rdcycle();
    char name[16];
    int ok = efx_init(game.gfx, game.gfx_size, n, track_mlb(), track_palette(), EFX_MEM, EFX_SIZE);
    dlog_str("effects: ");
    dlog_str(track_name("MAP#.SMG ", n, name));
    dlog_str(efx_table() ? "ok" : "missing");
    dlog_str(", SHADOW.3DX ");
    dlog_str(efx_shadow() ? "ok" : "missing");
    dlog_str(", SMOKE.3DX ");
    dlog_str(efx_smoke() ? "ok" : "missing");
    dlog_str(ok ? ", in " : " - not all there, in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* Sets up the props and the moving scenery of track n (scenery.h) from its DATA and GFX containers. From the
   DATA container it also takes the places of the trackside cameras (car.h) and the numbers of the race mode
   (the carnage time and the gates of the slalom). */
static void scenery(int n)
{
    uint32_t t0 = rdcycle();
    int props, moving;
    scenery_init(game.data, game.data_size, game.gfx, game.gfx_size, n, SCENERY_MEM, SCENERY_SIZE, &props,
                 &moving);
    car_trackside_init(game.data, game.data_size, n);
    car_mode_init(game.data, game.data_size, n, track_build_difficulty);
    dlog_str("scenery: ");
    dlog_dec(props);
    dlog_str(" props, ");
    dlog_dec(moving);
    dlog_str(" moving, in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* Sets up the sound effects (sound.h). SOUND.DAT goes whole into the SDRAM once (normally while the first
   track is built, else here). Their list from GEN.DAT is read before each race; scenery() adds the track's
   own sounds after them. */
static void sounds(void)
{
    uint32_t t0 = rdcycle();
    int err;
    if (!sound_size) sound_size = load_file(SOUND_SLOT, SOUND_MEM, SOUND_MAX, &err);
    dcache_invalidate();
    int n = sound_size && game.gen_size ?
            snd_init((const uint8_t *)(uintptr_t)SOUND_MEM, sound_size, game.gen, game.gen_size) : 0;
    AU_STATUS = 0;
    dlog_str("sound: ");
    dlog_dec(n);
    dlog_str(" effects, ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
}

/* Sets up the menu's effects (snd_menu in sound.h): the nine samples of MENU.DTR in SOUND.DAT. Each is read
   once to its place, until the whole container is loaded with the first track. */
static void menu_sounds(void)
{
    static const struct menu_part parts[] = {
        { "MENU1.RAW", ~0u }, { "MENU2.RAW", ~0u }, { "MENU3.RAW", ~0u }, { "MENU4.RAW", ~0u },
        { "MENU5.RAW", ~0u }, { "MENU6.RAW", ~0u }, { "MENU7.RAW", ~0u }, { "MENU8.RAW", ~0u },
        { "MENU9.RAW", ~0u }, { 0, 0 } };
    static uint32_t size;
    if (!sound_size && !size) size = load_parts(SOUND_SLOT, SOUND_MEM, SOUND_MAX, parts);
    uint32_t have = sound_size ? sound_size : size;
    int n = have && game.gen_size ?
            snd_menu((const uint8_t *)(uintptr_t)SOUND_MEM, have, game.gen, game.gen_size) : 0;
    dlog_str("menu sounds: ");
    dlog_dec(n);
    dlog_str(" of MENU.DTR\n");
}

/* Sets up the dashboard of the player's car for the cockpit view (hud.h) from the container GAME.DAT and
   from GEN.DAT. Returns 1 if it is there. */
static int dashboard_init(void)
{
    uint32_t t0 = rdcycle();
    const struct car_spec *sp = &car_state()->spec;
    int ok = game.game_size && game.gen_size &&
             hud_cockpit_init(game.game, game.game_size, game.gen, game.gen_size, sp, DASH_MEM, DASH_SIZE,
                              (uint8_t *)(uintptr_t)BUILD_SCRATCH, PROG_BASE - BUILD_SCRATCH);
    dlog_str("cockpit: the dashboard of ");
    dlog_str(sp->name);
    dlog_str(ok ? " ok, in " : " missing (the cockpit shows the whole 3D view), in ");
    dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
    dlog_str(" ms\n");
    return ok;
}

/* The simulation ends at the third frame of a menu screen: it reports its result and the stack use. */
INLINE_STEP void menu_sim_done(void)
{
    SYS_SIM = SIM_DONE | results;
    dlog_str("stack used ");
    dlog_dec((int32_t)stack_used());
    dlog_str(" bytes\n");
}

/* Writes a line to the log: how long frame number drawn of the menu screen took, from its start at cycle
   count t_frame. */
INLINE_STEP void menu_log_frame(int screen, uint32_t drawn, uint32_t t_frame)
{
    dlog_str(screen == MENU_CAR_SCREEN ? "car screen" :
             screen == MENU_TRACK_SCREEN ? "track screen" :
             screen == MENU_CHAMP_SCREEN ? "championship" :
             screen == MENU_RECORDS_SCREEN ? "records" :
             screen == MENU_NAME_SCREEN ? "name" : "pages");
    dlog_str(" frame ");
    dlog_dec((int32_t)drawn);
    dlog_str(": ");
    dlog_dec((int32_t)(rdcycle() - t_frame));
    dlog_str(" cycles\n");
}

/* The simulation's check of the picture just drawn: the C models draw the same picture into CHECK_BUF, and
   compare_log compares it with the screen. */
INLINE_STEP void menu_check(void)
{
    volatile uint8_t *m = CHECK_BUF;
    uint32_t t0 = rdcycle();
    blit_model(m, &back);
    track_mode = TRACK_MODEL;
    menu_scene(m);
    track_mode = TRACK_HW;
    blit_model(m, &hud);
    compare_log(FB(draw_buf), m, t0);
}

/*
 * Runs one screen of the menu (menu.h) until something is chosen. screen is one of the MENU_*_SCREEN values
 * and n is the choice to start on. Returns the number chosen: the track, the car's row + 1, or for the pages
 * the item (1 is NORMAL GAME). It returns -1 when the track screen or the car screen goes back.
 *
 * In each frame the blitter draws the picture, the geometry unit draws the 3D models (or the letters' cubes)
 * meanwhile (hw_piece), and the texts follow. The screen fades in from black with the models already
 * turning. The palette fades at 1.5 full fades a second (original: STARTL.EXE 0x2c078, 0x2bdcc, 0x29738),
 * the pages at 0.6 (0x29ea0). A screen is left with a cut to black. The pages fade out at 2 a second while
 * the camera pulls back (0x293e8).
 * MUSIC changed on the options page stops the menu's song, or starts it again, at once.
 * In the simulation (SIM_MENU) the third frame is checked against the C models and reported.
 */
static int menu_screen(int screen, int n)
{
    uint32_t drawn = 0, keys_before = in_keys(), last_frame = 0, sim = SYS_FLAGS & FLAG_SIM;
    int32_t fade = 0, rate = screen == MENU_PAGE_SCREEN ? FADE_PAGES : FADE_MENU;
    int chosen = 0;                             /* the pages: set when an item is taken, then they fade out */
    menu_enter(screen, n);
    while (!(MEM_STATUS & MEM_WRITES_IDLE))     /* wait until the championship's picture is in the SDRAM */
        ;
    const uint32_t *pal = menu_palette();
    music_play(0);
    track_mode = TRACK_HW;
    track_hw_piece = hw_piece;
    GU_MLB = (uint32_t)((uintptr_t)menu_mlb() - SDRAM_BASE);
    GU_INVAL = 1;
    RS_CTRL = 1;                                /* flush the texture cache: the cars' are those of MENU.MLB */
    for (;;) {
        uint32_t st = next_frame(), keys = in_keys(), t_frame = rdcycle();
        music_frame();
        frames_shown = drawn;
        if (sim && drawn == 3) menu_sim_done();

        /* The menu runs at 50 steps a second, at most 4 steps for a picture. A choice ends the screen, but
           the pages fade out first. */
        uint32_t steps = drawn ? (VID_FRAME(st) - last_frame) & 0xFFFF : 1;
        last_frame = VID_FRAME(st);
        if (steps > 4) steps = 4;
        for (uint32_t k = 0; k < steps; k++) {
            int c = menu_step(keys, keys_before);
            keys_before = keys;
            if (c && screen != MENU_PAGE_SCREEN) {
                black();
                return c;
            }
            if (c) chosen = c;
        }
        if (chosen && (fade -= (int32_t)steps * FADE_RACE) <= 0) {
            black();
            return chosen;
        }
        if (!chosen && (fade += (int32_t)steps * rate) > 0x8000) fade = 0x8000;
        struct menu_options opt;
        menu_options(&opt);
        if (opt.music != music_on) {
            music_on = opt.music;
            if (music_on) music_play(0);
            else music_stop();
        }

        /* Draw the picture: the backdrop first, the models while the CPU waits, then the texts. */
        draw_buf = !(st & VID_FRONT);
        blit_start(&back, (int)draw_buf);
        menu_backdrop(&back);
        back_wait = 1;
        hw_blit(&back);
        pal_faded(pal, fade);
        GU_MODE = draw_buf << 8;
        RS_CLIP0 = RS_XY(track_clip.x0, track_clip.y0);
        RS_CLIP1 = RS_XY(track_clip.x1, track_clip.y1);
        struct su_view view;
        menu_view(&view);
        su_view_regs(&view);
        menu_scene(0);
        back_wait = 0;
        blit_start(&hud, (int)draw_buf);
        menu_text(&hud);
        hw_blit(&hud);
        while (RS_DRAW & RS_BUSY)
            ;
        if (drawn % 250 == 2) menu_log_frame(screen, drawn, t_frame);   /* every 250 frames */
        if (sim && drawn == 2) menu_check();    /* the simulation's check against the C models */
        VID_SWAP = 1;
        drawn++;
    }
}

/* Shows the loading screen of track n (menu.h; original: STARTL.EXE 0x2a264). The blitter draws it in one
   swap together with its palette, and it stays up while the track is set up. Returns its palette, or 0
   without the menu's files (the simulation's race), in which case nothing is shown. */
static const uint32_t *loading(int n)
{
    uint32_t st = wait_swap_done();
    draw_buf = !(st & VID_FRONT);
    blit_start(&hud, (int)draw_buf);
    const uint32_t *pal = menu_loading(n, &hud);
    if (!pal) return 0;
    while (!(MEM_STATUS & MEM_WRITES_IDLE))     /* wait until the picture is in the SDRAM for the blitter */
        ;
    for (int i = 0; i < 256; i++) VID_PAL[i] = pal[i];
    still_show(&hud);
    return pal;
}

/* Prepares track n: loads its containers (unless they are loaded already), builds its image (or takes the
   testbench's) and checks the image's blocks by their CRC. The image is used only if all blocks are as
   written. Returns the number of placements, or 0 if the track could not be set up. */
static int track_ready(int n)
{
    int placements = 0;
    if (track_loaded != n) {
        track_loaded = 0;
        if (!load_containers(n, TRACK_FILES)) return 0;
        track_loaded = n;
    }
    if (load_image() || build_track(n)) {
        int bad = 0;
        uint32_t t0 = rdcycle();
        for (int k = 0; k < track_blocks(); k++)
            bad += !track_block_ok(k);
        dlog_str("track image: CRC of the 256 KB blocks:");
        for (int k = 0; bad && k < track_blocks(); k++)     /* if one is damaged, list each (4 KB log) */
            dlog_str(track_block_ok(k) ? " ok" : " BAD");
        dlog_str(bad ? " - damaged in the SDRAM, not used (" : " all ok (");
        dlog_dec((int32_t)((rdcycle() - t0) / (CPU_HZ / 1000)));
        dlog_str(" ms)\n");
        if (!bad) placements = track_init();
    }
    return placements;
}

/*
 * Plays track n with the car of CARS.DTR row car, behind the track's loading screen. Returns 1 when the race
 * was left, or 0 if the track could not be set up.
 *   - The track is set up first (track_ready), then the effects and the scenery.
 *   - Then comes the race (run_track), until it is left. It starts again while PLAY AGAIN? is answered YES,
 *     without the loading screen, as the game does, and as its replay when run_track asks for it.
 *   - A finished race of the normal game or of the championship goes into the track's records (choice_record;
 *     in the original 0x2a1a8 and 0x1febf call 0x2e5e4 after each race). The lap to beat comes from the
 *     records (0x2a264). After PLAY AGAIN? it comes from the race before (0x20eb8).
 *   - Once all is set up, the loading screen fades out, the picture alone (0x128dc(1): 0x21d8c(0)). A race
 *     that is left, or is to be set up again, fades out with its sound (0x21d8c(1): 0x1feed, 0x128dc(0)).
 */
static int race(int n, int car)
{
    music_stop();                               /* before the loads, so that no music read is running; the
                                                   menu's song ends as the loading screen goes up */
    const uint32_t *shown = loading(n);
    car_lap_to_beat = choice.lap[(n - 1) % MENU_TRACKS].time[0];
    int placements = track_ready(n);
    if (!placements) return 0;
    const volatile uint32_t *pal = (const volatile uint32_t *)track_palette();
    col_bg = nearest(pal, 0, 0, 0);
    col_text = nearest(pal, 255, 255, 255);
    col_dim = nearest(pal, 120, 200, 120);
    col_alert = nearest(pal, 230, 25, 178);
    dlog_str("track ");
    dlog_dec(n);
    dlog_str(": ");
    dlog_dec(placements);
    dlog_str(" placements, ");
    dlog_dec((int32_t)track_path_length());
    dlog_str(" path points\n");
    for (int replay = 0;;) {    /* each run is set up as at the start, so the replay runs the same way */
        effects(n);
        sounds();               /* before the scenery, which adds the track's own sounds */
        scenery(n);
        RS_CTRL = 1;            /* flush the texture cache */
        if (shown) fade_out(shown, 0);
        shown = 0;
        music_play(n);
        int end = run_track(n, car, replay);
        if (!replay) {
            const struct car_race *r = car_race();
            car_lap_to_beat = car_lap_record();
            if (r->finished && (car_mode == CAR_NORMAL || car_mode == CAR_CHAMP))
                choice_record(n, r->best, r->total, car_laps == 3, car_mode);
        }
        ghost_end();                            /* time attack: a run that counted is kept (0x1fd64) */
        fade_out(track_palette(), 1);
        /* The race's sounds end with it, and its song too. After PLAY AGAIN? YES, and for the replay, the
           race is set up again (0x128dc) and its song starts from the beginning. */
        AU_STOP = 0xFFFFFFFFu;
        music_stop();
        snd_fade(0x8000);
        if (end == RACE_LEAVE) return 1;
        replay = end == RACE_REPLAY;
    }
}

/*
 * Runs the intro's race on track n, which is set up already (intro.h). In each frame the opponents, the
 * scenery and the intro's camera move (original: STARTL.EXE 0x2780c), and the logos and credits are drawn
 * over the picture (0x279d4). The intro fades in from black as a race does (0x21e50). The cars start to move
 * once it is fully faded in, as the original looks.
 * B, A, START or SELECT end the intro (the original handles ENTER, ESC, SPACE and the joystick's buttons,
 * 0x211d0), and so does the end of the credits. Picture and sound then fade out in a second (0x27d28).
 * The function returns after that fade.
 */
static void run_intro(int n)
{
    static struct track_object objs[MAX_OBJECTS];
    const uint32_t *pal = track_palette();
    const struct rs_clip clip = { 0, 0, FB_W, FB_H };
    int32_t fade = 0;
    uint32_t drawn = 0, keys_before = IN_KEY, last_frame = 0;
    snd_fade(0);
    music_fade(0);
    car_init(choice.car, 0);                    /* set up the race's state, then take the player's car out */
    opp_init(choice.car);
    car_intro();
    intro_start(game.data, game.data_size, n);
    track_mode = TRACK_HW;
    track_hw_piece = hw_piece;
    GU_MLB = (uint32_t)((uintptr_t)track_mlb() - SDRAM_BASE);
    GU_INVAL = 1;
    for (;;) {
        uint32_t st = next_frame(), keys = IN_KEY, t_frame = rdcycle();
        music_frame();
        frames_shown = drawn;
        if (keys & ~keys_before & (KEY_A | KEY_B | KEY_START | KEY_SELECT)) intro_leave();
        keys_before = keys;

        /* Steps at 50 a second, at most 4 for a picture. intro_step() returns non-zero once the intro is
           over. The fade level combines the fade-in with the intro's own level (intro_level()). */
        uint32_t steps = drawn ? (VID_FRAME(st) - last_frame) & 0xFFFF : 1;
        last_frame = VID_FRAME(st);
        if (steps > 4) steps = 4;
        if (fade < 0x8000 && (fade += (int32_t)steps * FADE_RACE) > 0x8000) fade = 0x8000;
        int over = 0;
        for (uint32_t k = 0; k < steps; k++) {
            if (fade == 0x8000) {
                opp_step();
                scenery_step();
            }
            over |= intro_step();
        }
        int32_t level = (int32_t)((int64_t)fade * intro_level() >> 15);
        snd_fade(level);
        music_fade(level);
        if (over) return;
        track_clock(steps);

        /* Camera and objects, then the picture as in a race: backdrop, scene, texts. */
        struct track_cam cam;
        intro_camera(&cam);
        int nobj = opp_objects(objs, MAX_OBJECTS);
        nobj += scenery_objects(&cam, objs + nobj, MAX_OBJECTS - nobj);
        struct su_view view;
        track_view(&clip, &view);
        draw_buf = !(st & VID_FRONT);
        blit_start(&back, (int)draw_buf);
        track_backdrop_blit(&back, &clip, &cam);
        back_wait = 1;
        hw_blit(&back);
        pal_faded(pal, level);
        GU_MODE = draw_buf << 8;
        RS_CLIP0 = RS_XY(clip.x0, clip.y0);
        RS_CLIP1 = RS_XY(clip.x1, clip.y1);
        su_view_regs(&view);
        struct track_stats ts;
        track_draw_cam(0, &clip, &cam, objs, nobj, &ts);
        back_wait = 0;
        blit_start(&hud, (int)draw_buf);
        intro_text(&hud);
        hw_blit(&hud);
        while (RS_DRAW & RS_BUSY)
            ;
        if (drawn % 500 == 2) {                 /* every 500 frames a line in the log */
            dlog_str("intro frame ");
            dlog_dec((int32_t)drawn);
            dlog_str(": ");
            dlog_dec((int32_t)ts.pieces);
            dlog_str(" pieces, ");
            dlog_dec((int32_t)(rdcycle() - t_frame));
            dlog_str(" cycles\n");
        }
        VID_SWAP = 1;
        drawn++;
    }
}

/* The track of the next intro, 1 to 6: CHOICE.DAT +0x434 plus 1. */
static int intro_track(void)
{
    return (int)((uint32_t)choice.intro % MENU_TRACKS) + 1;
}

/* Shows the first picture of a boot, as early as it can be: the loading screen of the intro's track n, as a
   race has it (loading()). It needs LOADING from LOCAL.DAT and, of GRAF.DAT, only the directory, MENUL.SPR,
   MENUL.FNT and LOADnL.PCX, each at its place. The rest of the menu's files, the intro's track and the sounds
   load behind it. Returns its palette, or 0 if the files are not there; the screen then stays black until the
   intro. */
static const uint32_t *first_picture(int n)
{
    char file[16] = "LOAD1L.PCX";
    struct menu_part parts[] = { { "MENUL.SPR", ~0u }, { "MENUL.FNT", ~0u }, { file, ~0u }, { 0, 0 } };
    file[4] = (char)('0' + n);
    uint32_t size = load_containers(0, LOCAL_FILES) ? load_parts(GRAF_SLOT, GRAF_FILE, MENU_MAX, parts) : 0;
    const uint32_t *pal = size && menu_loading_init((const uint8_t *)(uintptr_t)GRAF_FILE, size, game.local,
                                                    game.local_size, MENU_MEM, MENU_SIZE,
                                                    (uint8_t *)BUILD_SCRATCH, PROG_BASE - BUILD_SCRATCH)
                          ? loading(n) : 0;
    dlog_str(pal ? "first picture: the loading screen of track "
                 : "first picture: none (GRAF.DAT or LOCAL.DAT missing), track ");
    dlog_dec(n);
    dlog_str(", ");
    dlog_dec((int32_t)(rdcycle() / (CPU_HZ / 1000)));
    dlog_str(" ms after the core's start\n");
    return pal;
}

/* The intro (intro.h; original: STARTL.EXE 0x276a0). Its race on track intro_track() is set up as race() sets
   one up, but without the loading screen (0x128dc): the screen is black meanwhile. At the boot, shown is the
   palette of the first picture. That picture stays up and fades out like a race's loading screen. The
   difficulty is NORMAL (0x2777f) and the menu's song plays. Afterwards the intro's track moves on by one
   (0x27f06) and every page of the menu goes back to its first line (0x27eb4). */
static void intro(const uint32_t *shown)
{
    int n = intro_track();
    if (!shown) black();
    music_stop();
    car_mode = CAR_INTRO;
    track_build_difficulty = 1;
    opp_bullet = 0;
    int ok = track_ready(n);
    if (ok) {
        effects(n);
        sounds();
        scenery(n);
        RS_CTRL = 1;
    }
    if (shown) fade_out(shown, 0);
    if (ok) {
        music_play(0);
        run_intro(n);
        AU_STOP = 0xFFFFFFFFu;
        music_stop();
        snd_fade(0x8000);
    }
    choice.intro = (int32_t)((uint32_t)n % MENU_TRACKS);
    menu_fresh();
}

/*
 * The championship (champ.h), from NEW or RESUME (original: STARTL.EXE 0x28c6b, 0x28c9e) until it goes back
 * to the main menu. resume is 1 for RESUME and 0 for NEW; *car is the row of the car chosen last. The loop
 * goes stage by stage:
 *   - The stage's screen is shown. ENTER on a race's text screen opens the car screen (0x291ae). Back from
 *     the car screen, the stage's screen is shown again.
 *   - The car chosen starts the race on the stage's track: three laps, the league's opponents
 *     (0x28ef6: 0x2a264(2, track, league)).
 *   - The result goes into the standings (0x29acc), which are shown next (0x293e8(5)).
 *   - A race that is finished goes on by itself after the end screen's 5 s. One that ran out of time, or was
 *     left, asks PLAY AGAIN? (0x1fe86), and YES races it again.
 */
static void championship(int resume, int *car)
{
    if (resume) champ_resume();
    else champ_new();
    for (;;) {
        if (menu_screen(MENU_CHAMP_SCREEN, 0) < 0) return;
        int k = champ_enter();
        if (k == CHAMP_MENU) return;
        if (k != CHAMP_RACE) continue;
        int c = menu_screen(MENU_CAR_SCREEN, *car + 1);
        if (c < 0) continue;
        *car = choice.car = c - 1;
        struct champ_stage st = champ_at(choice.stage);
        struct menu_options opt;
        menu_options(&opt);
        car_laps = 3;
        track_build_difficulty = st.league;
        opp_bullet = st.league > 2;
        speech_on = opt.speech;
        car_mode = CAR_CHAMP;
        int ok = race(st.track, *car);
        opp_bullet = 0;
        menu_sounds();
        if (!ok) return;
        const struct car_race *r = car_race();
        champ_result(!r->finished && !r->game_over, !r->finished && r->game_over, opp_player_rank(),
                     r->total);
        dlog_str("championship: stage ");
        dlog_dec(choice.stage);
        dlog_str(", place ");
        dlog_dec(opp_player_rank());
        dlog_str(r->finished ? " finished\n" : r->game_over ? " time over\n" : " left\n");
    }
}

/* Shows the list of the game's files (files.c) for ever. The swaps go on, and the simulation gets its result
   after three frames. failed is 0 if a file is missing, and 1 if all needed files are there but loading them
   failed (the log says why). */
static void __attribute__((noreturn)) files_loop(int failed)
{
    uint32_t drawn = 0;
    for (;;) {
        uint32_t st = wait_swap_done();
        frames_shown = drawn;       /* every frame drawn so far has been swapped in */
        if (drawn == 3) {           /* three frames drawn and swapped */
            SYS_SIM = SIM_DONE | results;
            dlog_str("stack used ");
            dlog_dec((int32_t)stack_used());
            dlog_str(" bytes\nSYS_SIM=");
            dlog_hex(SIM_DONE | results);
            dlog_char('\n');
        }
        files_draw(FB(!(st & VID_FRONT)), failed);
        VID_SWAP = 1;
        log_head("frames drawn: ", ++drawn);
    }
}

/*
 * The entry point, called by the start code (start_prog.S) with the boot results and the state of the debug
 * log (dlog.h). It never returns: the game runs for ever, or the list of files is shown.
 *
 * After the start-up it loads the game's files that stay (data slots 4, 6 and 7, which are deferload slots in
 * core/data.json) and the menu's. Then the screens follow each other for ever. The variable `at` says which
 * one comes next:
 *   - AT_PAGES: the main menu. At the start, and after 30 s on it without a key, the intro runs before it
 *     (original: STARTL.EXE 0x10020: intro, menu, intro, ...). A track of the VIEW RECORDS page shows its
 *     records; A or SELECT there goes back to the page (0x28a7c).
 *   - AT_NAME: ENTER YOUR NAME, after a game's line of the main menu. A or SELECT there shows the page
 *     again (0x293e8(4)). After the name the championship (champ.h) is played, or the game goes on:
 *   - AT_TRACKS: the track screen. A or SELECT there shows the name again (0x281c2).
 *   - AT_CARS: the car screen. A or SELECT there shows the track screen again.
 * Then the race on the track chosen, with the car chosen and the laps, difficulty and speech of the options
 * page, and the main menu again (0x2a114 -> 0x29ea0).
 * The simulation races the testbench's track with the first car at once, or shows the screen that
 * MENU_CHECK_SLOT names.
 */
void prog_main(uint32_t boot_results, uint32_t log_pos, uint32_t log_cur)
{
    results = boot_results;
    stack_fill();
    dlog_attach(log_pos, log_cur);
    log_head("frames drawn: ", 0);
    dlog_str("program.bin: running in the SDRAM\n");
    save_load();                    /* first: until it has run, an exit would save the SDRAM as it is */
    menu_options_load();
    volumes();
    config_load();                  /* screamer.cfg: how far the core draws (config.h) */
    race_debug_init();
    if (files_missing()) files_loop(0);         /* a file the game needs is missing: the list, for ever */
    int sim = (SYS_FLAGS & FLAG_SIM) != 0;
    const uint32_t *shown = sim ? 0 : first_picture(intro_track());    /* the simulation's race has none */

    /* The files that stay, the menu's files and the sounds; then the screens. */
    if (load_containers(0, game.local_size ? FIXED_FILES & ~LOCAL_FILES : FIXED_FILES)) {
        music_init(game.gen, game.gen_size);
        if (!hud_cd_init(game.game, game.game_size, CD_MEM, CD_SIZE, (uint8_t *)BUILD_SCRATCH,
                         PROG_BASE - BUILD_SCRATCH))
            dlog_str("pause: CDFRONTL.PKC missing (the darkened picture alone)\n");
        int check = sim ? (int)slot_size(MENU_CHECK_SLOT) : 0;
        int screen = (!sim || check) && menus();
        int n = sim && slot_size(TRACK_SLOT) ? (int)slot_size(TRACK_SLOT) : choice.track % MENU_TRACKS + 1;
        int car = choice.car;                   /* the last track and car, as CHOICE.DAT keeps them */
        race_debug_bench(race);
        if (screen) menu_sounds();
        enum { AT_PAGES, AT_NAME, AT_TRACKS, AT_CARS };     /* the screen to go on with */
        const int first = !check ? AT_PAGES : check == 2 ? AT_CARS : AT_TRACKS;
        for (int at = first, page = 1, g = 0, show_intro = intro_ok && !check;;) {
            if (screen) {
                if (at == AT_PAGES) {
                    if (show_intro) {
                        intro(shown);
                        menu_sounds();          /* the intro's race had its own sounds (sounds()) */
                        show_intro = 0;
                        page = 1;
                    } else if (shown) {
                        fade_out(shown, 0);     /* no intro: the first picture goes as a race's */
                    }
                    shown = 0;
                    g = menu_screen(MENU_PAGE_SCREEN, page);    /* g is the item taken on the page */
                    page = 0;                   /* when back on it, show the page as it was */
                    if (g == MENU_IDLE) {
                        show_intro = 1;
                        continue;
                    }
                    if (g > MENU_RECORDS) {
                        menu_screen(MENU_RECORDS_SCREEN, g - MENU_RECORDS);
                        continue;
                    }
                    at = AT_NAME;
                }
                if (at == AT_NAME) {
                    if (menu_screen(MENU_NAME_SCREEN, 0) < 0) {
                        at = AT_PAGES;
                        continue;
                    }
                    if (g == MENU_CHAMP_NEW || g == MENU_CHAMP_RESUME) {
                        championship(g == MENU_CHAMP_RESUME, &car);
                        at = AT_PAGES;
                        page = 1;
                        continue;
                    }
                    at = AT_TRACKS;
                }
                if (at == AT_TRACKS) {
                    int t = menu_screen(MENU_TRACK_SCREEN, n);
                    if (t < 0) {
                        at = first == AT_PAGES ? AT_NAME : first;
                        continue;
                    }
                    n = t;
                    choice.track = n - 1;
                }
                int c = menu_screen(MENU_CAR_SCREEN, car + 1);
                if (c < 0) {
                    at = first == AT_CARS ? AT_CARS : AT_TRACKS;
                    continue;
                }
                at = first;                     /* after the race the first screen comes again */
                page = 1;
                car = choice.car = c - 1;
                struct menu_options opt;
                menu_options(&opt);
                car_laps = opt.laps;
                car_mode = menu_mode();
                /* Difficulty 3 (AFTERBURNER) uses the files of difficulty E, 4 (original: 0x2a131). */
                track_build_difficulty = opt.difficulty == 3 ? 4 : opt.difficulty;
                opp_bullet = track_build_difficulty > 2;
                speech_on = opt.speech;
            }
            if (!race(n, car) && !screen) break;    /* without menus, a track that fails ends the loop */
            if (screen) menu_sounds();          /* the race had its own sounds (sounds()) */
        }
    }
    /* The game's files are all there, but loading them failed (the log says why). */
    files_loop(1);
}
