/*
 * Reads screamer.cfg (config.h). config_parse() turns the text into the values; it touches no hardware, so
 * tools/perf/config_test.c runs it on the PC. config_load() reads the data slot, parses it, hands the factors
 * to track.c and writes one line to debug.txt.
 */
#include "config.h"
#include "debug.h"
#include "dlog.h"
#include "hw.h"
#include "slot.h"
#include "track.h"

/* The limits of the numbers. A number outside them is moved to the nearest limit. */
#define FAR_MIN     0.5f
#define FAR_MAX     4.0f
#define LOD_MIN     0.5f
#define LOD_MAX     4.0f

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

/* Returns 1 if the name collected from a line equals want. len is the number of letters in the name. Only
   the first 5 are kept in name, so a longer name equals nothing. */
static int name_is(const char *name, int len, const char *want)
{
    int i = 0;
    while (want[i] && i < len && name[i] == want[i]) i++;
    return !want[i] && i == len;
}

/* Reads a number at *p, not beyond end: digits, then optionally a point or comma with more digits. There is
   no sign. Returns 1 and sets *v, and *p is behind the number. Returns 0 if there is no number. Decimal
   places that are too small to matter (below 1e-6) are skipped. */
static int number(const char **p, const char *end, float *v)
{
    const char *s = *p;
    float whole = 0.0f, frac = 0.0f, scale = 1.0f;
    int digits = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        whole = whole * 10.0f + (float)(*s++ - '0');
        digits++;
    }
    if (s < end && (*s == '.' || *s == ',')) {
        s++;
        while (s < end && *s >= '0' && *s <= '9') {
            if (scale > 1e-6f) {
                scale *= 0.1f;
                frac += scale * (float)(*s - '0');
            }
            s++;
            digits++;
        }
    }
    if (!digits) return 0;
    *v = whole + frac;
    *p = s;
    return 1;
}

unsigned config_parse(const char *s, uint32_t n, float *far_scale, float *lod_scale, int *bench)
{
    unsigned found = 0;
    const char *end = s + n;
    while (s < end) {
        /* Cut out one line, which ends at a line feed or a carriage return. Skip the blanks in front of it.
           An empty line or a comment line (# or ;) is ignored. */
        const char *line = s;
        while (s < end && *s != '\n' && *s != '\r') s++;
        const char *le = s;
        while (s < end && (*s == '\n' || *s == '\r')) s++;
        while (line < le && (*line == ' ' || *line == '\t')) line++;
        if (line == le || *line == '#' || *line == ';') continue;

        /* The name is a run of letters, in lower case. Only the first 5 are kept. In a release, "bench" is
           not a known name. */
        char name[6] = { 0 };
        int k = 0;
        while (line < le && lower(*line) >= 'a' && lower(*line) <= 'z') {
            if (k < 5) name[k] = (char)lower(*line);
            k++;
            line++;
        }
        int is_far = name_is(name, k, "far");
        int is_lod = name_is(name, k, "lod");
        int is_bench = DEBUG && name_is(name, k, "bench");

        /* A known name must be followed by = or :. Any other line is not understood. */
        while (line < le && (*line == ' ' || *line == '\t')) line++;
        if (!(is_far || is_lod || is_bench) || line == le || (*line != '=' && *line != ':')) {
            found |= CFG_BAD;
            continue;
        }
        line++;
        while (line < le && (*line == ' ' || *line == '\t')) line++;

        /* The value: for lod a word that starts with o or n ("off", "none") means no level of detail.
           Otherwise it must be a number. */
        float v = 0.0f;
        if (is_lod && line < le && (lower(*line) == 'o' || lower(*line) == 'n')) {
            *lod_scale = 0.0f;
            found |= CFG_LOD;
            continue;
        }
        if (!number(&line, le, &v)) {
            found |= CFG_BAD;
            continue;
        }

        /* Behind the number only blanks and an x may follow, then the end of the line or a comment. */
        while (line < le && (*line == ' ' || *line == '\t' || lower(*line) == 'x')) line++;
        if (line < le && *line != '#' && *line != ';') {
            found |= CFG_BAD;
            continue;
        }

        /* Use the value. far and lod are moved into their limits; a lod of 0 is kept and means none. */
        if (is_far) {
            if (v < FAR_MIN || v > FAR_MAX) found |= CFG_CLAMPED;
            *far_scale = v < FAR_MIN ? FAR_MIN : v > FAR_MAX ? FAR_MAX : v;
            found |= CFG_FAR;
        } else if (is_bench) {
            *bench = v != 0.0f;
            found |= CFG_BENCH;
        } else {
            if (v != 0.0f && (v < LOD_MIN || v > LOD_MAX)) found |= CFG_CLAMPED;
            *lod_scale = v == 0.0f ? 0.0f : v < LOD_MIN ? LOD_MIN : v > LOD_MAX ? LOD_MAX : v;
            found |= CFG_LOD;
        }
    }
    return found;
}

static uint32_t buf[CONFIG_MAX / 4];    /* the file is read into this buffer */
static int bench_on;                    /* "bench = 1" was read (debug build only) */

int config_bench(void)
{
    return DEBUG && bench_on;           /* the benchmark exists in a debug build only (debug.h) */
}

/* Reads the file of data slot CONFIG_SLOT and writes one line to debug.txt: what happened to the file, then
   the factors in force. Without a file, or if it is too big or cannot be read, the factors from track.h
   stay. */
void config_load(void)
{
    float far = track_far_scale(), lod = track_lod_scale();
    uint32_t size = slot_size(CONFIG_SLOT);
    unsigned found = 0;
    if (!size) {
        dlog_str("config: no screamer.cfg, ");
    } else if (size > CONFIG_MAX) {
        dlog_str("config: screamer.cfg is over 1024 bytes, left out, ");
    } else if (slot_read(CONFIG_SLOT, 0, (uint32_t)(uintptr_t)buf, SLOT_REST) != 0) {
        dlog_str("config: screamer.cfg could not be read, ");
    } else {
        found = config_parse((const char *)buf, size, &far, &lod, &bench_on);
        dlog_str("config: screamer.cfg");
        dlog_str(found & CFG_BAD ? " (a line left out)" : "");
        dlog_str(found & CFG_CLAMPED ? " (a number kept within its range)" : "");
        dlog_str(": ");
    }
    track_set_scales(far, lod);
    dlog_str("far ");
    dlog_scale(far);
    dlog_str(" lod ");
    dlog_scale(lod);
    dlog_str(DEBUG && bench_on ? ", benchmark\n" : "\n");
}
