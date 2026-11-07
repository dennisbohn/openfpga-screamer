/*
 * The C model of the sound mixer (mixer.h, which describes a pass). mix_write() takes the
 * register writes, mix_frame() makes one pass. sim/audio runs it next to the hardware.
 */
#include "mixer.h"

#define ADDR_MASK   0x3FFFFFFu      /* 26 bits: the SDRAM's 64 MB */

void mix_reset(struct mixer *m)
{
    m->playing = m->fresh = 0;
    m->mu_on = 0;
    m->out_l = m->out_r = 0;
}

void mix_write(struct mixer *m, uint32_t reg, uint32_t value)
{
    if (reg < 0x400) {
        /* A voice register: bits 9..5 of the offset pick the voice, bits 4..2 the register.
           Each case keeps only the bits that the hardware keeps. */
        struct mix_voice *v = &m->v[(reg >> 5) & (MIX_VOICES - 1)];
        switch ((reg >> 2) & 7) {
        case 0: v->addr = value & ADDR_MASK; break;
        case 1: v->end = value & ADDR_MASK; break;
        case 2: v->loop = value & 0xFF0FFFFFu; break;      /* loop count in 31..24, length in 19..0 */
        case 3: v->step = value & 0x3FFFFu; break;          /* 2.16 fixed point: 18 bits */
        case 4: v->vol = value & 0xFFFFu; break;            /* left in 7..0, right in 15..8 */
        default: break;
        }
    } else if (reg == 0x400) {              /* AU_PLAY: start the voices whose bit is set */
        m->playing |= value;
        m->fresh |= value;
    } else if (reg == 0x404) {              /* AU_STOP */
        m->playing &= ~value;
    } else if (reg == 0x40C) {              /* AU_MUSIC_ADDR: the ring is 256 KB aligned */
        m->mu_base = value & ADDR_MASK & ~(MIX_MUSIC_RING - 1);
    } else if (reg == 0x410) {              /* AU_MUSIC_VOL */
        m->mu_vol = value & 0xFFFF;
    } else if (reg == 0x414) {              /* AU_MUSIC: bit 0 starts (1) or stops (0) the music */
        m->mu_on = value & 1;
        m->mu_fill = 2;
        m->mu_idx = m->mu_frac = 0;
    }
}

/* Side k (0 left, 1 right) of the music's frame i in the ring */
static int32_t music_sample(const struct mixer *m, const uint8_t *sdram, uint32_t i, int k)
{
    const uint8_t *p = sdram + m->mu_base + 4 * (i & (MIX_MUSIC_RING / 4 - 1)) + 2 * k;
    return (int16_t)(p[0] | p[1] << 8);
}

/* Limit a sum to 16 bits */
static int16_t clip(int32_t a)
{
    return (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
}

void mix_frame(struct mixer *m, const uint8_t *sdram)
{
    int32_t acc_l = 0, acc_r = 0;
    for (int k = 0; k < MIX_VOICES; k++) {
        struct mix_voice *v = &m->v[k];
        uint32_t bit = 1u << k;
        if (!(m->playing & bit)) {
            m->fresh &= ~bit;
            continue;
        }
        if (m->fresh & bit) {               /* (re)started: set the place, play from the next pass */
            m->fresh &= ~bit;
            v->cur = v->addr;
            v->frac = 0;
            v->left = v->loop >> 24;
            continue;
        }
        int32_t s = (int32_t)sdram[v->cur] - 128;           /* centred sample */
        acc_l += s * (int32_t)(v->vol & 0xFF);
        acc_r += s * (int32_t)(v->vol >> 8);
        /* Move the place on: byte address and 16-bit fraction as one number, plus the step */
        uint64_t sum = ((uint64_t)v->cur << 16 | v->frac) + v->step;
        uint32_t at = (uint32_t)(sum >> 16) & ADDR_MASK;
        v->frac = (uint32_t)sum & 0xFFFF;
        if (at >= v->end) {
            if (v->left == 0) {             /* no loops left: the voice stops */
                m->playing &= ~bit;
                continue;
            }
            at = (at - (v->loop & 0xFFFFF)) & ADDR_MASK;   /* go back by the loop's length */
            if (v->left != 255) v->left--;                 /* 255 loops for ever */
        }
        v->cur = at;
    }
    if (m->mu_on && m->mu_fill) {           /* the music has started and waits for the line loads */
        m->mu_fill--;
    } else if (m->mu_on) {
        for (int k = 0; k < 2; k++) {
            int32_t s0 = music_sample(m, sdram, m->mu_idx, k), s1 = music_sample(m, sdram, m->mu_idx + 1, k);
            int32_t s = s0 + (int32_t)((int64_t)(s1 - s0) * m->mu_frac >> 16);     /* interpolated */
            int32_t o = (int32_t)((int64_t)s * m->mu_vol >> 15);
            if (k) acc_r += o;
            else acc_l += o;
        }
        m->mu_frac += MIX_MUSIC_STEP;
        m->mu_idx = (m->mu_idx + (m->mu_frac >> 16)) & 0xFFFF;     /* wraps round the 65536 frames */
        m->mu_frac &= 0xFFFF;
    }
    m->out_l = clip(acc_l);
    m->out_r = clip(acc_r);
}
