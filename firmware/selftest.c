/*
 * CPU self-tests (selftest.h): integer multiply, divide and shift, single-precision
 * floating point, and the CFU. Expected values are exact results with RISC-V semantics
 * (division by zero, overflow), computed beforehand with Python's integer arithmetic.
 * Inputs reach the instructions through opaque() or inline asm, so nothing is folded at
 * compile time; firmware.lst shows the instructions. A failure is logged as
 * "<test> check <n>: got <result>, expected <value>" (the first four of each test).
 */
#include <math.h>
#include <string.h>
#include "hw.h"
#include "dlog.h"
#include "selftest.h"

static const char *test_name;
static int failures;

static void begin(const char *name)
{
    test_name = name;
    failures = 0;
}

static void check(int n, uint32_t got, uint32_t expected)
{
    if (got == expected)
        return;
    if (failures++ < 4) {
        dlog_str(test_name);
        dlog_str(" check ");
        dlog_dec(n);
        dlog_str(": got ");
        dlog_hex(got);
        dlog_str(", expected ");
        dlog_hex(expected);
        dlog_char('\n');
    }
}

/* One R-type instruction, so the exact instruction is tested (also where C has no
   defined result, like division by zero) */
#define RR(insn, a, b) ({ uint32_t r_; __asm__ volatile (insn " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b)); r_; })

static const uint32_t int_in[8][2] = {
    { 0x00003039u, 0xFFFFE57Bu },       /* 12345, -6789 */
    { 0x12345678u, 0x9ABCDEF0u },
    { 0xFFFFFF9Cu, 0x00000007u },       /* -100, 7 */
    { 0x80000000u, 0xFFFFFFFFu },       /* INT_MIN / -1: overflow */
    { 0xDEADBEEFu, 0x00000000u },       /* division by zero */
    { 0x7FFFFFFFu, 0x7FFFFFFFu },
    { 0xFFFFFFFFu, 0xFFFFFFFFu },
    { 0x00000007u, 0xFFFFFFFEu },       /* 7, -2 */
};

/* mul, mulh, mulhsu, mulhu, div, divu, rem, remu of int_in; check n = 10 * row + column */
static const uint32_t int_out[8][8] = {
    { 0xFB012863u, 0xFFFFFFFFu, 0x00003038u, 0x00003038u, 0xFFFFFFFFu, 0x00000000u, 0x000015B4u, 0x00003039u },
    { 0x242D2080u, 0xF8CC93D6u, 0x0B00EA4Eu, 0x0B00EA4Eu, 0x00000000u, 0x00000000u, 0x12345678u, 0x12345678u },
    { 0xFFFFFD44u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000006u, 0xFFFFFFF2u, 0x24924916u, 0xFFFFFFFEu, 0x00000002u },
    { 0x80000000u, 0x00000000u, 0x80000000u, 0x7FFFFFFFu, 0x80000000u, 0x00000000u, 0x00000000u, 0x80000000u },
    { 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xDEADBEEFu, 0xDEADBEEFu },
    { 0x00000001u, 0x3FFFFFFFu, 0x3FFFFFFFu, 0x3FFFFFFFu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u },
    { 0x00000001u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u },
    { 0xFFFFFFF2u, 0xFFFFFFFFu, 0x00000006u, 0x00000006u, 0xFFFFFFFDu, 0x00000000u, 0x00000001u, 0x00000007u },
};

/* value, shift amount (only bits 4..0 count), sll, srl, sra; check n = 100 + 10 * row + column */
static const uint32_t shifts[5][5] = {
    { 0x87654321u,  4, 0x76543210u, 0x08765432u, 0xF8765432u },
    { 0x87654321u, 31, 0x80000000u, 0x00000001u, 0xFFFFFFFFu },
    { 0x87654321u, 40, 0x65432100u, 0x00876543u, 0xFF876543u },
    { 0x12345678u,  0, 0x12345678u, 0x12345678u, 0x12345678u },
    { 0x12345678u, 63, 0x00000000u, 0x00000000u, 0x00000000u },
};

