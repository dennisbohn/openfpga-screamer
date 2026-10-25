/*
 * The CPU system of step 1 as the firmware sees it: addresses and registers from
 * docs/soc.md (the contract with core/src/fpga/core/soc.v), CSR access, rdcycle and the
 * CFU instruction. All registers are 32 bits and accessed as words.
 */
#pragma once
#include <stdint.h>

#define CPU_HZ          74250000u           /* the CPU runs on clk_74a */

/* Boot RAM: code, data and stack (link.ld), loaded from the data slot firmware.bin */
#define BOOTRAM_BASE    0x00000000u
#define BOOTRAM_SIZE    32768u

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

/* Step 2 memories (docs/speicher.md). The SDRAM is cached for code and data and has an
   uncached view; the SRAM and PSRAM cram0 are uncached only. Code runs from the boot RAM
   and the SDRAM; fetching from anywhere else is an instruction access fault. */
#define SDRAM_BASE      0x10000000u
#define SDRAM_UBASE     0xF4000000u
#define SDRAM_SIZE      (64u << 20)
#define SRAM_BASE       0xF8000000u
#define SRAM_SIZE       (256u << 10)
#define PSRAM_BASE      0xFA000000u         /* cram0: die 0, die 1 from PSRAM_BASE + 8 MB */
#define PSRAM_SIZE      (16u << 20)

#define SYS_FLAGS       REG32(0xF0000008u)  /* R: FLAG_SIM */
#define MEM_STATUS      REG32(0xF000000Cu)  /* R: MEM_LOCKED ... MEM_WRITES_IDLE */
#define MEM_BRIDGE_WORDS REG32(0xF0000010u) /* R: bridge words into the SDRAM since Reset Enter */
#define MEM_REFRESH     REG32(0xF0000014u)  /* R: AUTO REFRESH commands since power-up */
#define MEM_PHASE       REG32(0xF0000018u)  /* W: PHASE_STEP, PHASE_LATER, PHASE_REINIT; R: PHASE_BUSY, steps in [31:16] */
#define FLAG_SIM        (1u << 0)           /* running in the simulation */
#define MEM_LOCKED      (1u << 0)           /* SDRAM PLL locked */
#define MEM_READY       (1u << 1)           /* SDRAM initialised */
#define MEM_OVERFLOW    (1u << 2)           /* a bridge word found the buffer full */
#define MEM_DROPPED     (1u << 3)           /* a bridge word met an SDRAM not ready */
#define MEM_WRITES_IDLE (1u << 4)           /* every accepted write has been done */
#define PHASE_STEP      (1u << 0)           /* move dram_clk by one step (168 ps) */
#define PHASE_LATER     (1u << 1)           /* ... later, else earlier */
#define PHASE_REINIT    (1u << 8)           /* initialise the SDRAM again */
#define PHASE_BUSY      (1u << 0)
#define PHASE_STEPS(r)  ((int16_t)((r) >> 16))

/* Rasterizer (raster.v): the same pixels as raster.c. Vertices and clip may be written while
   a triangle is drawn; a write to RS_DRAW waits until the rasterizer is free, and so do the
   CPU's framebuffer accesses. */
#define RS_V(n)         REG32(0xF0003000u + 4u * (n))   /* vertex n: [15:0] x, [31:16] y, 1/16 pixel */
#define RS_CLIP0        REG32(0xF000300Cu)  /* [15:0] x0, [31:16] y0, pixels */
#define RS_CLIP1        REG32(0xF0003010u)  /* [15:0] x1, [31:16] y1, pixels, exclusive */
#define RS_DRAW         REG32(0xF0003014u)  /* W: colour | buffer << 8, starts; R: RS_BUSY */
#define RS_PIXELS       REG32(0xF0003018u)  /* R: pixels drawn; W: clears */
#define RS_BUSY         (1u << 0)
#define RS_XY(x, y)     ((uint32_t)(uint16_t)(y) << 16 | (uint16_t)(x))

/* Cache maintenance: invalidate the whole data cache (it is write-through, so nothing is
   lost), and make the instruction cache fetch code written with stores (fence.i, then
   three instructions that may still come from the old line). */
static inline void dcache_invalidate(void)
{
    __asm__ volatile (".word 0x0000500F" ::: "memory");
}

static inline void icache_sync(void)
{
    __asm__ volatile ("fence.i\n\tnop\n\tnop\n\tnop" ::: "memory");
}

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
