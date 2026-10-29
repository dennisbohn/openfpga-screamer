#include "mixer.h"

#define ADDR_MASK   0x3FFFFFFu      /* 26 bits: the SDRAM's 64 MB */

void mix_reset(struct mixer *m)
{
    m->playing = m->fresh = 0;
    m->out_l = m->out_r = 0;
}

void mix_write(struct mixer *m, uint32_t reg, uint32_t value)
{
    if (reg < 0x400) {
        struct mix_voice *v = &m->v[(reg >> 5) & (MIX_VOICES - 1)];
        switch ((reg >> 2) & 7) {
        case 0: v->addr = value & ADDR_MASK; break;
        case 1: v->end = value & ADDR_MASK; break;
        case 2: v->loop = value & 0xFF0FFFFFu; break;
        case 3: v->step = value & 0x3FFFFu; break;
        case 4: v->vol = value & 0xFFFFu; break;
        default: break;
        }
    } else if (reg == 0x400) {
        m->playing |= value;
        m->fresh |= value;
    } else if (reg == 0x404) {
        m->playing &= ~value;
    }
}

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
        if (m->fresh & bit) {
            m->fresh &= ~bit;
            v->cur = v->addr;
            v->frac = 0;
            v->left = v->loop >> 24;
            continue;
        }
        int32_t s = (int32_t)sdram[v->cur] - 128;
        acc_l += s * (int32_t)(v->vol & 0xFF);
        acc_r += s * (int32_t)(v->vol >> 8);
        uint64_t sum = ((uint64_t)v->cur << 16 | v->frac) + v->step;
        uint32_t at = (uint32_t)(sum >> 16) & ADDR_MASK;
        v->frac = (uint32_t)sum & 0xFFFF;
        if (at >= v->end) {
            if (v->left == 0) {
                m->playing &= ~bit;
                continue;
            }
            at = (at - (v->loop & 0xFFFFF)) & ADDR_MASK;
            if (v->left != 255) v->left--;
        }
        v->cur = at;
    }
    m->out_l = clip(acc_l);
    m->out_r = clip(acc_r);
}
