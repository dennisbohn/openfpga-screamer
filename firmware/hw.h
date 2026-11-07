/*
 * The CPU system as the firmware sees it: addresses and registers (the contract is docs/soc.md
 * and core/src/fpga/core/soc.v), CSR access, the cycle counter, cache maintenance and the CFU
 * instruction. Every firmware source includes this file.
 *
 * All registers are 32 bits wide and are accessed as words. The comment of a register starts
 * with its direction (R, W or R/W), then gives the bit fields as [high:low] (or "32 bits" for
 * one value), the unit in parentheses, and the meaning.
 */
#pragma once
#include <stdint.h>

#define CPU_HZ          74250000u           /* the CPU runs on clk_74a, 74.25 MHz */

/* Boot RAM, 32 KB: the boot loader's code, data and stack (link.ld). The Pocket loads it from
   the data slot firmware.bin. The program's stack is at the top of it, too (link_prog.ld). */
#define BOOTRAM_BASE    0x00000000u
#define BOOTRAM_SIZE    32768u

#define REG32(addr)     (*(volatile uint32_t *)(addr))

/* System registers */
#define SYS_ID          REG32(0xF0000000u)  /* R, 32 bits: SYS_ID_VALUE */
#define SYS_SIM         REG32(0xF0000004u)  /* W, 32 bits: result for the testbench (SIM_DONE, SIM_TRAP).
                                               No effect on the Pocket */
#define SYS_ID_VALUE    0x53435231u         /* "SCR1" */
#define SIM_DONE        0x80000000u         /* SYS_SIM: bit 31 = done, bits 29..0 = result */
#define SIM_TRAP        0xC0000000u         /* SYS_SIM: done with bit 30 set = trap, bits 29..0 = mcause */

/* Video */
#define VID_STATUS      REG32(0xF0001000u)  /* R, [0] VID_FRONT, [1] VID_PENDING, [31:16] VID_FRAME() */
#define VID_SWAP        REG32(0xF0001004u)  /* W, 32 bits: any value requests a swap in the next
                                               blanking; ignored while one is pending */
#define VID_PAL         ((volatile uint32_t *)0xF0001400u)  /* W, 256 entries of 32 bits, 0x00RRGGBB:
                                                               palette of the back buffer. Write it only
                                                               while no swap is pending, else it hits
                                                               the buffer on screen */
#define VID_FRONT       (1u << 0)           /* the buffer on screen (0 or 1) */
#define VID_PENDING     (1u << 1)           /* a swap is requested and not yet done */
#define VID_FRAME(st)   ((st) >> 16)        /* frame counter from a VID_STATUS value; counts every
                                               video frame (VID_HZ) */
#define VID_HZ          50                  /* video frames a second */

/* Input: the APF's cont1_key, cont1_joy and cont1_trig */
#define IN_KEY          REG32(0xF0002000u)  /* R, [15:0] the KEY_ bits, [31:28] device type */
#define IN_JOY          REG32(0xF0002004u)  /* R, [7:0] left X, [15:8] left Y, [23:16] right X,
                                               [31:24] right Y (unsigned, 0x80 = middle) */
#define IN_TRIG         REG32(0xF0002008u)  /* R, [7:0] left trigger, [15:8] right trigger */
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
/* A stick value of 0 means left or up. A trigger value of 0 means released.
   Only a docked controller with sticks has them: device type 3 in IN_KEY [31:28]. Its sticks
   read 0 until it reports. IN_ANALOG(keys, joy) is true once such a controller has reported. */
#define IN_ANALOG(keys, joy)    ((keys) >> 28 == 3 && ((joy) & 0xFFFFu) != 0)

/* The keys, plus the left stick of such a controller as the d-pad, when it is pushed more than
   half way (for the menus). */
static inline uint32_t in_keys(void)
{
    uint32_t keys = IN_KEY, joy = IN_JOY;
    if(IN_ANALOG(keys, joy)) {
        uint32_t x = joy & 0xFF, y = (joy >> 8) & 0xFF;
        keys |= (x < 0x40 ? KEY_LEFT : 0) | (x >= 0xC0 ? KEY_RIGHT : 0)
              | (y < 0x40 ? KEY_UP : 0) | (y >= 0xC0 ? KEY_DOWN : 0);
    }
    return keys;
}

/* Debug log: 4 KB of text, the non-volatile data slot debug.txt (dlog.h). Byte, halfword and
   word accesses work, reads too. */
#define DLOG_BASE       0xF0008000u
#define DLOG_SIZE       4096u

