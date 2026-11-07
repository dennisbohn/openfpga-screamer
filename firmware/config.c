/*
 * screamer.cfg (config.h).
 */
#include "config.h"
#include "dlog.h"
#include "hw.h"
#include "slot.h"
#include "track.h"

#define FAR_MIN     0.5f
#define FAR_MAX     4.0f
#define LOD_MIN     0.5f
#define LOD_MAX     4.0f
#define TINY_MAX    8.0f

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

/* The number at *p (up to end): digits, a point or comma and more digits, no sign. 0 if there is none; *p is behind it */
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

unsigned config_parse(const char *s, uint32_t n, float *far_scale, float *lod_scale, float *tiny_px)
{
    unsigned found = 0;
    const char *end = s + n;
    while (s < end) {
        const char *line = s;
        while (s < end && *s != '\n' && *s != '\r') s++;
        const char *le = s;
        while (s < end && (*s == '\n' || *s == '\r')) s++;
        while (line < le && (*line == ' ' || *line == '\t')) line++;
        if (line == le || *line == '#' || *line == ';') continue;
        char name[5] = { 0 };
        int k = 0;
        while (line < le && lower(*line) >= 'a' && lower(*line) <= 'z') {
            if (k < 4) name[k] = (char)lower(*line);
            k++;
            line++;
        }
        int is_far = k == 3 && name[0] == 'f' && name[1] == 'a' && name[2] == 'r';
        int is_lod = k == 3 && name[0] == 'l' && name[1] == 'o' && name[2] == 'd';
        int is_tiny = k == 4 && name[0] == 't' && name[1] == 'i' && name[2] == 'n' && name[3] == 'y';
        while (line < le && (*line == ' ' || *line == '\t')) line++;
        if (!(is_far || is_lod || is_tiny) || line == le || (*line != '=' && *line != ':')) {
            found |= CFG_BAD;
            continue;
        }
        line++;
        while (line < le && (*line == ' ' || *line == '\t')) line++;
        float v = 0.0f;
        if ((is_lod || is_tiny) && line < le && (lower(*line) == 'o' || lower(*line) == 'n')) {      /* off, none */
            if (is_lod) {
                *lod_scale = 0.0f;
                found |= CFG_LOD;
            } else {
                *tiny_px = 0.0f;
                found |= CFG_TINY;
            }
            continue;
        }
        if (!number(&line, le, &v)) {
            found |= CFG_BAD;
            continue;
        }
        while (line < le && (*line == ' ' || *line == '\t' || lower(*line) == 'x')) line++;
        if (line < le && *line != '#' && *line != ';') {                  /* something else behind the number */
            found |= CFG_BAD;
            continue;
        }
        if (is_far) {
            if (v < FAR_MIN || v > FAR_MAX) found |= CFG_CLAMPED;
            *far_scale = v < FAR_MIN ? FAR_MIN : v > FAR_MAX ? FAR_MAX : v;
            found |= CFG_FAR;
        } else if (is_tiny) {
            if (v > TINY_MAX) found |= CFG_CLAMPED;
            *tiny_px = v > TINY_MAX ? TINY_MAX : v;
            found |= CFG_TINY;
        } else {
            if (v != 0.0f && (v < LOD_MIN || v > LOD_MAX)) found |= CFG_CLAMPED;
            *lod_scale = v == 0.0f ? 0.0f : v < LOD_MIN ? LOD_MIN : v > LOD_MAX ? LOD_MAX : v;
            found |= CFG_LOD;
        }
    }
    return found;
}

/* "1.50" */
static void log_scale(float v)
{
    int h = (int)(v * 100.0f + 0.5f);
    dlog_dec(h / 100);
    dlog_str(h % 100 < 10 ? ".0" : ".");
    dlog_dec(h % 100);
}

static uint32_t buf[CONFIG_MAX / 4];

void config_load(void)
{
    float far = track_far_scale(), lod = track_lod_scale(), tiny = track_tiny();
    uint32_t size = slot_size(CONFIG_SLOT);
    unsigned found = 0;
    if (!size) {
        dlog_str("config: no screamer.cfg, ");
    } else if (size > CONFIG_MAX) {
        dlog_str("config: screamer.cfg is over 1024 bytes, left out, ");
    } else if (slot_read(CONFIG_SLOT, 0, (uint32_t)(uintptr_t)buf, SLOT_REST) != 0) {
        dlog_str("config: screamer.cfg could not be read, ");
    } else {
        found = config_parse((const char *)buf, size, &far, &lod, &tiny);
        dlog_str("config: screamer.cfg");
        dlog_str(found & CFG_BAD ? " (a line left out)" : "");
        dlog_str(found & CFG_CLAMPED ? " (a number kept within its range)" : "");
        dlog_str(": ");
    }
    track_set_scales(far, lod);
    track_set_tiny(tiny);
    dlog_str("far ");
    log_scale(far);
    dlog_str(" lod ");
    if (lod == 0.0f) dlog_str("off"); else log_scale(lod);
    dlog_str(" tiny ");
    if (tiny == 0.0f) dlog_str("off\n"); else { log_scale(tiny); dlog_str("\n"); }
}
