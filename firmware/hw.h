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
#define VID_FRAME(st)   ((st) >> 16)        /* counts every video frame (VID_HZ) */
#define VID_HZ          50                  /* video frames a second */

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
   uncached view; SRAM and PSRAM are not used. Code runs from the boot RAM and the SDRAM;
   fetching from anywhere else is an instruction access fault. */
#define SDRAM_BASE      0x10000000u
#define SDRAM_UBASE     0xF4000000u
#define SDRAM_SIZE      (64u << 20)
/* The program in the SDRAM (program.bin, data slot 2): a header the boot loader checks,
   then code and data. Above the 32 MB the quick memory test writes in, below its last window. */
#define PROG_BASE       (SDRAM_BASE + 0x3000000u)
#define PROG_MAX        (0x1000000u - 0x800u)
#define PROG_MAGIC      0x50524353u         /* "SCRP" */

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

/* a data slot read from the APF (deferload slots, slot.h) */
#define DS_ID           REG32(0xF0000020u)  /* W: slot id */
#define DS_OFFSET       REG32(0xF0000024u)  /* W: byte offset in the file */
#define DS_ADDR         REG32(0xF0000028u)  /* W: bridge address to load to */
#define DS_LENGTH       REG32(0xF000002Cu)  /* W: bytes, a multiple of 4 */
#define DS_CTRL         REG32(0xF0000030u)  /* W: DS_START; R: DS_BUSY, [6:4] the APF's result, 0 = ok */
#define DS_START        (1u << 0)
#define DS_BUSY         (1u << 0)
#define PHASE_STEPS(r)  ((int16_t)((r) >> 16))

/* Rasterizer (raster.v): the same pixels as raster.c. Vertices and clip may be written while
   a triangle is drawn; a write to RS_DRAW waits until the rasterizer is free, and so do the
   CPU's framebuffer accesses. */
#define RS_V(n)         REG32(0xF0003000u + 4u * (n))   /* vertex n: [15:0] x, [31:16] y, 1/16 pixel */
#define RS_CLIP0        REG32(0xF000300Cu)  /* [15:0] x0, [31:16] y0, pixels */
#define RS_CLIP1        REG32(0xF0003010u)  /* [15:0] x1, [31:16] y1, pixels, exclusive */
#define RS_DRAW         REG32(0xF0003014u)  /* W: colour | buffer << 8, starts; R: RS_BUSY */
#define RS_PIXELS       REG32(0xF0003018u)  /* R: pixels drawn; W: clears */
#define RS_TEX          REG32(0xF000301Cu)  /* texture: SDRAM byte address of texel (0, 0), rows of 256 bytes */
#define RS_TEXSIZE      REG32(0xF0003020u)  /* [8:0] width, [24:16] height (1..256), [31] RS_CUT */
#define RS_REF          REG32(0xF0003024u)  /* reference pixel, RS_XY(x, y) */
#define RS_A(k)         REG32(0xF0003028u + 4u * (k))   /* Q, U, V at the reference pixel (rs_tex_setup) */
#define RS_DX(k)        REG32(0xF0003034u + 4u * (k))   /* their change per pixel in x, 8 fraction bits */
#define RS_DY(k)        REG32(0xF0003040u + 4u * (k))   /* ... in y */
#define RS_CTRL         REG32(0xF000304Cu)  /* W: any value forgets the cached texture lines */
#define RS_MISSES       REG32(0xF0003050u)  /* R: texture lines fetched from the SDRAM; W: clears */
#define RS_CLOCKS       REG32(0xF0003054u)  /* R: clocks the rasterizer was busy; W: clears (and the 5 below) */
#define RS_MISS_CLOCKS  REG32(0xF0003058u)  /* R: clocks the texture pipeline waited for a line */
#define RS_EDGE_CLOCKS  REG32(0xF000305Cu)  /* R: clocks in edge setup */
#define RS_ROW_CLOCKS   REG32(0xF0003060u)  /* R: clocks in the work of a row (span, first pixel) */
#define RS_DRAIN_CLOCKS REG32(0xF0003064u)  /* R: clocks idle while the texture pipeline drained */
#define RS_STARVED      REG32(0xF0003068u)  /* R: clocks idle while the setup or geometry unit worked */
#define RS_BUSY         (1u << 0)
#define RS_TEXTURED     (1u << 9)           /* RS_DRAW: textured with RS_TEX ... RS_DY */
#define RS_CUT          (1u << 31)          /* RS_TEXSIZE: texel 0 is see-through */
#define RS_XY(x, y)     ((uint32_t)(uint16_t)(y) << 16 | (uint16_t)(x))

/* the setup unit (core/src/fpga/core/setup.v, C model setup.h): polygons in view space */
#define REGF(addr)      (*(volatile float *)(addr))
#define SU_CENTRE       REG32(0xF0003080u)  /* W: centre of the view in 1/16 pixel, RS_XY */
#define SU_SCALE        REG32(0xF0003084u)  /* W: focal length in pixels times 16 */
#define SU_MODE         REG32(0xF0003088u)  /* W: as RS_DRAW (colour, buffer, RS_TEXTURED), SU_CORNERS */
#define SU_TEX          REG32(0xF000308Cu)  /* W: as RS_TEX     } taken with the first */
#define SU_TEXSIZE      REG32(0xF0003090u)  /* W: as RS_TEXSIZE } corner of a polygon */
#define SU_R            REGF(0xF0003094u)   /* W: a corner: r, u, f, s (floats) */
#define SU_U            REGF(0xF0003098u)
#define SU_F            REGF(0xF000309Cu)
#define SU_S            REGF(0xF00030A0u)
#define SU_T_NEXT       REGF(0xF00030A4u)   /* W: t; the corner continues the polygon */
#define SU_T_FIRST      REGF(0xF00030A8u)   /* W: t; the corner starts a polygon */
#define SU_CLOCKS       REG32(0xF00030ACu)  /* R: clocks the unit computed; W: clears */
#define SU_FAR          REG32(0xF00030B0u)  /* W: far plane depth with 8 fraction bits */
#define SU_CORNERS(n)   ((uint32_t)(n) << 10)  /* SU_MODE: corners of the polygon, 3 or 4 */

/* the geometry unit (core/src/fpga/core/geom.v, C model geom.h): the pieces of the track */
#define GU_MLB          REG32(0xF0003144u)  /* W: SDRAM byte address of the MLB */
#define GU_MODE         REG32(0xF0003148u)  /* W: [8] the buffer drawn into */
#define GU_FACES        REG32(0xF0003150u)  /* R: faces handed on; W: clears */
#define GU_CLOCKS       REG32(0xF0003154u)  /* R: clocks busy; W: clears */
#define GU_INVAL        REG32(0xF0003158u)  /* W: forget the cached mesh lines */
#define BL_PUSH         REG32(0xF0003180u)  /* W: next word of a blitter command; waits while its buffer is full */
#define BL_CLOCKS       REG32(0xF0003184u)  /* R: clocks busy; W: clears */
#define GU_PUSH         REG32(0xF000315Cu)  /* W: next word of a piece into the buffer; waits while it is full.
                                               18 words: m[9], t[3], e[3], object, block (SDRAM byte
                                               addresses), face row (struct gu_piece) */

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
