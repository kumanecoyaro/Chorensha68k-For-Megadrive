/*
 * Enemies, enemy bullets and explosions used by the first part of stage 1.
 *
 *   type $190  small fighter "zako"      init fn_131d4  move L13298
 *   type $1f0  heavy gunship (obj200)    init fn_16fc0  move L170dc
 *   type $130  aimed small bullet        init fn_10c24  move L10caa
 *   type $134  spread bullet             init fn_11246  move L112cc
 *   type $132  large directional bullet  init fn_10ef4  move L10ff0
 *   type $102  explosion                 init fn_100b2  move L1006e
 */
#include "game.h"

static void draw(u8 layer, Obj *o)
{
    spr_frame(layer, o->pt, o->x, o->y);
}

/* enemy bullet vs player: |dx| <= rx, |dy| <= ry (1/64 px) */
static void bullet_hit_player(Obj *o, s16 rx, s16 ry)
{
    s16 dx = o->x - player_x, dy = o->y - player_y;
    if (player_dead || player_entry)
        return;
    if (dx >= -rx && dx <= rx && dy >= -ry && dy <= ry) {
        player_kill();
        obj_free(o);
    }
}

/* ------------------------------------------------------------ explosion $102 */
static void expl_move(Obj *o)
{
    s16 f = o->c4e >> 4;                    /* 42 source frames, 14 converted */
    o->pt = o->base + (f >= 39 ? 13 : divs(f, 3));
    draw(LAYER_FX, o);
    o->c4e += o->c4c;
    if (o->c4e > 0x29F)
        obj_free(o);
}

Obj *spawn_explosion(const Tmpl *t)
{
    Obj *o = obj_alloc();
    if (!o)
        return NULL;
    o->type = 0x102;
    o->update = expl_move;
    o->x = t->x;
    o->y = t->y;
    o->base = FR_EXPL;
    o->info = t->info ^ (rnd() & 0xC000);
    o->c4c = t->rate;
    o->c4e = 0;
    return o;
}

/* fn_0ea1a: big explosion for heavy enemies */
void spawn_big_explosion(s16 x, s16 y)
{
    static const s8 ofs[5][2] = { {0, 0}, {-20, -12}, {20, -10}, {-14, 14}, {16, 12} };
    Tmpl t;
    int i;
    for (i = 0; i < 5; i++) {
        t.x = x + (ofs[i][0] << 6);
        t.y = y + (ofs[i][1] << 6);
        t.rate = 0x10 + (i << 2);
        t.info = 0x900;
        spawn_explosion(&t);
    }
}

/* ------------------------------------------------------------ bullet $130 */
static void bul130_move(Obj *o)
{
    o->x += o->vx;
    o->y += o->vy;
    o->c4c--;
    o->pt = FR_BUL + ((o->c4c >> 2) & 7);
    if (!obj_onscreen(o)) {
        obj_free(o);
        return;
    }
    draw(LAYER_EBUL, o);
    bullet_hit_player(o, 0x80, 0x100);
}

Obj *spawn_bullet130(const Tmpl *t)
{
    Obj *o = obj_alloc();
    u8 a = (u8)t->angle;
    if (!o)
        return NULL;
    o->type = 0x130;
    o->update = bul130_move;
    o->x = t->x;
    o->y = t->y;
    o->info = t->info;
    o->vx = muls(bulvel_x[a], t->speed) >> 4;
    o->vy = muls(bulvel_y[a], t->speed) >> 4;
    o->c4c = 0;
    return o;
}

/* ------------------------------------------------------------ bullet $134 */
static void bul134_move(Obj *o)
{
    o->x += o->vx;
    o->y += o->vy;
    o->c4c--;
    o->pt = FR_BULS + ((o->c4c >> 1) & 1);
    if (!obj_onscreen(o)) {
        obj_free(o);
        return;
    }
    draw(LAYER_EBUL, o);
    bullet_hit_player(o, 0xC0, 0x140);
}

Obj *spawn_bullet134(const Tmpl *t)
{
    Obj *o = obj_alloc();
    u8 a = (u8)t->angle;
    if (!o)
        return NULL;
    o->type = 0x134;
    o->update = bul134_move;
    o->x = t->x;
    o->y = t->y;
    o->info = t->info;
    o->vx = muls(bulvel_x[a], t->speed) >> 4;
    o->vy = muls(bulvel_y[a], t->speed) >> 4;
    o->c4c = 0;
    return o;
}