/* Framebuffers: 320 x 200 bytes (palette indices), pixel (x, y) at offset y * 320 + x. They are
   uncached: every store is one bus access, so fill with words where possible. */
#define FB_W            320
#define FB_H            200
#define FB0_BASE        0xF1000000u
#define FB1_BASE        0xF1010000u
#define FB(n)           ((volatile uint8_t *)((n) ? FB1_BASE : FB0_BASE))

/* Memories (docs/memory.md). The core uses only the SDRAM; the Pocket's SRAM and PSRAM are
   not used. The SDRAM is cached for code and data, and it has an uncached view of the same
   memory. Code runs only from the boot RAM and the SDRAM; fetching from anywhere else is an
   instruction access fault. */
#define SDRAM_BASE      0x10000000u         /* the cached view */
#define SDRAM_UBASE     0xF4000000u         /* the uncached view of the same memory */
#define SDRAM_SIZE      (64u << 20)         /* 64 MB */
/* The program in the SDRAM (program.bin, data slot 2): a 16-byte header that the boot loader
   checks, then code and data. It lies above the 32 MB that the memory test writes in. The file
   is at most PROG_MAX, the 1 MB of the data slot. Together with its bss the program may use the
   15 MB up to the saved games (link_prog.ld, save.h). */
#define PROG_BASE       (SDRAM_BASE + 0x3000000u)
#define PROG_MAX        0x100000u
#define PROG_MAGIC      0x50524353u         /* "SCRP" */

#define SYS_FLAGS       REG32(0xF0000008u)  /* R, [0] FLAG_SIM */
#define MEM_STATUS      REG32(0xF000000Cu)  /* R, [4:0] MEM_LOCKED to MEM_WRITES_IDLE */
#define MEM_BRIDGE_WORDS REG32(0xF0000010u) /* R, 32 bits (words): bridge words into the SDRAM since
                                               power-up or Reset Enter */
#define MEM_REFRESH     REG32(0xF0000014u)  /* R, 32 bits (commands): AUTO REFRESH commands sent to the
                                               SDRAM since power-up */
#define MEM_PHASE       REG32(0xF0000018u)  /* W, [0] PHASE_STEP, [1] PHASE_LATER, [8] PHASE_REINIT.
                                               R, [0] PHASE_BUSY, [31:16] steps moved since start
                                               (signed, 80 steps = one clock period) */
#define FLAG_SIM        (1u << 0)           /* SYS_FLAGS: running in the simulation */
#define MEM_LOCKED      (1u << 0)           /* MEM_STATUS: the SDRAM PLL is locked */
#define MEM_READY       (1u << 1)           /* MEM_STATUS: the SDRAM is initialised */
#define MEM_OVERFLOW    (1u << 2)           /* MEM_STATUS: a bridge word found the buffer full */
#define MEM_DROPPED     (1u << 3)           /* MEM_STATUS: a bridge word met an SDRAM that was not ready */
#define MEM_WRITES_IDLE (1u << 4)           /* MEM_STATUS: every accepted write has been done */
#define PHASE_STEP      (1u << 0)           /* MEM_PHASE: move dram_clk by one step (168 ps) */
#define PHASE_LATER     (1u << 1)           /* MEM_PHASE: move it later (else earlier) */
#define PHASE_REINIT    (1u << 8)           /* MEM_PHASE: re-initialise the SDRAM */
#define PHASE_BUSY      (1u << 0)           /* MEM_PHASE: a phase step is in progress */

/* Data slot reads from the APF (the deferload slots, slot.h). Set DS_ID, DS_OFFSET, DS_ADDR and
   DS_LENGTH, then write DS_START to DS_CTRL. */
#define DS_ID           REG32(0xF0000020u)  /* W, 32 bits: slot id */
#define DS_OFFSET       REG32(0xF0000024u)  /* W, 32 bits (bytes): offset in the file */
#define DS_ADDR         REG32(0xF0000028u)  /* W, 32 bits: bridge address to load to */
#define DS_LENGTH       REG32(0xF000002Cu)  /* W, 32 bits (bytes): a multiple of 4, or SLOT_REST */
#define DS_CTRL         REG32(0xF0000030u)  /* W, [0] DS_START.
                                               R, [0] DS_BUSY, [1] DS_SIZING, [6:4] the APF's result
                                               (0 = ok) */
#define DS_SIZE         REG32(0xF0000034u)  /* R, 32 bits (bytes): size of the file of slot DS_ID from
                                               the APF's slot table, 0 if there is none.
                                               W, 32 bits (bytes): enters that size into the table
                                               (for a nonvolatile slot, which is saved on exit) */
