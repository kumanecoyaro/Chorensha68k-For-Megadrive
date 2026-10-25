/*
 * Title menu (replaces fn_02dac, called by the original top level L03e60).
 *
 * The original menu is START / CONTINUE / CONFIGURE / CREDITS / exit, from
 * text row 20 down (cut by the Mega Drive's 224 lines); CONFIGURE is an
 * X68000 settings screen.  Here:
 *
 *       EASY  NORMAL  HARD          (left / right, A or B: start)
 *       CONTINUE
 *       CREDITS                     (the original credits screen)
 *       exit                        (does nothing)
 *
 * The difficulty is set with the original options: GAME LEVEL B_4a0f8
 * (0 normal, 1 impossible) and the enemy bullet speed TD_SPEED B_4a106
 * (hidden option, $10): EASY = normal with slower bullets ($0C),
 * NORMAL = normal, HARD = impossible.  The return value is the original's:
 * 0 start, 1 continue.
 */
#include "game.h"
#include "orig.h"

long ocall(void *fn, long a1, long a2, long a3, long a4);
long x_text_clear(void);
long x_text_putc(long x, long y, long ch);
long x_xsp_out0(void);
long x_wait(long n);
long x_screen_clear(long mode);
long x_bgm_play(long no);
long x_joybtn(long port);
long x_joydir(long port);
void xsp_call(s16 x, s16 y, u16 id, u16 info);
void text_map_set(u8 kind);

#define ROW0   17                       /* first menu row (text rows of 8 lines) */
#define CURSOR 5                        /* the original's cursor character */

static const char *const items[4] = { 0, "CONTINUE", "CREDITS", "exit" };
static const struct { u8 col; const char *s; u8 level, td_speed; } diff[3] = {
    { 7, "EASY", 0, 0x0C }, { 13, "NORMAL", 0, 0x10 }, { 21, "HARD", 1, 0x10 },
};
static u8 sel_diff = 1;

static void puts_at(u8 x, u8 y, const char *s)
{
    while (*s)
        x_text_putc(x++, y, *s++);
}

static void draw_menu(void)
{
    u8 i;
    x_text_clear();
    ocall(OFN(0x022b6), 0, 0, 0, 0);    /* score line */
    for (i = 0; i < 3; i++)
        puts_at(diff[i].col, ROW0, diff[i].s);
    for (i = 1; i < 4; i++)
        puts_at(13, ROW0 + 2 * i, items[i]);
}

static void draw_cursor(u8 row, u8 on)
{
    u8 col = row ? 12 : diff[sel_diff].col - 1;
    x_text_putc(col, ROW0 + 2 * row, on ? CURSOR : ' ');
}

/* the logo slides in (as fn_02dac: title ref 0, y $4400 -> 0 by 1/16 a frame) */
static void logo_slide(void)
{
    s16 y = 0x4400;
    u8 i;
    for (i = 0; i < 0x40; i++) {
        y -= y / 16;
        xsp_call(0x400, y, 0, 0x13F);
        x_xsp_out0();
    }
}

/* left alone on the menu: the ranking (as fn_02dac: after $180 frames,
 * fn_011ce at row 10, for $100 frames or until a button / direction) */
static u8 pad_edge(u8 *last);
static u8 pad_now_any(u8 *last) { return pad_edge(last) != 0; }
#define IDLE_RANKING 0x180
static void show_ranking(u8 *last)
{
    u16 t;
    x_text_clear();
    text_map_set(2);                    /* (rows 5-28: one row up) */
    ocall(OFN(0x011ce), 0, 10, 0, 0);
    for (t = 0; t < 0x100; t++) {
        x_wait(0);
        if (pad_now_any(last))
            break;
    }
    text_map_set(0);
}

static u8 pad_edge(u8 *last)
{
    u8 now = (x_joybtn(1) ? 0x10 : 0), d = x_joydir(1), e;
    if (d == 8) now |= 1;               /* up */
    if (d == 2) now |= 2;               /* down */
    if (d == 4) now |= 4;               /* left */
    if (d == 6) now |= 8;               /* right */
    e = now & ~*last;
    *last = now;
    return e;
}

long x_title_menu(void)
{
    u8 row, last = 0xFF, e;
    u16 idle = 0;
    x_bgm_play(0x10);
    x_text_clear();
    x_xsp_out0();
    x_xsp_out0();
    x_screen_clear(0);                  /* graphic screen off (the last game's BG) */
    ocall(OFN(0x051b8), 0, 0, 0, 0);    /* palettes */
    logo_slide();
    OL(0x4a0ea) = OL(0x4a0dc);          /* (as the original: the score shown) */
    row = OW(0x82018) ? 1 : 0;          /* after a game: CONTINUE */
    draw_menu();
    draw_cursor(row, 1);
    for (;;) {
        x_wait(0);
        e = pad_edge(&last);
        if (last)
            idle = 0;
        else if (++idle >= IDLE_RANKING) {
            show_ranking(&last);
            draw_menu();
            draw_cursor(row, 1);
            idle = 0;
            continue;
        }
        if (e & 3) {
            draw_cursor(row, 0);
            row = (row + (e & 1 ? 3 : 1)) & 3;
            draw_cursor(row, 1);
        } else if ((e & 12) && row == 0) {
            draw_cursor(0, 0);
            if (e & 4)
                sel_diff = sel_diff ? sel_diff - 1 : 2;
            else
                sel_diff = sel_diff < 2 ? sel_diff + 1 : 0;
            draw_cursor(0, 1);
        } else if (e & 0x10) {
            if (row <= 1) {
                OW(0x4a0f8) = diff[sel_diff].level;
                OW(0x4a106) = diff[sel_diff].td_speed;
                OW(0x82018) = -1;
                x_bgm_play(row ? 2 : 1);    /* (as the original) */
                x_text_clear();
                x_xsp_out0();
                return row;
            }
            if (row == 2) {             /* credits, then the menu again */
                ocall(OFN(0x0294a), 0, 0, 0, 0);
                draw_menu();                /* (the logo sprites stay) */
                draw_cursor(row, 1);
            }
        }
    }
}