/* fn_0ead4: fan of `count` bullets, angles 0, +s, -s, +2s, -2s ... */
Obj *spawn_spread(const Tmpl *t)
{
    Tmpl b = *t;
    s16 d3 = 0, d5 = t->spread;
    int k;
    for (k = 0; k < t->count; k++) {
        b.angle = t->angle + d3;
        spawn_bullet134(&b);
        if (k & 1) {
            d3 = -d3;
            d5 = -d5;
        } else {
            d3 += d5;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ bullet $132 */
static void bul132_move(Obj *o)
{
    o->x += o->vx;
    o->y += o->vy;
    if (o->x < -0x400 || o->y < -0x400 || o->x > 0x4800 || o->y > 0x4800) {
        obj_free(o);
        return;
    }
    draw(LAYER_EBUL, o);                    /* (palette flicker on the X68000) */
    bullet_hit_player(o, 0x100, 0x180);
}

Obj *spawn_bullet132(const Tmpl *t)
{
    Obj *o = obj_alloc();
    u8 a = (u8)t->angle;
    if (!o)
        return NULL;
    o->type = 0x132;
    o->update = bul132_move;
    o->x = t->x + TRIG_X(8, a);
    o->y = t->y + TRIG_Y(8, a);
    o->vx = muls(bulvel_x[a], t->speed) >> 4;
    o->vy = muls(bulvel_y[a], t->speed) >> 4;
    o->pt = FR_BIGBUL + shotdir_tab[a * 2];
    o->info = t->info;
    return o;
}

/* ------------------------------------------------------------ zako $190 */
static void zako_move(Obj *o)
{
    s16 x, y, d1 = 0;
    o->x += o->vx;
    x = o->x;
    y = o->y += o->vy;
    o->vy -= 2;
    if (y < -0x10 && o->vy < 0) {           /* left through the top */
        obj_free(o);
        return;
    }
    if ((++o->c4c & 7) == 0) {
        s16 target = 0, diff;
        d1 = atan2_x68(player_x - x, player_y - y);
        if (o->vy >= 0)
            target = d1;
        diff = o->dir + 8 - target;
        if (diff & 0xF0) {
            o->dir += ((s8)diff < 0) ? 0x10 : -0x10;
        } else if ((o->c4c & 15) == 0 && o->vy > 0 && o->c4c < o->c4e) {
            Tmpl t;
            t.x = x; t.y = y;
            t.angle = d1 - 4 + (rnd() & 7);
            t.speed = 0x30;
            t.info = 0xC3C;
            spawn_bullet130(&t);
        }
    }
    o->pt = o->base + (((o->dir + 8) >> 4) & 15);
    draw(LAYER_ENEMY, o);
    if (shots_hit(o)) {
        Tmpl t;
        t.x = o->x; t.y = o->y;
        t.rate = 0x10;
        t.info = (o->info & 0xF0FF) | 0x200;
        spawn_explosion(&t);
        add_score(SCORE_190);               /* fn_06d9a */
        obj_free(o);
    }
}

Obj *spawn_zako190(const Tmpl *t)
{
    Obj *o = obj_alloc();
    s16 v;
    if (!o)
        return NULL;
    o->hw = 8;
    o->hh = 8;
    o->type = 0x190;
    o->update = zako_move;
    o->x = t->x;
    o->y = t->y;
    v = TRIG_X(1, atan2_x68(player_x - o->x, player_y - o->y));
    o->vx = (v + ((u16)v >> 15)) >> 1;
    o->vy = 0xC0 + (rnd() & 0x3F);
    o->dir = 0x80;
    o->c4c = 0;
    o->c4e = (rank >> 2) + 0x20;
    o->info = (t->info & 0xF0FF) | 0x700;
    o->base = FR_ZAKO;
    return o;
}

/* ------------------------------------------------------------ gunship $1f0 */
static s16 quarter(s16 v)
{
    return (v < 0 ? v + 3 : v) >> 2;
}

static void ship_move(Obj *o)
{
    s16 t = ++o->t78, dmg, h = (t & 0xFF) - 0x60;
    if (h < 0 || h > 0x80)
        o->y += 0x40;                       /* descend except while hovering */
    o->c4c += 13;
    o->c4e += 21;
    o->x += quarter(TRIG_X(1, (o->c4c >> 4) & 0xFF));
    o->y += quarter(TRIG_Y(1, (o->c4e >> 4) & 0xFF));
    if (o->y > 0x4C00) {
        obj_free(o);
        return;
    }
    if ((t & 0xE7) == 0x80) {               /* two 3-way fans */
        Tmpl b;
        s16 off = (0x20 - (t & 0x10)) * 2, a;
        if ((t & 0x1F) == 0)
            o->dir = atan2_x68(player_x - o->x, player_y - o->y);
        b.info = 0xC3C; b.speed = 0x38; b.type = 0x134; b.count = 3; b.spread = 8;
        a = (o->dir + off) & 0xFF;
        b.angle = a; b.x = o->x + TRIG_X(8, a); b.y = o->y + TRIG_Y(8, a);
        spawn_spread(&b);
        a = (o->dir - off) & 0xFF;
        b.angle = a; b.x = o->x + TRIG_X(8, a); b.y = o->y + TRIG_Y(8, a);
        spawn_spread(&b);
    }
    if ((t & 0xE7) == 0xA0) {               /* large bullet */
        Tmpl b;
        b.x = o->x; b.y = o->y; b.info = 0x13C; b.angle = o->dir; b.speed = 0x40;
        spawn_bullet132(&b);
    }
    /* hit flash (palette $f on the X68000): skip drawing for a frame */
    if (!o->flash)
        draw(LAYER_ENEMY, o);
    o->flash = 0;
    dmg = shots_hit(o);
    if (dmg) {
        o->flash = 1;
        o->hp -= dmg;
        if (o->hp < 0) {
            spawn_big_explosion(o->x, o->y);
            add_score(SCORE_1F0);
            obj_free(o);
        }
    }
}

Obj *spawn_ship1f0(const Tmpl *t)
{
    Obj *o = obj_alloc();
    if (!o)
        return NULL;
    o->hw = 0x20;
    o->hh = 0x18;
    o->type = 0x1F0;
    o->update = ship_move;
    o->x = t->x;
    o->y = (s16)0xF800;
    o->info = o->info18 = (t->info & 0xF0FF) | 0x900;
    o->pt = FR_SHIP;
    o->hp = o->hpmax = HP_1F0;
    o->t78 = 0;
    o->c4c = rnd();
    o->c4e = rnd();
    return o;
}
