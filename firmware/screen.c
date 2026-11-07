/*
 * What the boot loader and the program both draw (screen.h): the wait for a buffer swap and
 * the test band.
 */
#include "hw.h"
#include "dlog.h"
#include "screen.h"

uint32_t wait_swap_done(void)
{
    static int warned;
    uint32_t t0 = rdcycle(), st;
    while ((st = VID_STATUS) & VID_PENDING) {
        if (!warned && rdcycle() - t0 > CPU_HZ / 2) {
            warned = 1;
            dlog_str("swap still pending after 0.5 s, VID_STATUS=");
            dlog_hex(st);
            dlog_char('\n');
        }
    }
    return st;
}

void draw_band(volatile uint8_t *fb)
{
    volatile uint32_t *p = (volatile uint32_t *)fb;
    for (int y = 0; y < BAND_H; y++)
        for (uint32_t x = 0; x < FB_W / 4; x++)     /* word x holds the pixels 4x .. 4x+3 */
            *p++ = x < 64 ? 0x03020100u + 0x04040404u * x : 0;     /* index = column up to 255 */
}
