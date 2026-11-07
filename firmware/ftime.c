/*
 * The race's frame times (ftime.h): the statistics of a run of frames, and the figures for the overlay.
 */
#include "hw.h"
#include "console.h"
#include "ftime.h"

#define RING    50                  /* the overlay's mean covers the last second (50 pictures) */

static struct {
    /* Since ft_reset, over all frames of the run. */
    uint64_t sum;                   /* the work of all frames */
    uint64_t sum_faces, sum_pieces;
    uint32_t n, max, late;          /* frames, the largest work, frames over FT_LATE */
    struct ft_frame worst;          /* the longest frame */
    /* The overlay. */
    int on;
    uint32_t o_max, o_late;         /* since the overlay was switched on */
    uint32_t o_last, o_faces, o_pieces;     /* the last frame */
    uint32_t ring[RING], ring_at, ring_n;   /* the work of the last frames; where the next goes; how many */
    uint64_t ring_sum;              /* the sum of the work in the ring */
} ft;

void ft_reset(void)
{
    ft.sum = ft.n = ft.max = ft.late = 0;
    ft.sum_faces = ft.sum_pieces = 0;
    ft.worst = (struct ft_frame){ 0 };
}

void ft_summary(struct ft_summary *s)
{
    uint32_t n = ft.n ? ft.n : 1;   /* no division by zero for an empty run */
    s->frames = ft.n;
    s->avg = (uint32_t)(ft.sum / n);
    s->max = ft.max;
    s->late = ft.late;
    s->faces_avg = (uint32_t)(ft.sum_faces / n);
    s->faces_worst = ft.worst.faces;
    s->pieces_avg = (uint32_t)(ft.sum_pieces / n);
    s->pieces_worst = ft.worst.npieces;
}

void ft_overlay_toggle(void)
{
    ft.on = !ft.on;
    ft.o_max = ft.o_late = 0;
}

int ft_overlay_on(void)
{
    return ft.on;
}

void ft_frame(const struct ft_frame *f)
{
    ft.n++;
    ft.sum += f->work;
    ft.late += f->work > FT_LATE;
    ft.sum_faces += f->faces;
    ft.sum_pieces += f->npieces;
    if (f->work > ft.max) {
        ft.max = f->work;
        ft.worst = *f;
    }
    ft.o_late += f->work > FT_LATE;
    if (f->work > ft.o_max) ft.o_max = f->work;
    ft.o_last = f->work;
    ft.o_faces = f->faces;
    ft.o_pieces = f->npieces;
    /* The ring replaces its oldest value by the new one. The sum is updated in two steps, because the
       difference of two uint32_t values would wrap around instead of becoming negative. */
    ft.ring_sum += f->work;
    ft.ring_sum -= ft.ring[ft.ring_at];
    ft.ring[ft.ring_at] = f->work;
    if (++ft.ring_at == RING) ft.ring_at = 0;
    if (ft.ring_n < RING) ft.ring_n++;
}

/* Writes cycles as milliseconds with one decimal ("12.3"), rounded, at p. Returns a pointer to the end. */
char *ft_ms(char *p, uint32_t cycles)
{
    uint32_t tenths = (cycles + CPU_HZ / 20000) / (CPU_HZ / 10000);
    p = fmt_udec(p, tenths / 10);
    *p++ = '.';
    *p++ = (char)('0' + tenths % 10);
    *p = 0;
    return p;
}

char *ft_head(char *p)
{
    if (!ft.n) return p;
    p = ft_ms(fmt_str(p, " avg "), (uint32_t)(ft.sum / ft.n));
    p = ft_ms(fmt_str(p, " max "), ft.max);
    p = fmt_udec(fmt_str(p, " late "), ft.late);
    p = ft_ms(fmt_str(p, " | a"), ft.worst.pre);
    p = ft_ms(fmt_str(p, " c"), ft.worst.cull);
    p = ft_ms(fmt_str(p, " s"), ft.worst.pieces);
    p = ft_ms(fmt_str(p, " h"), ft.worst.post);
    p = ft_ms(fmt_str(p, " w"), ft.worst.wait);
    p = fmt_udec(fmt_str(p, " f"), ft.worst.faces);
    p = fmt_udec(fmt_str(p, " p"), ft.worst.npieces);
    return fmt_udec(fmt_str(p, " m"), ft.worst.where);
}

void ft_draw(volatile uint8_t *fb, uint8_t fg, uint8_t bg, float far_scale, float lod_scale)
{
    char line[CON_COLS + 1], *p;
    if (!ft.ring_n) return;
    /* Line 0: the last frame, the mean of the last second, the maximum and the late frames. */
    p = ft_ms(line, ft.o_last);
    p = ft_ms(fmt_str(p, " ms  avg "), (uint32_t)(ft.ring_sum / ft.ring_n));
    p = ft_ms(fmt_str(p, "  max "), ft.o_max);
    fmt_udec(fmt_str(p, "  late "), ft.o_late);
    con_text(fb, 0, 0, line, fg, bg);
    /* Line 1: the faces and pieces of the last frame. */
    p = fmt_udec(fmt_str(line, "faces "), ft.o_faces);
    fmt_udec(fmt_str(p, "  pieces "), ft.o_pieces);
    con_text(fb, 0, 1, line, fg, bg);
    /* Line 2: the settings in force, for example "F 2.00  L 2.50". */
    p = fmt_scale(fmt_str(line, "F "), far_scale);
    fmt_scale(fmt_str(p, "  L "), lod_scale);
    con_text(fb, 0, 2, line, fg, bg);
}
