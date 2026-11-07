/*
 * Debug log (dlog.h). The log is written with word stores only. The bytes of the word being
 * filled are kept in cur, and the whole word is stored each time a byte is added, so the log
 * is never read back.
 */
#include "hw.h"
#include "console.h"
#include "dlog.h"

#define LOG ((volatile uint32_t *)DLOG_BASE)

static uint32_t pos;            /* the offset of the next byte */
static uint32_t cur;            /* the bytes already in the word at pos & ~3 */
static uint32_t limit;          /* appends stop here; 0 until dlog_init() */
static char last;               /* the last character appended */

void dlog_init(void)
{
    for (uint32_t i = 0; i < DLOG_SIZE / 4; i++)
        LOG[i] = 0;
    pos = DLOG_HEAD;
    cur = 0;
    last = '\n';
    limit = DLOG_SIZE - DLOG_RESERVE;
}

static uint32_t kept;           /* the room held back by dlog_keep() */

void dlog_keep(uint32_t bytes)
{
    kept = bytes;
    if (limit) limit = DLOG_SIZE - DLOG_RESERVE - bytes;
}

uint32_t dlog_kept(void)
{
    return kept;
}

uint32_t dlog_free(void)
{
    return limit > pos ? limit - pos : 0;
}

void dlog_state(uint32_t *p, uint32_t *c)
{
    *p = pos;
    *c = cur;
}

/* The program takes over the log of the boot loader. p and c are the values that the boot
   loader's dlog_state() returned. The log is not cleared. */
void dlog_attach(uint32_t p, uint32_t c)
{
    pos = p;
    cur = c;
    last = '\n';
    limit = DLOG_SIZE - DLOG_RESERVE;
}

void dlog_head(const char *s)
{
    if (!limit)                                     /* before dlog_init() there is no log */
        return;
    for (uint32_t i = 0; i < DLOG_HEAD; i += 4) {   /* one word of the line at a time */
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t c = ' ';                       /* padding after the end of s */
            if (i + k == DLOG_HEAD - 1)
                c = '\n';
            else if (*s)
                c = (uint8_t)*s++;
            w |= c << (8 * k);                      /* little-endian: byte k of the word */
        }
        LOG[i / 4] = w;
    }
}

void dlog_char(char c)
{
    /* The last byte below the limit is kept for '\n', so the last line is always complete. */
    if (pos >= limit || (c != '\n' && pos + 1 >= limit))
        return;
    cur |= (uint32_t)(uint8_t)c << (8 * (pos & 3));
    LOG[pos / 4] = cur;
    if (++pos % 4 == 0)
        cur = 0;                    /* the word is full: the next byte starts a new one */
    last = c;
}

void dlog_scale(float v)
{
    char buf[12];
    fmt_scale(buf, v);
    dlog_str(buf);
}

void dlog_str(const char *s)
{
    while (*s)
        dlog_char(*s++);
}

void dlog_dec(int32_t v)
{
    char buf[12];
    fmt_dec(buf, v);
    dlog_str(buf);
}

void dlog_hex(uint32_t v)
{
    char buf[9];
    fmt_hex(buf, v, 8);
    dlog_str(buf);
}

void dlog_trap(void)
{
    if (!limit) {               /* a trap before main: the header line would stay empty */
        dlog_init();
        dlog_head("Screamer Pocket step 3: trap before main");
    }
    limit = DLOG_SIZE;          /* open the reserve at the end of the log */
    if (last != '\n')
        dlog_char('\n');
}
