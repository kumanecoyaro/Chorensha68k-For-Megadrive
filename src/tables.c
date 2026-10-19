#include "game.h"

static u16 rnd_idx;                 /* $345e0 */
s16 bulvel_x[256], bulvel_y[256];

/* fn_345c8: table driven random numbers (replay deterministic) */
u16 rnd(void)
{
    rnd_idx++;
    return rand_tab[rnd_idx & 0x3FF];
}

/* fn_2fb06: angle from (dx,dy); 0 = up, 0x40 = right, 0x80 = down */
u8 atan2_x68(s16 dx, s16 dy)
{
    s16 ndx = -dx;
    if (dx == 0 && dy == 0)
        return 0;
    if (dx > dy) {
        if (ndx > dy)
            return atan_tab[128 + divs((s32)ndx << 7, dy)];
        return (u8)(atan_tab[128 + divs((s32)dy << 7, dx)] + 0x40);
    }
    if (ndx > dy)
        return (u8)(atan_tab[128 + divs((s32)dy << 7, dx)] + 0xC0);
    return (u8)(atan_tab[128 + divs((s32)ndx << 7, dy)] + 0x80);
}

/* fn_05170: enemy bullet unit vectors = speed-16 trig entry * rate / 256 */
static s16 scale256(s16 v, u16 rate)
{
    s32 t = muls(v, rate);
    if (t < 0)
        t += 0xFF;
    return (s16)(t >> 8);
}

void tables_init(u16 rate)
{
    int a;
    for (a = 0; a < 256; a++) {
        bulvel_x[a] = scale256(TRIG_X(16, a), rate);
        bulvel_y[a] = scale256(TRIG_Y(16, a), rate);
    }
}
