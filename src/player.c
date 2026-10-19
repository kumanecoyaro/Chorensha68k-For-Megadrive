/* Player ship and "cho-rensha" shots (port of fn_05e50 / fn_06470 / fn_05ca8) */
#include "game.h"
#include "vdp.h"

#define MAX_SHOT 30

s16 player_x, player_y;         /* B_4cc9e, B_4cca0 */
s16 player_entry;               /* B_5939a: entry / invulnerable timer */
s16 player_dead;                /* respawn timer (0 = alive) */
static s16 bank;                /* B_9048e: -8 .. 8 */
static s16 speed;               /* B_90490 */
u16 pad_now, pad_prev;

typedef struct { s16 x, y, frame, vx, vy, power; } Shot;
static Shot shots[MAX_SHOT];    /* B_4bd1e.. : y == 0 means free */

/* fn_05ca8 power level parameters (power_tab = D_489a6) */
static s16 shot_mask, shot_vy, shot_spd, shot_pow;
static s16 shot_timer = -1;     /* B_90496 */
static s16 shot_presses;        /* B_90498 */
static s16 shot_btn, shot_btn_prev;   /* B_9049a, B_9049c */
static s16 muzzle;              /* B_9049e */
static s16 autofire;

static void set_power(int lv)
{
    const s16 *p = &power_tab[lv * 6];
    shot_mask = p[0];           /* B_90480 */
    shot_vy = p[1];             /* B_90482 */
    shot_spd = p[2];            /* B_90484 */
    /* p[3], p[4]: shot PCG numbers (rot32 $48/$51) -> FR_SHOTC / FR_SHOTS here */
    shot_pow = p[5];            /* B_9048a */
}

/* fn_05da8 */
void player_init(void)
{
    int i;
    bank = 0;
    speed = 0x30;               /* B_4a0f6 default (config 'speed') */
    player_x = 0x2200;
    player_y = 0x5400;
    player_entry = 0x100;
    player_dead = 0;
    shot_timer = -1;
    shot_presses = 0;
    shot_btn = 3;
    for (i = 0; i < MAX_SHOT; i++)
        shots[i].y = 0;
    set_power(0);
}

/* fn_0456e: pad -> numeric keypad direction (0 = none) */
static s16 pad_dir(u16 p)
{
    static const u8 dirs[16] = {
        /* RLDU */
        0, 8, 2, 0, 4, 7, 1, 4, 6, 9, 3, 6, 0, 8, 2, 0
    };
    return dirs[p & 15];
}

void player_kill(void)
{
    Tmpl t;
    if (player_entry || player_dead)
        return;
    t.x = player_x; t.y = player_y; t.rate = 0x10; t.info = 0x200;
    spawn_explosion(&t);
    sound_request(0x28);
    player_dead = 0x60;
}

/* fn_05e50 */
void player_update(void)
{
    s16 d6 = 0, d1, d3 = speed;
    int moving = 1;

    if (player_dead) {
        if (--player_dead == 0)
            player_init();
        return;
    }
    if (player_entry) {
        player_entry--;
        if (player_entry > 0x80) {
            player_y += -0x40;
            moving = 0;
        }
    }
    if (moving) {
        s16 dir = pad_dir(pad_now);
        if (dir) {
            u8 ang = dir_angle_tab[dir];
            d6 = TRIG_X(1, ang);
            d1 = TRIG_Y(1, ang);
            d3 >>= 4;
            player_x += muls(d6, d3);
            player_y += muls(d1, d3);
            if (player_x <= 0x3FF) player_x = 0x400;
            if (player_x > 0x4000) player_x = 0x4000;
            if (player_y <= 0x3FF) player_y = 0x400;
            /* X68000: $3f00; the MD screen is 32 lines shorter */
            if (player_y > 0x3F00 - 0x800) player_y = 0x3F00 - 0x800;
        }
    }
    /* bank (L05ef2 / L06330) */
    if (d6 < 0) {
        if (--bank < -8) bank = -8;
    } else if (d6 > 0) {
        if (++bank > 8) bank = 8;
    } else if (bank > 0) {
        bank--;
    } else if (bank < 0) {
        bank++;
    }
    {
        s16 b = bank < 0 ? (bank + 7) >> 3 : bank >> 3;
        u16 fr = FR_PLAYER + (b + 1) * 2 + ((frame_count >> 1) & 1);
        /* the original flashes the palette while invulnerable; blink instead */
        if (!(player_entry && (frame_count & 1)))
            spr_frame(LAYER_TOP, fr, player_x, player_y);
    }
}

