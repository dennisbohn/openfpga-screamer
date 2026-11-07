/*
 * The race's frame times (ftime.h).
 */
#include "hw.h"
#include "console.h"
#include "ftime.h"

#define RING    50                  /* the overlay's mean is over the last second (50 pictures) */

static struct {
    uint64_t sum;                   /* since ft_reset: all the frames' work */
    uint64_t sum_faces, sum_pieces, sum_tiny;
    uint32_t n, max, late;
    struct ft_frame worst;          /* the longest of them */
    int on;                         /* the overlay */
    uint32_t o_max, o_late;         /* since it was switched on */
    uint32_t o_last, o_faces, o_pieces, o_tiny;
    uint32_t ring[RING], ring_at, ring_n;
    uint64_t ring_sum;
} ft;

void ft_reset(void)
{
    ft.sum = ft.n = ft.max = ft.late = 0;
    ft.sum_faces = ft.sum_pieces = ft.sum_tiny = 0;
    ft.worst = (struct ft_frame){ 0 };
}

void ft_summary(struct ft_summary *s)
{
    uint32_t n = ft.n ? ft.n : 1;
    s->frames = ft.n;
    s->avg = (uint32_t)(ft.sum / n);
    s->max = ft.max;
    s->late = ft.late;
    s->faces_avg = (uint32_t)(ft.sum_faces / n);
    s->faces_worst = ft.worst.faces;
    s->pieces_avg = (uint32_t)(ft.sum_pieces / n);
    s->pieces_worst = ft.worst.npieces;
    s->tiny_avg = (uint32_t)(ft.sum_tiny / n);
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
    ft.sum_tiny += f->tiny;
    if (f->work > ft.max) {
        ft.max = f->work;
        ft.worst = *f;
    }
    ft.o_late += f->work > FT_LATE;
    if (f->work > ft.o_max) ft.o_max = f->work;
    ft.o_last = f->work;
    ft.o_faces = f->faces;
    ft.o_pieces = f->npieces;
    ft.o_tiny = f->tiny;
    ft.ring_sum += f->work;                     /* (two steps: the difference of two uint32_t is not negative) */
    ft.ring_sum -= ft.ring[ft.ring_at];
    ft.ring[ft.ring_at] = f->work;
    if (++ft.ring_at == RING) ft.ring_at = 0;
    if (ft.ring_n < RING) ft.ring_n++;
}

/* cycles as milliseconds with one decimal ("12.3"), rounded */
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

/* a factor with two decimals ("1.50"), "off" for 0 */
char *ft_scale(char *p, float v)
{
    if (v <= 0.0f) return fmt_str(p, "off");
    uint32_t h = (uint32_t)(v * 100.0f + 0.5f);
    p = fmt_udec(p, h / 100);
    *p++ = '.';
    *p++ = (char)('0' + h % 100 / 10);
    *p++ = (char)('0' + h % 10);
    *p = 0;
    return p;
}

void ft_draw(volatile uint8_t *fb, uint8_t fg, uint8_t bg, float far_scale, float lod_scale, float tiny_px)
{
    char line[CON_COLS + 1], *p;
    if (!ft.ring_n) return;
    p = ft_ms(line, ft.o_last);
    p = ft_ms(fmt_str(p, " ms  avg "), (uint32_t)(ft.ring_sum / ft.ring_n));
    p = ft_ms(fmt_str(p, "  max "), ft.o_max);
    fmt_udec(fmt_str(p, "  late "), ft.o_late);
    con_text(fb, 0, 0, line, fg, bg);
    p = fmt_udec(fmt_str(line, "faces "), ft.o_faces);
    fmt_udec(fmt_str(p, "  pieces "), ft.o_pieces);
    con_text(fb, 0, 1, line, fg, bg);
    p = ft_scale(fmt_str(line, "F "), far_scale);          /* (40 columns: "F 2.00  L 2.50  tiny 1.00: 137 out") */
    p = ft_scale(fmt_str(p, "  L "), lod_scale);
    p = ft_scale(fmt_str(p, "  tiny "), tiny_px);
    if (tiny_px > 0.0f) fmt_udec(fmt_str(p, ": "), ft.o_tiny);
    con_text(fb, 0, 2, line, fg, bg);
}