#define DS_START        (1u << 0)           /* DS_CTRL: start the request */
#define DS_BUSY         (1u << 0)           /* DS_CTRL: the request is still running */
#define DS_SIZING       (1u << 1)           /* DS_CTRL: DS_SIZE is being looked up, which happens
                                               after each write of DS_ID */
#define SLOT_REST       0xFFFFFFFFu         /* DS_LENGTH: the rest of the file; the APF sizes it */
#define PHASE_STEPS(r)  ((int16_t)((r) >> 16))  /* steps moved, from a MEM_PHASE read */

/* The rasterizer (core/src/fpga/core/raster.v), which draws the same pixels as raster.c.
   Vertices and clip may be written while a triangle is drawn. A write to RS_DRAW waits until the
   rasterizer is free, and so do the CPU's framebuffer accesses. */
#define RS_V(n)         REG32(0xF0003000u + 4u * (n))   /* W, [15:0] x, [31:16] y (1/16 pixel, signed):
                                                           vertex n */
#define RS_CLIP0        REG32(0xF000300Cu)  /* W, [15:0] x0, [31:16] y0 (pixels): clip, first corner */
#define RS_CLIP1        REG32(0xF0003010u)  /* W, [15:0] x1, [31:16] y1 (pixels): clip, corner after
                                               the last pixel (exclusive) */
#define RS_DRAW         REG32(0xF0003014u)  /* W, [7:0] colour, [8] buffer, [9] RS_TEXTURED: starts a
                                               triangle. R, [0] RS_BUSY */
#define RS_PIXELS       REG32(0xF0003018u)  /* R, 32 bits (pixels): pixels drawn. W: clears */
#define RS_TEX          REG32(0xF000301Cu)  /* W, 32 bits (bytes): SDRAM byte address of texel (0, 0).
                                               The texture has rows of 256 bytes */
#define RS_TEXSIZE      REG32(0xF0003020u)  /* W, [8:0] width, [24:16] height (texels, 1..256),
                                               [29] RS_WRAP, [30] RS_BLEND, [31] RS_CUT */
#define RS_REF          REG32(0xF0003024u)  /* W, reference pixel as RS_XY(x, y) (pixels) */
#define RS_A(k)         REG32(0xF0003028u + 4u * (k))   /* W, 32 bits: Q, U, V (k = 0, 1, 2) at the
                                                           reference pixel (rs_tex_setup) */
#define RS_DX(k)        REG32(0xF0003034u + 4u * (k))   /* W, 32 bits (8 fraction bits): change of
                                                           Q, U, V per pixel in x */
#define RS_DY(k)        REG32(0xF0003040u + 4u * (k))   /* W, 32 bits (8 fraction bits): the same in y */
#define RS_CTRL         REG32(0xF000304Cu)  /* W, 32 bits: any value forgets the cached texture lines */
#define RS_MISSES       REG32(0xF0003050u)  /* R, 32 bits (lines): texture lines fetched from the SDRAM.
                                               W: clears */
#define RS_CLOCKS       REG32(0xF0003054u)  /* R, 32 bits (clocks): clocks the rasterizer was busy.
                                               W: clears this counter and the five below */
#define RS_MISS_CLOCKS  REG32(0xF0003058u)  /* R, 32 bits (clocks): clocks the texture pipeline waited
                                               for a line */
#define RS_EDGE_CLOCKS  REG32(0xF000305Cu)  /* R, 32 bits (clocks): clocks in edge setup */
#define RS_ROW_CLOCKS   REG32(0xF0003060u)  /* R, 32 bits (clocks): clocks in the work of a row (span,
                                               first pixel) */
#define RS_DRAIN_CLOCKS REG32(0xF0003064u)  /* R, 32 bits (clocks): clocks idle while the texture
                                               pipeline drained */
#define RS_STARVED      REG32(0xF0003068u)  /* R, 32 bits (clocks): clocks idle while the setup unit
                                               or the geometry unit worked */
#define RS_SMG          REG32(0xF000306Cu)  /* W, [21:8] row << 8 | old pixel, [7:0] new pixel: one
                                               entry of the see-through table (MAPn.SMG, raster.h
                                               rs_smg). It is not held back: write it only while
                                               nothing blended is drawn */