/* fn_06470 */
void shots_update(void)
{
    int i;
    s16 nfree = 0;
    static s16 freelist[MAX_SHOT];

    shot_btn_prev = shot_btn;
    shot_btn = (pad_now & (PAD_A | PAD_B)) ? 1 : 0;
    /* C = auto fire (MD addition): one press every 12 frames while held */
    if (pad_now & PAD_C) {
        if (++autofire >= 12) autofire = 0;
        if (autofire < 6) shot_btn = 1;
    } else {
        autofire = 0;
    }
    if ((shot_btn & 1) && !(shot_btn_prev & 1))
        shot_presses++;
    if (shot_presses && shot_timer < 0) {
        shot_timer = 0;
        shot_presses = 0;
        muzzle = 0;
    }

    for (i = 0; i < MAX_SHOT; i++)
        if (shots[i].y == 0)
            freelist[nfree++] = i;

    if (!player_dead && shot_timer >= 0) {
        s16 t = shot_timer;
        if ((t & shot_mask) == 0) {
            sound_request(0x20);
            if (t <= 7 && nfree > 0) {          /* centre shot */
                Shot *s = &shots[freelist[--nfree]];
                s->vx = 0;
                s->vy = shot_vy;
                s->x = player_x;
                s->y = player_y - 0x100;
                s->frame = FR_SHOTC;
                s->power = shot_pow * 2;
            }
            if (nfree > 1) {                    /* spreading pair */
                s16 ang = t & ~3;
                s16 fr = FR_SHOTS + shotdir_tab[ang * 2];
                s16 dx = 0x200 + ((t << 7) & 0x100);
                s16 vx = TRIG_X(shot_spd, ang), vy = TRIG_Y(shot_spd, ang);
                Shot *s = &shots[freelist[--nfree]];
                s->x = player_x + dx; s->y = player_y + 0x100;
                s->vx = vx; s->vy = vy; s->frame = fr; s->power = shot_pow;
                s = &shots[freelist[--nfree]];
                s->x = player_x - dx; s->y = player_y + 0x100;
                s->vx = -vx; s->vy = vy; s->frame = fr; s->power = shot_pow;
                spr_frame(LAYER_SHOT, FR_MUZZLE + muzzle, player_x + 0x200, player_y);
                spr_frame(LAYER_SHOT, FR_MUZZLE + muzzle, player_x - 0x200, player_y);
                muzzle ^= 1;
            }
        }
        if (++shot_timer > 11)
            shot_timer = -1;
    }

    for (i = 0; i < MAX_SHOT; i++) {
        Shot *s = &shots[i];
        if (s->y == 0)
            continue;
        s->x += s->vx;
        s->y += s->vy;
        if ((u16)s->x >= 0x4400 || (u16)s->y >= 0x4400) {
            s->y = 0;
            continue;
        }
        spr_frame(LAYER_SHOT, s->frame, s->x, s->y);
    }
}

/* fn_2f448 + fn_0416c: shots inside the object's hit box are consumed */
int shots_hit(Obj *o)
{
    int i, dmg = 0;
    s16 hw = o->hw << 6, hh = o->hh << 6;
    for (i = 0; i < MAX_SHOT; i++) {
        Shot *s = &shots[i];
        s16 dx, dy;
        if (s->y == 0)
            continue;
        dx = s->x - o->x;
        dy = s->y - o->y;
        if (dx >= -hw && dx <= hw && dy >= -hh && dy <= hh) {
            dmg += s->power;
            s->y = 0;
        }
    }
    if (dmg)
        sound_request(0x42);
    return dmg;
}
