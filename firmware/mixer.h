/*
 * The C model of the sound mixer (core/src/fpga/core/audio.v), bit for bit: 32 voices of 8-bit
 * unsigned samples in the SDRAM, one pass per output frame of 48 kHz. Its registers are the
 * hardware's (hw.h AU_*); a register written between two passes takes effect in the next.
 * sim/audio checks the hardware against it sample by sample; the firmware drives the hardware
 * through sound.c.
 *
 * A pass, voice by voice: a playing voice that was (re)started takes its first byte as its
 * place, with no fraction and its loop count, and is heard from the next pass on; any other
 * playing voice adds the byte at its place, centred (byte - 128), times its left and right
 * volume to the sums, then its place moves on by its step (2.16). At or past its end it goes
 * back by its loop's length while it has loops left (255: for ever), else it stops there.
 * Both sums clipped to 16 bits are the frame.
 */
#pragma once
#include <stdint.h>

#define MIX_VOICES  32

struct mix_voice {
    uint32_t addr, end, loop, step, vol;    /* the registers as written (the bits the hardware keeps) */
    uint32_t cur, frac, left;               /* the place: byte address, fraction, loops left */
};

struct mixer {
    struct mix_voice v[MIX_VOICES];
    uint32_t playing, fresh;                /* bit per voice: plays; (re)started, place not set */
    int16_t out_l, out_r;                   /* the last pass's frame */
};

void mix_reset(struct mixer *m);
/* A register write: reg is the offset from the first register (AU_ADDR(0)), as the hardware's
   wr_addr */
void mix_write(struct mixer *m, uint32_t reg, uint32_t value);
/* One pass: the frame into out_l, out_r. sdram is the SDRAM, byte address 0 at sdram[0] */
void mix_frame(struct mixer *m, const uint8_t *sdram);
