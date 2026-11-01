/*
 * openscreamer's driver (port/src/music.c, GPL-3.0, read from the game's HMIMDRV.386 and
 * checked there) on the PC, with the chip replaced by a log: every register write with the
 * number of the tick it was made in, in the form tools/host/fmdump.c prints for the firmware's.
 * tools/fmcheck.sh compares the two. music.c is included from where $OPENSCREAMER has it (-I),
 * never copied into this repository.
 * Usage: fmref <folder with MELODIC.BNK, DRUM.BNK, MUSICn.HMP> <song> <ticks>
 */
#include <stdio.h>
#include <stdlib.h>
#include "music.c"

static long tickno;

void fmchip_reset(void) {}
void fmchip_write(int reg, int value) { printf("%ld %02x %02x\n", tickno, reg & 0xff, value & 0xff); }
float fmchip_frame(void) { return 0.0f; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: fmref <folder> <song> <ticks>\n");
        return 2;
    }
    long ticks = atol(argv[3]);
    if (!music_play(argv[1], atoi(argv[2]))) return 1;
    for (tickno = 0; tickno < ticks; tickno++)
        song_tick();
    return 0;
}