#define RS_BUSY         (1u << 0)           /* RS_DRAW: the rasterizer is drawing */
#define RS_TEXTURED     (1u << 9)           /* RS_DRAW: textured, with RS_TEX to RS_DY */
#define RS_CUT          (1u << 31)          /* RS_TEXSIZE: texel 0 is see-through */
#define RS_BLEND        (1u << 30)          /* RS_TEXSIZE: routine 224, pixel = table[texel & 63][pixel] */
#define RS_WRAP         (1u << 29)          /* RS_TEXSIZE: routine 160, u and v wrap at 256 (not clamped) */
#define RS_XY(x, y)     ((uint32_t)(uint16_t)(y) << 16 | (uint16_t)(x))

/* The setup unit (core/src/fpga/core/setup.v, C model setup.h): turns polygons in view space
   into triangles for the rasterizer. A polygon is sent corner by corner (SU_R to SU_T_*). */
#define REGF(addr)      (*(volatile float *)(addr))
#define SU_CENTRE       REG32(0xF0003080u)  /* W, as RS_XY (1/16 pixel): centre of the view */
#define SU_SCALE        REG32(0xF0003084u)  /* W, 32 bits: focal length in pixels times 16 */
#define SU_MODE         REG32(0xF0003088u)  /* W, as RS_DRAW (colour, buffer, RS_TEXTURED) plus the
                                               number of corners, SU_CORNERS */
#define SU_TEX          REG32(0xF000308Cu)  /* W, as RS_TEX     } taken with the first */
#define SU_TEXSIZE      REG32(0xF0003090u)  /* W, as RS_TEXSIZE } corner of a polygon */
#define SU_R            REGF(0xF0003094u)   /* W, float: corner, right (view space) */
#define SU_U            REGF(0xF0003098u)   /* W, float: corner, up */
#define SU_F            REGF(0xF000309Cu)   /* W, float: corner, forward (depth) */
#define SU_S            REGF(0xF00030A0u)   /* W, float: corner, texel column */
#define SU_T_NEXT       REGF(0xF00030A4u)   /* W, float: texel row; the corner continues the polygon */
#define SU_T_FIRST      REGF(0xF00030A8u)   /* W, float: texel row; the corner starts a polygon */
#define SU_CLOCKS       REG32(0xF00030ACu)  /* R, 32 bits (clocks): clocks the unit computed. W: clears */
#define SU_FAR          REG32(0xF00030B0u)  /* W, 32 bits (8 fraction bits): depth of the far plane */
#define SU_CORNERS(n)   ((uint32_t)(n) << 10)  /* SU_MODE [12:10]: corners of the polygon, 3 or 4 */

/* The geometry unit (core/src/fpga/core/geom.v, C model geom.h): draws the pieces of the track. */
#define GU_MLB          REG32(0xF0003144u)  /* W, 32 bits: SDRAM byte address of the MLB file */
#define GU_MODE         REG32(0xF0003148u)  /* W, [8] the buffer drawn into */
#define GU_FACES        REG32(0xF0003150u)  /* R, 32 bits (faces): faces handed on. W: clears */
#define GU_CLOCKS       REG32(0xF0003154u)  /* R, 32 bits (clocks): clocks busy. W: clears */
#define GU_INVAL        REG32(0xF0003158u)  /* W, 32 bits: any value forgets the cached mesh lines */
#define GU_PUSH         REG32(0xF000315Cu)  /* W, 32 bits: next word of a piece; waits while the piece
                                               buffer is full. A piece is 19 words (struct gu_piece):
                                               m[9] matrix, t[3] origin, e[3] eye, mesh object and
                                               block (SDRAM byte addresses), face row and phase,
                                               reflection */
/* The blitter (core/src/fpga/core/blit.v, C model blit.h) */
#define BL_PUSH         REG32(0xF0003180u)  /* W, 32 bits: next word of a command; waits while the
                                               command buffer is full */
#define BL_CLOCKS       REG32(0xF0003184u)  /* R, 32 bits (clocks): clocks busy. W: clears */

/* The sound mixer (core/src/fpga/core/audio.v, C model mixer.h): 32 voices of 8-bit unsigned
   samples in the SDRAM, 48 kHz stereo. A voice's registers take effect in the next pass. */
#define AU_VOICES       32
#define AU_RATE         48000               /* output frames a second */
#define AU_ADDR(v)      REG32(0xF0004000u + 32u * (v))  /* W, 32 bits: SDRAM byte address of the first
                                                           byte of the sample */
#define AU_END(v)       REG32(0xF0004004u + 32u * (v))  /* W, 32 bits: SDRAM byte address where the
                                                           sample (or its loop) ends */
#define AU_LOOP(v)      REG32(0xF0004008u + 32u * (v))  /* W, [19:0] (bytes) go back by this much at the
                                                           end, [31:24] repeat that often (0 never,
                                                           255 for ever) */
