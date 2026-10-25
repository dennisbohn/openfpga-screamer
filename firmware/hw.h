/*
 * The CPU system of step 1 as the firmware sees it: addresses and registers from
 * docs/soc.md (the contract with core/src/fpga/core/soc.v), CSR access, rdcycle and the
 * CFU instruction. All registers are 32 bits and accessed as words.
 */
#pragma once
#include <stdint.h>

#define CPU_HZ          74250000u           /* step 1: the CPU runs on clk_74a */

/* Boot RAM: code, data and stack (link.ld), loaded from the data slot firmware.bin */
#define BOOTRAM_BASE    0x00000000u
#define BOOTRAM_SIZE    16384u

#define REG32(addr)     (*(volatile uint32_t *)(addr))

/* System registers */
#define SYS_ID          REG32(0xF0000000u)  /* R: SYS_ID_VALUE */
#define SYS_SIM         REG32(0xF0000004u)  /* W: for the testbench only, bit 31 = done */
#define SYS_ID_VALUE    0x53435231u         /* "SCR1" */
#define SIM_DONE        0x80000000u         /* SYS_SIM: done, bits 30..0 = result */
#define SIM_TRAP        0xC0000000u         /* SYS_SIM: trap, bits 29..0 = mcause */

/* Video */
#define VID_STATUS      REG32(0xF0001000u)  /* R: VID_FRONT, VID_PENDING, VID_FRAME() */
#define VID_SWAP        REG32(0xF0001004u)  /* W: any value: swap in the next blanking */
#define VID_PAL         ((volatile uint32_t *)0xF0001400u)  /* W: [256] 0x00RRGGBB, back buffer */
#define VID_FRONT       (1u << 0)           /* the buffer on screen (0/1) */
#define VID_PENDING     (1u << 1)           /* swap requested, not yet done */
#define VID_FRAME(st)   ((st) >> 16)        /* counts every video frame (60 Hz) */

/* Input (cont1_key, cont1_joy, cont1_trig) */
#define IN_KEY          REG32(0xF0002000u)
#define IN_JOY          REG32(0xF0002004u)
#define IN_TRIG         REG32(0xF0002008u)
#define KEY_UP          (1u << 0)
#define KEY_DOWN        (1u << 1)
#define KEY_LEFT        (1u << 2)
#define KEY_RIGHT       (1u << 3)
#define KEY_A           (1u << 4)
#define KEY_B           (1u << 5)
#define KEY_X           (1u << 6)
#define KEY_Y           (1u << 7)
#define KEY_L           (1u << 8)
#define KEY_R           (1u << 9)
#define KEY_SELECT      (1u << 14)
#define KEY_START       (1u << 15)

/* Debug log: the non-volatile data slot debug.txt (dlog.h) */
#define DLOG_BASE       0xF0008000u
#define DLOG_SIZE       4096u

/* Framebuffers: 320 x 200 bytes, palette indices, pixel (x, y) at y * 320 + x. Uncached:
   every store is one bus access, so fill with words where possible. */
#define FB_W            320
#define FB_H            200
#define FB0_BASE        0xF1000000u
#define FB1_BASE        0xF1010000u
#define FB(n)           ((volatile uint8_t *)((n) ? FB1_BASE : FB0_BASE))

/* CSRs by name, e.g. csr_read(mcause) */
#define csr_read(csr)   ({ uint32_t v_; __asm__ volatile ("csrr %0, " #csr : "=r"(v_)); v_; })

/* Cycle counter (low 32 bits: wraps after 58 s at 74.25 MHz, fine for differences) */
static inline uint32_t rdcycle(void)
{
    uint32_t c;
    __asm__ volatile ("csrr %0, cycle" : "=r"(c));
    return c;
}

/* CFU, custom-0 funct7 = 0, funct3 = 0: (a * b) >> 16, signed (16.16 fixed point) */
static inline int32_t fixmul(int32_t a, int32_t b)
{
    int32_t r;
    __asm__ volatile (".insn r 0x0B, 0, 0, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/* Hide a value from the optimizer, so that tests and benchmarks compute at run time */
static inline uint32_t opaque(uint32_t x)
{
    __asm__ volatile ("" : "+r"(x));
    return x;
}

static inline float opaquef(float x)
{
    __asm__ volatile ("" : "+f"(x));
    return x;
}
