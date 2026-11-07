/* screamer.cfg's parser (firmware/config.c) on texts a player might write, and the overlay's second line.
   Stubs stand in for the log, the slot and the track. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void dlog_str(const char *s) { (void)s; }
void dlog_dec(int32_t v) { (void)v; }
uint32_t slot_size(uint32_t id) { (void)id; return 0; }
int slot_read(uint32_t id, uint32_t o, uint32_t a, uint32_t l) { (void)id; (void)o; (void)a; (void)l; return -1; }
void track_set_scales(float f, float l) { (void)f; (void)l; }
float track_far_scale(void) { return 1.5f; }
float track_lod_scale(void) { return 1.5f; }
#include "../../firmware/config.c"

static int fails;

static void t(const char *text, float far0, float lod0, float far1, float lod1, unsigned flags)
{
    float f = far0, l = lod0;
    int b = 0;
    unsigned got = config_parse(text, (uint32_t)strlen(text), &f, &l, &b);
    int ok = f > far1 - 0.0005f && f < far1 + 0.0005f && l > lod1 - 0.0005f && l < lod1 + 0.0005f && got == flags;
    printf("%-4s far %.3f lod %.3f flags %u  <- \"%s\"%s\n", ok ? "ok" : "FAIL", f, l, got,
           strchr(text, '\n') ? "(lines)" : text, ok ? "" : "  (expected far/lod/flags differ)");
    fails += !ok;
}

int main(void)
{
    {   /* the release's settings */
        int ok = FAR_SCALE == 1.8f && LOD_SCALE == 1.4f;
        printf("%-4s defaults far %.2f lod %.2f\n", ok ? "ok" : "FAIL", (double)FAR_SCALE, (double)LOD_SCALE);
        fails += !ok;
    }
    t("far = 2.0\nlod = 2.5\n", 1.5f, 1.5f, 2.0f, 2.5f, CFG_FAR | CFG_LOD);
    t("", 1.5f, 1.5f, 1.5f, 1.5f, 0);
    t("# comment\n; another\n\n   FAR=1,25  # comma\r\nLod : 3x\r\n", 1.5f, 1.5f, 1.25f, 3.0f, CFG_FAR | CFG_LOD);
    t("lod = 0", 1.5f, 1.5f, 1.5f, 0.0f, CFG_LOD);
    t("lod = off", 1.5f, 1.5f, 1.5f, 0.0f, CFG_LOD);
    t("far = 9", 1.5f, 1.5f, 4.0f, 1.5f, CFG_FAR | CFG_CLAMPED);
    t("far = 0.1\nlod = 0.2", 1.5f, 1.5f, 0.5f, 0.5f, CFG_FAR | CFG_LOD | CFG_CLAMPED);
    t("lod = 12", 1.5f, 1.5f, 1.5f, 4.0f, CFG_LOD | CFG_CLAMPED);
    t("speed = 3\nfar = 2", 1.5f, 1.5f, 2.0f, 1.5f, CFG_FAR | CFG_BAD);
    t("far = \nlod = abc\nfar 2", 1.5f, 1.5f, 1.5f, 1.5f, CFG_BAD);
    t("far = 2 3", 1.5f, 1.5f, 1.5f, 1.5f, CFG_BAD);
    t("far=2.5\nfar=1.75", 1.5f, 1.5f, 1.75f, 1.5f, CFG_FAR);        /* the last one counts */
    t("far = 1.5 # the first value", 1.0f, 1.0f, 1.5f, 1.0f, CFG_FAR);
#if !DEBUG
    {   /* a release does not know bench: the line is left out */
        float f = 1.5f, l = 1.5f;
        int b = 0;
        unsigned got = config_parse("bench = 1\n", 10, &f, &l, &b);
        int ok = got == CFG_BAD && b == 0 && !config_bench();
        printf("%-4s release: bench = 1 is left out, flags %u\n", ok ? "ok" : "FAIL", got);
        fails += !ok;
    }
#else
    {   /* bench (a debug build) */
        float f = 1.5f, l = 1.5f;
        int b = 0;
        unsigned got = config_parse("bench = 1\n", 10, &f, &l, &b);
        int ok = got == CFG_BENCH && b == 1;
        printf("%-4s bench = 1 -> %d flags %u\n", ok ? "ok" : "FAIL", b, got);
        fails += !ok;
        got = config_parse("Bench:0 # off", 13, &f, &l, &b);
        ok = got == CFG_BENCH && b == 0;
        printf("%-4s Bench:0 -> %d flags %u\n", ok ? "ok" : "FAIL", b, got);
        fails += !ok;
        b = 5;
        got = config_parse("bench\nbenchx = 1\n", 17, &f, &l, &b);
        ok = got == CFG_BAD && b == 5;
        printf("%-4s bench without a number, benchx -> flags %u\n", ok ? "ok" : "FAIL", got);
        fails += !ok;
    }
#endif
    return fails ? 1 : 0;
}
