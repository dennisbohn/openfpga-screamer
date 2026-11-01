/*
 * The game's files and the screen that lists them (files.h)
 */
#include "files.h"
#include "hw.h"
#include "console.h"
#include "dlog.h"
#include "slot.h"

/* core/data.json: the slot, the name on the SD card, 1 if the game cannot run without it */
static const struct file { uint8_t slot, must; const char *name; } files[] = {
    {  4, 1, "GFX/GAME.DAT" },   { 20, 1, "GFX/GRAF.DAT" },   { 19, 1, "GFX/MENU.DAT" },
    {  3, 1, "GFX/MAP1H.DAT" },  {  9, 1, "GFX/MAP2H.DAT" },  { 10, 1, "GFX/MAP3H.DAT" },
    { 11, 1, "GFX/MAP4H.DAT" },  { 12, 1, "GFX/MAP5H.DAT" },  { 13, 1, "GFX/MAP6H.DAT" },
    {  6, 1, "DATA/GEN.DAT" },   {  7, 1, "DATA/LOCAL.DAT" }, {  8, 1, "DATA/SOUND.DAT" },
    {  5, 1, "DATA/MAP1.DAT" },  { 14, 1, "DATA/MAP2.DAT" },  { 15, 1, "DATA/MAP3.DAT" },
    { 16, 1, "DATA/MAP4.DAT" },  { 17, 1, "DATA/MAP5.DAT" },  { 18, 1, "DATA/MAP6.DAT" },
    { 25, 0, "DATA/MUSIC.DAT" }, { 22, 0, "SCREAM.gog" },     { 21, 0, "SCREAM.DAT" },
};
#define NFILES  (int)(sizeof files / sizeof files[0])

/* the screen's palette: entries 0 to 4 */
#define BLACK   0
#define WHITE   1
#define GREEN   2
#define RED     3
#define GREY    4

static int present(const struct file *f)
{
    return slot_size(f->slot) != 0;
}

int files_missing(void)
{
    int must = 0, any = 0;
    for (int i = 0; i < NFILES; i++)
        if (!present(&files[i])) {
            must += files[i].must;
            any++;
        }
    dlog_str("game files: ");
    dlog_dec(must);
    dlog_str(" missing");
    if (any) {
        dlog_char(':');
        for (int i = 0; i < NFILES; i++)
            if (!present(&files[i])) {
                dlog_char(' ');
                dlog_str(files[i].name);
            }
    }
    dlog_char('\n');
    return must;
}

void files_draw(volatile uint8_t *fb, int failed)
{
    static const uint32_t pal[5] = { 0x000000, 0xFFFFFF, 0x40D070, 0xF04848, 0x909090 };
    for (int i = 0; i < 5; i++) VID_PAL[i] = pal[i];
    fb_fill(fb, 0, 0, FB_W, FB_H, BLACK);
    fb_fill(fb, 0, 0, FB_W, 8, WHITE);
    con_text(fb, 1, 0, "SCREAMER - GAME FILES", BLACK, WHITE);
    for (int i = 0; i < NFILES; i++) {
        const struct file *f = &files[i];
        int ok = present(f);
        con_text(fb, 1, 1 + i, f->name, WHITE, BLACK);
        con_text(fb, 17, 1 + i, ok ? "[OK]" : "[MISSING]", ok ? GREEN : f->must ? RED : GREY, BLACK);
        if (!f->must) con_text(fb, 28, 1 + i, "*", GREY, BLACK);
    }
    con_text(fb, 1, 22, "* optional, only for the music", GREY, BLACK);
    if (failed) {
        con_text(fb, 1, 23, "A file could not be read:", WHITE, BLACK);
        con_text(fb, 1, 24, "see debug.txt in the Saves folder", WHITE, BLACK);
    } else {
        con_text(fb, 1, 23, "Copy the game files to the SD card:", WHITE, BLACK);
        con_text(fb, 1, 24, "Assets/screamer/common/ (see README)", WHITE, BLACK);
    }
}
