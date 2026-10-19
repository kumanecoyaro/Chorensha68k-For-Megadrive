/*
 * Stage 1 script (port of fn_06de4, state variable B_59328, timer B_59326).
 *
 *   $100  BG start, "STAGE 1" caption                    ($e0 frames)
 *   $104  quiet                                          ($180 frames)
 *   $108  zako $190 in groups of four from the left / right, gunship $1f0 at $200
 *   $110  second gunship; map loop moved on (fn_058de $10,2)
 *         (the type $208 enemies of this state are not ported yet)
 *   then back to $108 so the test keeps running.
 */
#include "game.h"

u16 frame_count;        /* B_4a0ca */
u16 rank;               /* B_4a0c8 */
u32 score;

static u16 state, timer;

void stage_init(void)
{
    state = 0x100;
    timer = 0;
    rank = 0;
    score = 0;
}

void add_score(u16 pts)
{
    score += pts;
}

static void zako_at(s16 xbase)
{
    Tmpl t;
    t.x = ((rnd() & 0x3F) << 6) + xbase;
    t.y = (s16)0xFC00;
    t.info = 0x34;
    spawn_zako190(&t);
}

static void ship_at(s16 x)
{
    Tmpl t;
    t.x = x;
    t.y = 0;
    t.info = 0x934;
    spawn_ship1f0(&t);
}

void stage_update(void)
{
    /* fn_04338: rank rises every 128 frames */
    if ((frame_count & 0x7F) == 0 && rank < 0xFF)
        rank++;

    switch (state) {
    case 0x100:
#ifndef NO_MUSIC
        if (timer == 0)
            music_play(0x11);           /* fn_03590($11): stage 1 BGM */
#endif
        if (timer >= 0x30 && timer < 0xC0)
            spr_text(LAYER_TOP, 100, 108, "STAGE 1");
        if (++timer > 0xDF) {
            state = 0x104;
            timer = 0;
        }
        break;
    case 0x104:
        if (++timer > 0x17F) {
            state = 0x108;
            timer = 0;
        }
        break;
    case 0x108:
        if ((timer & 0xCF) == 0)
            zako_at(0x600);
        if ((timer & 0xCF) == 0x80)
            zako_at(0x2E00);
        if (timer == 0x200)
            ship_at(0x2200);
        if (++timer > 0x37F) {
            state = 0x110;
            timer = 0;
        }
        break;
    case 0x110:
        if (timer == 0x140)
            ship_at(0x2200);
        if (timer == 0x200)
            bg_set_loop(0x10, 2);
        if (++timer > 0x3BF) {
            state = 0x108;
            timer = 0;
        }
        break;
    }
}
