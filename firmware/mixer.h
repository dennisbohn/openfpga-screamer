/*
 * The C model of the sound mixer (core/src/fpga/core/audio.v), bit for bit. The mixer has 32
 * voices of 8-bit unsigned samples in the SDRAM and makes one pass per output frame of 48 kHz.
 * Its registers are the hardware's (hw.h AU_*). A register written between two passes takes
 * effect in the next pass. sim/audio checks the hardware against this model sample by sample;
 * the firmware drives the hardware through sound.c.
 *
 * A pass handles the voices one by one:
 *  - A playing voice that was (re)started takes its first byte as its place, with no fraction
 *    and with its loop count. It is heard from the next pass on.
 *  - Any other playing voice adds the byte at its place, centred (byte - 128), times its left
 *    and right volume to the sums. Then its place moves on by its step (2.16 fixed point).
 *  - At or past its end, a voice goes back by its loop's length while it has loops left (255
 *    means for ever). Otherwise it stops there.
 *
 * Then the music, while it plays: 16-bit stereo frames at 44.1 kHz in a ring of 256 KB. A frame
 * is one word, with the left sample in the low half.
 *  - A music that was started waits two passes, because the hardware loads the two lines
 *    around its place.
 *  - Each pass then takes the frame at its place and the one after it. For each side it adds
 *    first + (second - first) * fraction >> 16, times the volume >> 15, to the sums.
 *  - The place moves on by 60211 / 65536 of a frame (44100 / 48000), round the ring.
 *
 * Both sums, clipped to 16 bits, are the output frame.
 */
#pragma once
#include <stdint.h>

#define MIX_VOICES  32
#define MIX_MUSIC_RING  0x40000u            /* the music's ring: 256 KB, 65536 frames */
#define MIX_MUSIC_STEP  60211u              /* the music's step per output frame, 0.16: 44100 / 48000 */

struct mix_voice {
    uint32_t addr, end, loop, step, vol;    /* the registers as written (only the bits the hardware keeps) */
    uint32_t cur, frac, left;               /* the place: byte address, fraction, loops left */
};

struct mixer {
    struct mix_voice v[MIX_VOICES];
    uint32_t playing, fresh;                /* a bit per voice: it plays; it was (re)started, no place yet */
    uint32_t mu_base, mu_vol, mu_on;        /* the music's registers */
    uint32_t mu_fill, mu_idx, mu_frac;      /* passes still to wait; the place: frame in the ring, fraction */
    int16_t out_l, out_r;                   /* the frame of the last pass */
};

/* Stop all voices and the music and clear the output */
void mix_reset(struct mixer *m);

/* A register write. reg is the byte offset from the first register (AU_ADDR(0)), the same as
   the hardware's wr_addr. Voice registers are 32 bytes apart; the other registers follow at
   0x400 (AU_PLAY). */
void mix_write(struct mixer *m, uint32_t reg, uint32_t value);

/* One pass: the frame goes into out_l and out_r. sdram is the SDRAM, with byte address 0 at
   sdram[0]. */
void mix_frame(struct mixer *m, const uint8_t *sdram);
