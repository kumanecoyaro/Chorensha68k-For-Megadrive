#ifndef GAME_H
#define GAME_H
/*
 * Chorensha68k -> Mega Drive port: shared game state.
 *
 * Coordinates follow the X68000 original: 16-bit values in 1/64 pixel,
 * XSP sprite space (screen pixel = (v >> 6) - 16 for the top-left of a
 * 16x16 cell, i.e. object centre = (v >> 6) - 8).  The X68000 screen is
 * 256x256; the Mega Drive shows the top 224 lines of it.
 */
#include "types.h"
#include "assets.h"

/* ---- sprite list (sprite.c) ---- */
enum { LAYER_TOP, LAYER_EBUL, LAYER_ENEMY, LAYER_SHOT, LAYER_FX, LAYER_COUNT };
void spr_begin(void);
void spr_frame(u8 layer, u16 frame, s16 x, s16 y);   /* x,y in 1/64 px */
void spr_text(u8 layer, s16 px, s16 py, const char *s);
void spr_end(void);
void spr_upload(void);

/* ---- tables lifted from SZ2.X (tables.c) ---- */
u16 rnd(void);                                       /* fn_345c8 */
u8 atan2_x68(s16 dx, s16 dy);                        /* fn_2fb06 */
#define TRIG_X(speed, ang) trig_tab[(((speed) << 8) + ((ang) & 0xFF)) << 1]
#define TRIG_Y(speed, ang) trig_tab[((((speed) << 8) + ((ang) & 0xFF)) << 1) + 1]
extern s16 bulvel_x[256], bulvel_y[256];             /* B_4b91e */
void tables_init(u16 bullet_speed);                  /* fn_05170 */

/* ---- background (bg.c) ---- */
void bg_init(s16 loop_start, s16 loop_len);          /* fn_05998 + fn_05906 + fn_05574 */
void bg_set_loop(s16 a, s16 b);                      /* fn_058de */
void bg_update(void);                                /* fn_056ae */
void bg_upload(void);
extern s32 bg_target_speed[2];

/* ---- objects (obj.c) ---- */
typedef struct Obj Obj;
typedef void (*ObjFn)(Obj *);
struct Obj {
    s16 x, y;           /* +$00 +$02 */
    s16 pt;             /* +$04 frame */
    u16 info;           /* +$06 attribute (only flip / flash use) */
    s16 hw, hh;         /* +$08 +$0a hit box half size (px) */
    s16 base;           /* +$16 base pattern */
    u16 info18;         /* +$18 */
    s16 type;           /* +$22 */
    ObjFn update;       /* +$24 */
    s16 vx, vy;         /* +$3c +$3e */
    s16 c4c, c4e;       /* +$4c +$4e */
    s16 dir;            /* +$54 */
    s16 hp, hpmax;      /* +$5c +$5e */
    s16 speed;          /* +$64 */
    s16 t78, n7a, s7c;  /* +$78 +$7a +$7c */
    u8 active;
    u8 flash;
    Obj *next;
};
/* spawn template (the -$80(a6) block the original passes to init functions) */
typedef struct {
    s16 x, y;
    u16 info;
    s16 angle;          /* -$2c (+$54) */
    s16 speed;          /* -$1c (+$64) */
    s16 rate;           /* -$34 (+$4c) */
    s16 type, count, spread;  /* +$78 +$7a +$7c */
} Tmpl;

void obj_init(void);
Obj *obj_alloc(void);                                /* fn_040b6 */
void obj_free(Obj *o);                               /* fn_0410e */
void obj_update_all(void);                           /* fn_04094 */
int obj_onscreen(const Obj *o);                      /* fn_2ca2c clip test */

/* enemies / bullets / effects (enemy.c) */
Obj *spawn_zako190(const Tmpl *t);                   /* fn_131d4 */
Obj *spawn_ship1f0(const Tmpl *t);                   /* fn_16fc0 */
Obj *spawn_bullet130(const Tmpl *t);                 /* fn_10c24 */
Obj *spawn_bullet134(const Tmpl *t);                 /* fn_11246 */
Obj *spawn_bullet132(const Tmpl *t);                 /* fn_10ef4 */
Obj *spawn_spread(const Tmpl *t);                    /* fn_0ead4 */
Obj *spawn_explosion(const Tmpl *t);                 /* fn_100b2 */
void spawn_big_explosion(s16 x, s16 y);              /* fn_0ea1a */

/* ---- player (player.c) ---- */
extern s16 player_x, player_y;                       /* B_4cc9e B_4cca0 */
extern s16 player_entry;                             /* B_5939a */
extern s16 player_dead;                              /* B_90494 */
extern u16 pad_now, pad_prev;
void player_init(void);                              /* fn_05da8 */
void player_update(void);                            /* fn_05e50 */
void shots_update(void);                             /* fn_06470 */
void player_kill(void);                              /* fn_06c0e */
int shots_hit(Obj *o);                               /* fn_2f448 + fn_0416c: damage */

/* ---- sound (sound.c) ---- */
void sound_init(void);
void sound_request(u8 no);                           /* fn_034ae: PCM number */
void sound_update(void);
void music_play(u8 no);                              /* fn_03590: BGM number */
void music_stop(void);
void music_update(void);

/* ---- stage (stage.c) ---- */
extern u16 frame_count;                              /* B_4a0ca */
extern u16 rank;                                     /* B_4a0c8 */
extern u32 score;
void stage_init(void);
void stage_update(void);                             /* fn_04338 / fn_06de4 */
void add_score(u16 pts);

#endif