#define AU_STEP(v)      REG32(0xF000400Cu + 32u * (v))  /* W, 32 bits (samples, 2.16 fixed point, below 4):
                                                           step per output frame */
#define AU_VOL(v)       REG32(0xF0004010u + 32u * (v))  /* W, [7:0] left, [15:8] right volume */
#define AU_PLAY         REG32(0xF0004400u)  /* W, bit v: (re)starts voice v. R, bit v: voice v plays */
#define AU_STOP         REG32(0xF0004404u)  /* W, bit v: stops voice v */
#define AU_STATUS       REG32(0xF0004408u)  /* R, [0] AU_LATE. W: clears */
#define AU_LATE         (1u << 0)           /* AU_STATUS: a pass was not done by the next frame */
/* The music: 16-bit stereo frames at 44.1 kHz (as on the CD) in a ring of 256 KB, one frame
   per word. */
#define AU_MUSIC_ADDR   REG32(0xF000440Cu)  /* W, [25:18]: SDRAM byte address of the ring (256 KB
                                               aligned); write it before a start */
#define AU_MUSIC_VOL    REG32(0xF0004410u)  /* W, [15:0]: each sample is multiplied by this value
                                               and shifted right by 15 */
#define AU_MUSIC        REG32(0xF0004414u)  /* W, [0]: 1 plays the ring from its start, 0 stops.
                                               R, [15:0] place (frame in the ring), [16] plays */
#define AU_MUSIC_RING   0x40000u            /* size of the ring in bytes */

/* The FM music (core/src/fpga/core/opl.v, fm.h): the AdLib's OPL2 chip, fed through a queue of
   time-stamped register writes. Its output joins the mixer's sums, mono. Time is counted in the
   chip's samples, 3579545 / 72 = 49715.9 a second. */
#define FM_CTRL         REG32(0xF0005000u)  /* R/W, [0] run: 0 holds the chip in reset (silent) and the
                                               queue empty; 1 starts it, ready 36 us later */
#define FM_WRITE        REG32(0xF0005004u)  /* W, [31:16] time, [15:8] chip register, [7:0] value: one
                                               write for the chip. The time is the low 16 bits of
                                               the FM_TIME at which it is made. Ignored when the
                                               queue is full or run is 0 */
#define FM_STATUS       REG32(0xF0005008u)  /* R, [8:0] writes in the queue (0..256), [16] the chip is
                                               ready, [17] a write is going on */
#define FM_TIME         REG32(0xF000500Cu)  /* R, [15:0] (chip samples): time since power-up; wraps
                                               after 1.3 s */
#define FM_GAIN         REG32(0xF0005010u)  /* W, [7:0]: the output is multiplied by this value / 32
                                               (32 at power-up) */
#define FM_QUEUE        256                 /* writes the queue holds */

/* The Pocket menu's Music Source (core/interact.json). The Pocket writes it over the bridge to
   0x2000_0000, and it is kept in a register that no reset clears. R, 32 bits: 0 is Auto, 1 is
   FM Synth (any non-zero value counts as 1). */
#define OPT_MUSIC       REG32(0xF0000040u)

/* Cache maintenance. dcache_invalidate() invalidates the whole data cache; the cache is
   write-through, so nothing is lost. The word 0x0000500F is the instruction that does it.
   icache_sync() makes the instruction cache fetch code that was written with stores: it
   executes fence.i, then three instructions that may still come from the old line. */
static inline void dcache_invalidate(void)
{
    __asm__ volatile (".word 0x0000500F" ::: "memory");
}

static inline void icache_sync(void)
{
    __asm__ volatile ("fence.i\n\tnop\n\tnop\n\tnop" ::: "memory");
}

/* Read a CSR by name, e.g. csr_read(mcause) */
#define csr_read(csr)   ({ uint32_t v_; __asm__ volatile ("csrr %0, " #csr : "=r"(v_)); v_; })

/* Cycle counter. It returns the low 32 bits, which wrap after 58 s at 74.25 MHz; that is fine
   for differences. It is 0 on the PC, where track.c and track_build.c run in the host tests. */
static inline uint32_t rdcycle(void)
{
    uint32_t c;
#ifdef __riscv
    __asm__ volatile ("csrr %0, cycle" : "=r"(c));
#else
    c = 0;
#endif
    return c;
}

/* The CFU (custom instruction), custom-0 with funct7 = 0 and funct3 = 0: (a * b) >> 16 as a
   signed multiply, i.e. a 16.16 fixed-point product. */
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