int selftest_int(void)
{
    begin("integer");
    for (int i = 0; i < 8; i++) {
        uint32_t a = int_in[i][0], b = int_in[i][1];
        const uint32_t *e = int_out[i];
        check(10 * i + 0, RR("mul", a, b), e[0]);
        check(10 * i + 1, RR("mulh", a, b), e[1]);
        check(10 * i + 2, RR("mulhsu", a, b), e[2]);
        check(10 * i + 3, RR("mulhu", a, b), e[3]);
        check(10 * i + 4, RR("div", a, b), e[4]);
        check(10 * i + 5, RR("divu", a, b), e[5]);
        check(10 * i + 6, RR("rem", a, b), e[6]);
        check(10 * i + 7, RR("remu", a, b), e[7]);
    }
    for (int i = 0; i < 5; i++) {
        const uint32_t *s = shifts[i];
        check(100 + 10 * i + 0, RR("sll", s[0], s[1]), s[2]);
        check(100 + 10 * i + 1, RR("srl", s[0], s[1]), s[3]);
        check(100 + 10 * i + 2, RR("sra", s[0], s[1]), s[4]);
    }
    /* the same through the compiler: shifts by immediates, C operators */
    uint32_t v = opaque(0x87654321u);
    check(150, v << 4, 0x76543210u);
    check(151, v >> 4, 0x08765432u);
    check(152, (uint32_t)((int32_t)v >> 4), 0xF8765432u);
    int32_t x = (int32_t)opaque((uint32_t)-100), y = (int32_t)opaque(7);
    check(153, (uint32_t)(x * y), (uint32_t)-700);
    check(154, (uint32_t)(x / y), (uint32_t)-14);
    check(155, (uint32_t)(x % y), (uint32_t)-2);
    return failures == 0;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

int selftest_float(void)
{
    begin("float");
    float a = opaquef(1.5f), b = opaquef(2.25f), one = opaquef(1.0f);
    check(1, fbits(a * b), 0x40580000u);                                /* fmul.s: 3.375 */
    check(2, fbits(a + b), 0x40700000u);                                /* fadd.s: 3.75 */
    check(3, fbits(opaquef(0.1f) + opaquef(0.2f)), 0x3E99999Au);        /* rounded: 0.3f */
    check(4, fbits(opaquef(1.1f) * opaquef(1.1f)), 0x3F9AE148u);        /* rounded: 1.21000004f */
    check(5, fbits(opaquef(1.0000001f) - one), 0x34000000u);            /* fsub.s: 2^-23 */
    check(6, fbits(opaquef(10.0f) / opaquef(4.0f)), 0x40200000u);       /* fdiv.s: 2.5 */
    check(7, fbits(one / opaquef(3.0f)), 0x3EAAAAABu);                  /* rounded: 0.333333343f */
    check(8, fbits(sqrtf(b)), 0x3FC00000u);                             /* fsqrt.s: 1.5 */
    check(9, fbits(sqrtf(opaquef(2.0f))), 0x3FB504F3u);                 /* rounded: 1.41421354f */
    /* fmadd.s, exact: VexRiscv's FMA rounds the product early (FpuCore.scala, "FMA precision
       lost"), so a case that needs a true fused result would fail */
    check(10, fbits(__builtin_fmaf(a, b, opaquef(0.125f))), 0x40600000u);    /* 3.5 */
    check(11, fbits((float)(int32_t)opaque((uint32_t)-123456789)), 0xCCEB79A3u);   /* fcvt.s.w: -123456792 */
    check(12, fbits((float)opaque(0xFFFFFFFFu)), 0x4F800000u);          /* fcvt.s.wu: 2^32 */
    check(13, (uint32_t)(int32_t)opaquef(-2.75f), 0xFFFFFFFEu);         /* fcvt.w.s, toward 0: -2 */
    check(14, (uint32_t)(int32_t)opaquef(1234567.875f), 1234567u);
    check(15, (uint32_t)opaquef(3.0e9f), 3000000000u);                  /* fcvt.wu.s */
    check(16, (uint32_t)((a < b) + 2 * (b <= a) + 4 * (a == opaquef(1.5f))), 5u);  /* flt, fle, feq */
    check(17, fbits(-a), 0xBFC00000u);                                  /* fneg.s */
    check(18, fbits(fabsf(opaquef(-2.25f))), 0x40100000u);              /* fabs.s */
    return failures == 0;
}

/* fixmul(a, b) must equal ((int64_t)a * b) >> 16, i.e. bits 47..16 of the product */
static const int32_t cfu_in[][2] = {
    {  0x00018000,  0x00024000 },       /* 1.5 * 2.25 = 3.375 */
    { -0x00018000,  0x00024000 },
    {  0x00018000, -0x00024000 },
    { -0x00018000, -0x00024000 },
    { -1, 1 },                          /* -1/65536 rounds down: -1 */
    {  1, -1 },
    {  3, 0x5555 },                     /* 0xFFFF >> 16 = 0 */
    { -3, 0x5555 },                     /* -0xFFFF >> 16 = -1 */
    {  0x00010000, 0x12345678 },        /* 1.0 * x = x */
    { -0x00010000, 0x12345678 },
    {  0x12345678, (int32_t)0x9ABCDEF0 },
    {  INT32_MAX, INT32_MAX },          /* the top 16 bits of the product are dropped */
    {  INT32_MIN, INT32_MIN },
    {  INT32_MIN, INT32_MAX },
};

int selftest_cfu(void)
{
    begin("cfu");
    check(0, (uint32_t)fixmul((int32_t)opaque(0x00018000), (int32_t)opaque(0x00024000)), 0x00036000u);
    for (int i = 0; i < (int)(sizeof cfu_in / sizeof cfu_in[0]); i++) {
        int32_t a = (int32_t)opaque((uint32_t)cfu_in[i][0]), b = (int32_t)opaque((uint32_t)cfu_in[i][1]);
        check(i + 1, (uint32_t)fixmul(a, b), (uint32_t)(int32_t)(((int64_t)a * b) >> 16));
    }
    return failures == 0;
}
