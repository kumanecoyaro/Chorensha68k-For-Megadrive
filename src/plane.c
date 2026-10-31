/*
 * Plane A: large enemies and big effects (the BG uses plane B only, src/bg.c).
 *
 * Large enemies: the stage 1 boss (obj204) and the crab (obj202, two halves
 * and a centre drawn as three refs), obj21f, the stage 2 mid boss (obj200:
 * body and four arms) and boss (obj205) are pre-composited by
 * tools/convert_sprites.py into tile images (the crab's halves open 0..4 px:
 * images at 0 and 4 px; obj200 with its arms closed, or the body alone).  At the end of each frame the recorded XSP calls are
 * searched for one of them; when its tiles are in VRAM its calls are dropped
 * from the sprite list and plane A shows the image at the same place instead
 * (one enemy at a time: a boss first, else the one nearest to the one
 * already shown; plane A scrolls to put the image there).
 *
 * Big effects (the player's bomb, the boss's death: expl2 / expl3 / flare
 * refs) are drawn into the same plane from the sprite blocks and the sprite
 * VRAM cache (xsp.c): each block's tiles go into the cells under it, the
 * position rounded to the plane's 8 px grid; in front of the enemy image.
 *
 *   * hit flash (another palette): the image stays, the flashing refs are
 *     drawn as sprites over it;
 *   * plane A is as wide as the screen, so a column cut by the left / right
 *     edge would wrap around: such cells are left empty;
 *   * plane A is behind every sprite;
 *   * the tiles of an image group (crab / boss: never on screen together)
 *     are loaded into VRAM over a few VBlanks the first time it appears,
 *     sprites until then.
 *
 * The visible rows of plane A are rebuilt every frame in win[] and the rows
 * that hold (or held) something are written in VBlank.
 */
#include "game.h"
#include "vdp.h"
#include "spr.h"

#define ENEMY_BASE 1234                 /* VRAM tiles 1234..1503 (xsp.c cache ends at 1234, the */
#define ENEMY_TILES (VRAM_SPRITES / 32 - ENEMY_BASE)    /* sprite table at 1504) */
/* a group of more tiles (the stage 6 boss) borrows the sprite cache's last
 * 16-tile slots below them while it is loaded (src/xsp.c xsp_lend) */
#define LEND_SLOTS ((PLANE_MAX_TILES - ENEMY_TILES + 15) / 16)
_Static_assert(PLANE_MAX_TILES <= ENEMY_TILES + 16 * 16, "plane A enemy tiles: too many to borrow");
void xsp_lend(u8 n);
static u16 tbase = ENEMY_BASE;          /* VRAM tile of the loaded group's first tile */
#define EMPTY_REF  REF_EMPTY            /* a dropped call (spr.h: a ref without pieces) */
#define REF_CRAB   0xB1E                /* obj202: left half, right half, centre */
#define REF_BOSS   0xB29                /* obj204 */
#define REF_O21F   0xB79                /* obj21f, $B7A after it fires (images 3, 4) */
#define REF_O200   0xB18                /* obj200: body, arm $B19 x2, arm $B1A x2 (right ones mirrored) */
#define REF_O205   0xB2B                /* stage 2 boss obj205: $B2B..$B30 with pods, $B31 core */
#define IMG_O200   5                    /* (unused: obj200's arms closed / body alone; now IMG_O200P) */
#define IMG_O205   9                    /* core, then $B2B..$B30 */
#define REF_O207   0xB34                /* stage 3 boss obj207: $B34..$B3B, second form $B3C / $B3D */
#define IMG_O207   16                   /* $B34..$B3D */
#define IMG_O1F0   26                   /* stage 3 type $1F0: obj200 body $B18, palette 9 */
#define REF_O1A1   0xB16                /* stage 3 type $1A1 (obj1a0): centre $B16 after its pods $B17 */
#define IMG_O1A1   27                   /* the pods fully out (palette 6) */
#define REF_SHIP   0xB43                /* stage 4 battleship type $20A (obj20A): centre $B43, wings */
#define IMG_SHIP   28                   /* $B44 (the right one mirrored; $B45 damaged): + mask - 1 */
#define REF_O20E   0xB46                /* stage 4 large ship type $20E (obj20E): frames $B46..$B4D */
#define IMG_O20E   35                   /* its frames (palette 9) */
#define REF_O21A   0xB75                /* stage 5 battleship obj21a (palette 6): $B75, damaged $B76 */
#define IMG_O21A   43
#define REF_O213   0xB5B                /* stage 5 mid boss obj213 (palette 3) */
#define IMG_O213   45
#define REF_O216   0xB5E                /* stage 5 boss obj216: body; parts $B5F..$B67 over it, arms $B68.. */
#define IMG_O216   46                   /* the body alone, + k: with part $B5E + k (k 1..9) */
#define REF_O210   0xB51                /* stage 6 boss obj210 (palette 3): $B51..$B54 drawn twice (mirrored), */
#define IMG_O200P  65                   /* obj200 whole: + 4 * (palette 7) + left pose * 2 + right pose, */
                                        /* + 8 / 9 the body alone (palette 8 / 7) */
#define REF_F15    0xB15                /* type $1A1 folded (obj1a0 $B15), opened: REF_O1A1 */
#define IMG_FM     76                   /* formation mode: + 0 folded, + 1 the pods out, */
                                        /* + 2 .. 6 opening (s = 2, 4 .. 10) */
#define REF_O212   0xB59                /* stage 6 large enemy obj212: centre $B59, wings $B5A */
#define IMG_O212   83                   /* (palette 7) closed, + 1 open: formation mode */
#define FM_PAL(m)  ((u8)((m) - IMG_O212) < 2 ? 7 : 6)   /* (formation mode units: not flashing) */
#define FM_MAX     16                   /* units looked at (src/plane.c fm_select) */
#define FM_PL      10                   /* units placed */
#define FM_BANDS   4                    /* rows of them (each its own horizontal scroll) */
#define FM_EV      4                    /* band switches a frame (src/xsp_fast.s _hint) */
#define REF_O20F   0xB4E                /* stage 6 obj20F: pods $B4E, $B4F, centre $B50 */
#define IMG_O20F   75                   /* the whole of it (palette 8) */
#define IMG_O21F9  63                   /* obj21f in palette 9 (stage 6; images 3 / 4: palette 7) */
#define IMG_O210   56                   /* parts $B57 / $B58: $B51, $B52, $B52 + $B58, $B53, $B54, $B53 + $B57, $B55 */
#define IMG_O210F  85                   /* its last form $B56 */
#define BOSS_IMG(m) ((m) == 2 || ((m) >= IMG_O205 && (m) < IMG_O1F0) || (u8)((m) - IMG_O216) < 10 \
                     || (u8)((m) - IMG_O210) < 7 || (m) == IMG_O210F)
#define LOAD_CHUNK 64                   /* tiles per VBlank (a group borrowing tiles: twice that) */
#define WROWS      29                   /* plane rows on screen (224 lines + fine scroll) */
#define WIN_ROWS   32                   /* win[]: the window, or a still image up to 32 rows (obj21a) */

typedef struct { s16 x, y; u16 id, info; } Call;
extern Call calls[];
extern u16 ncalls;
extern u8 pl_ix[];                      /* calls of the refs looked for here (xsp_fast.s) */
extern u16 pl_n;
extern volatile u8 bg_on;               /* (bg.c) */
extern const s16 size_w[16], size_h[16];
u16 xsp_tile_for(u16 block, u8 pal, u8 mesh, u16 *key);
void xsp_touch(u16 key);
void xsp_pairs(int on);                 /* src/xsp.c */

static u8 loaded = 0xFF, loading = 0xFF;
static u16 load_pos;
static s8 shown = -1;                   /* image shown, -1 = none */
static u8 mirror;                       /* it is shown mirrored (no image is now: always 0) */
static s16 ax, ay;                      /* anchor of the shown image (screen pixels) */
static s8 st_img = -2;                  /* image written by write_static (-2: none, rewrite) */
static u32 st_cols;
static u8 was_fx;                       /* effects last frame */
static s16 hs, vs;                      /* plane A scroll: col c at x = c*8 + hs, row r at y = r*8 - vs */
/* two battleships (stage 4's last part: always one above the other): the
 * wing of the second one in plane rows 32.., shown from line split on with
 * scroll hs2 / vs2 (the H-int, src/xsp_fast.s _hint) */
#define SHIP_MAX   4
static u16 split;                       /* 0: one image */
static s16 hs2, vs2;
static s16 ax2, ay2 = -999;             /* centre of the second ship (kept while it lasts) */
static s8 st_img2 = -1;
static u32 st_cols2;
static s16 act_hs, act_vs, act_hs2, act_vs2;    /* of the frame shown (plane_vblank) */
static u16 act_split;
/* two stage 5 battleships side by side (they come in pairs): both in plane
 * rows 0.., in their own columns, plane A scrolled by 2-cell columns
 * (VDP register 11): columns below colk at vs, from colk on at vsr */
static u8 colmode, act_colmode;
static s16 vsr, act_vsr;
static u8 colk, act_colk;
static s8 col_base;                     /* build_rows: image column c at plane column c + col_base */
static s8 st_base, st_base2;
extern s16 bg_vs;                       /* (bg.c: plane B's vertical scroll) */
extern u16 split_line, split_vs, split_hs, act_n;

static u16 win[WIN_ROWS][32];              /* visible rows, window row k = plane row top + k */
static u32 win_nz;                      /* window rows holding something */
static u32 vram_nz[2];                  /* plane rows (0..63) holding something in VRAM */
static u8 wr_row[64], wr_win[64];       /* rows to write: plane row, window row (0xFF: empty row) */
static volatile u8 wr_n;
u16 plane_frames, plane_fx;
#ifdef PLANE_STATS
u16 stat_pp, stat_pr, stat_cells;
#endif             /* (stats for tools/uctrace.py) */

static u8 pal_of(const Call *c) { return (c->info >> 8) & 15; }

/* the player's bomb (expl2 / expl3 / flare refs at the very front, $3F),
 * the large expl2 frames ($B8 / $B9: the boss's death, once the boss has
 * been seen; the crab's death puffs stay sprites) and flares; the small
 * expl2 frames enemies draw at their own priority stay sprites */
static u8 boss_seen;
static u8 is_effect(const Call *c)
{
    u16 id = c->id;
#ifdef NO_PLANE_FX
    return 0;
#endif
    if (id >= 0xAA9 && id <= 0xAAE)
        return 1;
    if (id == 0xB8 || id == 0xB9)
        return boss_seen;
    return id >= 0xB6 && id <= 0xBF && (c->info & 63) == 0x3F;
}

/* the stage 5 boss's body: high priority tiles, and its arms and the
 * hexagons it sends out ($33, behind the body as on the X68000) low
 * priority sprites (src/xsp_fast.s xsp_build: plane_behind) */
u8 plane_behind;
/* the stage 5 battleship is on plane A: its turrets get sprites kept for them
 * (src/xsp.c xsp_flush, src/xsp_fast.s enemy_room) */
u8 plane_turrets;
/* formation mode: type $1A1 units of a stage 3 formation (obj1a0, 64 px
 * apart on their path) lined up (src/plane.c fm_select) on plane A, in bands
 * (a column; each row) one above the other, each in its own window rows with
 * its own full-screen scroll, switched by the H-int (fm_ev: the line, the
 * horizontal scroll, the vertical scroll) in the gap (or where they
 * meet): the bands 2 window rows apart (blank), the line in max(b, t - 16)
 * .. min(t, b + 16) (b the bottom of the one above, t the top of the next) */
typedef struct { u8 line, cols; u16 v; } FmEv;
FmEv fm_ev[FM_EV], act_fm_ev[FM_EV + 1];   /* (act_fm_ev: then a line 0xFF, src/xsp_fast.s _hint) */
u8 fm_nev, act_fm_nev, act_fm_on, fm_evi;
extern u8 hint_nx;
extern u16 hint_r1, hint_exp;
#ifdef TEST_ROWS
u32 rows_up, rows_fr, rows_shown;
#endif
static u8 fm_on;
static u8 fm_n, fm_ix[FM_MAX];          /* the frame's units: call indices (B15 / B16 calls) */
static u8 fm_band[FM_MAX];              /* (fm_select: the row a unit is in) */
static u8 fm_seen, fm_col;              /* (fm_col: a column of them, fm_write) */
/* fm_write's last layout, kept while each band's units are as they were but
 * for the band moving: a signature of each band (its units' places from its
 * first, images, columns on screen), the units giving its scroll (x: its 64 px
 * block, y), its first row in the window, its event (0xFF: the top one, 0xFE:
 * none), the units placed */
static u32 fm_bsig[FM_BANDS];
static u8 fm_bax[FM_BANDS], fm_bay[FM_BANDS], fm_bR0[FM_BANDS], fm_bev[FM_BANDS];
static s8 fm_bblk[FM_BANDS];
static s16 fm_by0[FM_BANDS];
static u16 fm_placed;
static u8 fm_sn = 0xFF, fm_scol;
/* the units as they were (as many in the list, each a few px from where it
 * was, the same image, its columns on screen the same): fm_select / fm_write
 * skipped, the scrolls from the units giving them only (fm_skip; the units by
 * their places in the list: fm_raw of each one placed, fm_brx / fm_bry the
 * bands' scroll units, fm_praw those placed) */
static u8 fm_raw[FM_MAX], fm_rawn, fm_skip;
static u8 fm_brx[FM_BANDS], fm_bry[FM_BANDS];
static u16 fm_praw;
static u8 fm_rband[FM_MAX], fm_bRe[FM_BANDS], fm_bf[FM_BANDS];  /* (a unit's band; a band's */
                                            /* last row, horizontal scroll) */
static s16 fm_lx[FM_MAX], fm_ly[FM_MAX];    /* (each unit in the list last frame: its place, */
static s8 fm_limg[FM_MAX];                  /* its image) */
static s8 fm_img[FM_MAX];
typedef struct { u32 k; u16 c; } FmRect;  /* a unit written: img, r0, nr, row, base; columns lo << 8 | hi */
static FmRect fm_old[FM_PL];           /* (the units in the window: only those that change rewritten) */
static u8 fm_nold;
static u16 img_prio(s8 img)
{
    return (u8)(img - IMG_O216) < 10 ? 0x8000 : 0;
}

/* one cell at absolute plane cell (c, r) relative to window top */
static void put(s16 c, s16 r, s16 top, u16 w)
{
    s16 x = c * 8 + hs, k = r - top;
    if (x < 0 || x > 256 - 8 || k < 0 || k >= WROWS)
        return;
    win[k][c & 31] = w;
    win_nz |= 1UL << k;
}

static void put_image(s8 img, s16 top)
{
    const PlaneImg *im = &plane_img[img];
    const u16 *m = plane_map + im->map;
    u16 attr = (((u16)im->pal << 13) | img_prio(img)) + tbase - 1;
    u8 r, c;
    for (r = 0; r < im->h; r++)
        for (c = 0; c < im->w; c++, m++)
            if (*m) {
                if (mirror)
                    put(im->w - 1 - c, r, top, (attr + *m) ^ 0x800);
                else
                    put(c, r, top, attr + *m);
            }
}

/* a ref into the plane cells (like the composite path of xsp_build): all
 * of its pieces or none (0: left a sprite this frame; a part of an effect,
 * cut along the 8 px cells, shows as hard-edged blocks).  The player's bomb
 * ($3F) is drawn meshed: translucent as on the X68000. */
#define REF_PIECES 48
#define HELD_MAX   128
static u16 held_key[HELD_MAX];          /* cache keys of the effect pieces in the plane (fx_hold) */
static u16 held_n;
typedef struct { u16 t; s8 c0, k0; u8 tw, th, fl; } PPiece;
static PPiece pp[REF_PIECES];           /* the pieces of the ref being drawn, on screen */
static u8 put_ref(const Call *cl, s16 top)
{
    u16 i0 = ref_start[cl->id], n = ref_start[cl->id + 1], i;
    u8 objfl = (cl->info >> 14) & 3, opal = pal_of(cl), mesh = (cl->info & 63) == 0x3F, np = 0;
    s16 px = cl->x >> 6, py = cl->y >> 6;
    s16 c_lo = (7 - hs) >> 3, c_hi = (256 - 8 - hs) >> 3;  /* columns fully on screen */
    PPiece *q;
    if (n - i0 > REF_PIECES)
        return 0;
    {                                   /* the whole ref off screen: nothing to draw */
        const s16 *b = ref_box + cl->id * 4;
        s16 x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3], t;
        if (objfl & 1) { t = x0; x0 = -x1 - 16; x1 = -t - 16; }
        if (objfl & 2) { t = y0; y0 = -y1 - 16; y1 = -t - 16; }
        if (((px + x1 - hs + 4) >> 3) <= c_lo || ((px + x0 - hs + 4) >> 3) > c_hi
            || ((py + y1 + vs + 4) >> 3) - top <= 0 || ((py + y0 + vs + 4) >> 3) - top >= WROWS)
            return 1;
    }
    /* pass 1: the pieces on screen and their tiles (all or none) */
    for (i = i0; i < n; i++) {
        const SprPiece *p = &ref_pieces[i];
        u8 sz = p->size & 15, fl = ((p->size >> 4) & 3) ^ objfl, pal = (p->palx ^ opal) & 15;
        s16 w = size_w[sz], h = size_h[sz], dx = p->dx, dy = p->dy, c0, k0;
        u16 t, key;
        if (objfl & 1) dx = -dx - 16 - w;
        if (objfl & 2) dy = -dy - 16 - h;
        c0 = (px + dx - hs + 4) >> 3;
        k0 = ((py + dy + vs + 4) >> 3) - top;   /* window row of the piece's top */
        if (k0 + (h >> 3) <= 0 || k0 >= WROWS || c0 + (w >> 3) <= c_lo || c0 > c_hi)
            continue;
        if (!(t = xsp_tile_for(p->block, pal, mesh, &key)))
            return 0;
        if (held_n < HELD_MAX)
            held_key[held_n] = key;
        held_n++;
        q = &pp[np++];
        q->t = t | ((u16)pal_slot[pal] << 13) | ((fl & 1) << 11) | ((fl & 2) << 11);
        q->c0 = c0; q->k0 = k0; q->tw = w >> 3; q->th = h >> 3; q->fl = fl;
    }
#ifdef PLANE_STATS
    stat_pp += np; stat_pr++;
#endif
    /* pass 2: their cells */
    for (q = pp; np; np--, q++) {
        s16 c0 = q->c0, k0 = q->k0, th = q->th, ka = k0 < 0 ? 0 : k0, kb = k0 + th > WROWS ? WROWS : k0 + th;
        s16 ca = c0 < c_lo ? c_lo : c0, cb = c0 + q->tw - 1 > c_hi ? c_hi : c0 + q->tw - 1, c, k;
        for (c = ca; c <= cb; c++) {
            s16 tx = (q->fl & 1) ? c0 + q->tw - 1 - c : c - c0;
            u16 *d = &win[ka][c & 31], t = q->t + tx * th;
            if (q->fl & 2)
                for (t += k0 + th - 1 - ka, k = ka; k < kb; k++, d += 32)
                    *d = t--;
            else
                for (t += ka - k0, k = ka; k < kb; k++, d += 32)
                    *d = t++;
#ifdef PLANE_STATS
            stat_cells += kb - ka;
#endif
        }
        win_nz |= ((1UL << (kb - ka)) - 1) << ka;
    }
    return 1;
}

/* the calls shown by the plane image are dropped (a flashing part stays a
 * sprite, over it) */
static s8 o200_pose(const Call *c, u16 i, u16 *a);
static u8 o20f_parts(const Call *c, u16 i, u16 *a);

static void ship_drop_shown(void);
static u8 o210_ix[8], o210_n;           /* the stage 6 boss's calls in its image */
static void drop_image_calls(s8 img, Call *c)
{
#ifdef PLANE_KEEP
    return;                             /* (debug: the sprites drawn over the plane images) */
#endif

    if ((u8)(img - IMG_O200P) < 10) {
        u8 m = img - IMG_O200P, p = m < 4 || m == 8 ? 8 : 7, k;
        u16 a[4];
        if (pal_of(c) == p)
            c->id = EMPTY_REF;
        if (m < 8 && o200_pose(c, c - calls, a) >= 0)
            for (k = 0; k < 4; k++)
                if (pal_of(&calls[a[k]]) == p)
                    calls[a[k]].id = EMPTY_REF;
    } else if (img == IMG_O1F0 || (u8)(img - IMG_O21F9) < 2) {
        if (pal_of(c) == 9)
            c->id = EMPTY_REF;
    } else if ((u8)(img - IMG_O20E) < 8) {
        if (pal_of(c) == 9)
            c->id = EMPTY_REF;
    } else if ((u8)(img - IMG_SHIP) < 7) {
        ship_drop_shown();
    } else if ((u8)(img - IMG_O21A) < 2) {
        if (pal_of(c) == 6)
            c->id = EMPTY_REF;
    } else if (img == IMG_O20F) {
        u16 a[4];
        u8 k;
        if (o20f_parts(c, c - calls, a)) {
            if (pal_of(c) == 8)
                c->id = EMPTY_REF;
            for (k = 0; k < 4; k++)
                if (pal_of(&calls[a[k]]) == 8)
                    calls[a[k]].id = EMPTY_REF;
        }
    } else if (img == IMG_O213) {
        if (pal_of(c) == 3)
            c->id = EMPTY_REF;
    } else if ((u8)(img - IMG_O216) < 10) {
        if (pal_of(c) == 8)
            c->id = EMPTY_REF;
        if (img != IMG_O216 && pal_of(c + 1) == 8 && c[1].x == c->x && c[1].y == c->y)
            c[1].id = EMPTY_REF;
    } else if (img == IMG_O210F) {
        c->id = EMPTY_REF;              /* (its hit flash too, as its other forms) */
    } else if ((u8)(img - IMG_O210) < 7) {
        u8 k;
        for (k = 0; k < o210_n; k++)       /* (its hit flash too: too large for sprites) */
            calls[o210_ix[k]].id = EMPTY_REF;
    } else if (img == IMG_O1A1) {
        if (pal_of(c) == 6) c->id = EMPTY_REF;
        if (pal_of(c - 1) == 6) c[-1].id = EMPTY_REF;
        if (pal_of(c - 2) == 6) c[-2].id = EMPTY_REF;
    } else if (img >= 2) {
        if (pal_of(c) == 7)
            c->id = EMPTY_REF;
    } else {
        if (pal_of(c) == 5) c->id = EMPTY_REF;
        if (pal_of(c - 1) == 6) c[-1].id = EMPTY_REF;
        if (pal_of(c - 2) == 6) c[-2].id = EMPTY_REF;
    }
}

/* the image columns fully on screen, lo << 16 | hi + 1 (0: none; at most 32
 * of them: plane A is as wide as the screen, column c goes to plane column
 * c & 31, also for an image wider than the plane) */
static u32 visible_cols(const PlaneImg *im, s16 h)
{
    s16 lo = (7 - h) >> 3, hi = (256 - 8 - h) >> 3;    /* h + 8c in 0..248 */
    if (lo < 0) lo = 0;
    if (hi > im->w - 1) hi = im->w - 1;
    if (lo > hi)
        return 0;
    return ((u32)lo << 16) | (hi + 1);
}

/* the window rows holding something cleared */
static void clear_win_rows(void)
{
    u32 nz = win_nz;
    u32 *w = (u32 *)win[0];
    for (; nz; nz >>= 1, w += 16)
        if (nz & 1) {
            w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0; w[4] = 0; w[5] = 0; w[6] = 0; w[7] = 0;
            w[8] = 0; w[9] = 0; w[10] = 0; w[11] = 0; w[12] = 0; w[13] = 0; w[14] = 0; w[15] = 0;
        }
    win_nz = 0;
}
/* image rows into window rows k.. (cols: lo << 16 | hi + 1, the columns on
 * screen); returns the number of rows */
static u8 build_rows(s8 img, u32 cols, u8 mir, u8 k)
{
    const PlaneImg *im = &plane_img[img];
    const u16 *m = plane_map + im->map;
    u16 attr = (((u16)im->pal << 13) | img_prio(img)) + tbase - 1;
    u8 r, c, lo = cols >> 16, hi = (u8)cols - 1;
    for (r = 0; r < im->h; r++, m += im->w) {
        u16 *d = win[k + r];
        if (!cols)
            continue;
        for (c = lo; c <= hi; c++) {
            u8 j = mir ? im->w - 1 - c : c;
            d[(c + col_base) & 31] = m[j] ? (attr + m[j]) ^ (mir ? 0x800 : 0) : 0;
        }
    }
    return im->h;
}

#define ROWS_MASK(n) ((n) >= 32 ? 0xFFFFFFFFUL : (1UL << (n)) - 1)
#ifdef PLANE_STATS
u16 stat_wst, stat_wrows;
#endif
/* two images in plane rows 0.., side by side (their columns apart) */
static void write_cols2(s8 img, u32 cols, s8 base, s8 img2, u32 cols2, s8 base2)
{
    u8 h, h2, pr, half;
    u32 rows;
#ifdef PLANE_STATS
    stat_wst++;
#endif
    clear_win_rows();
    col_base = base;
    h = build_rows(img, cols, 0, 0);
    col_base = base2;
    h2 = build_rows(img2, cols2, 0, 0);
    col_base = 0;
    if (h2 > h)
        h = h2;
    rows = ROWS_MASK(h);
    win_nz = rows;
    wr_n = 0;
    for (half = 0; half < 2; half++) {
        u32 v = vram_nz[half] | (half ? 0 : rows);
        for (pr = half << 5; v; v >>= 1, pr++)
            if (v & 1) {
                wr_row[wr_n] = pr;
                wr_win[wr_n] = !half && pr < h ? pr : 0xFF;
                wr_n++;
            }
    }
    vram_nz[0] = rows;
    vram_nz[1] = 0;
}
/* image rows into plane rows 0..h-1, a second image (img2 >= 0) into plane
 * rows 32..32+h2-1 (built in win[]), the other rows that hold something
 * cleared */
static const u16 zero_row[32] __attribute__((aligned(64))) = { 0 };   /* (in ROM: DMA reads it there) */
static void write_static(s8 img, u32 cols, u8 mir, s8 img2, u32 cols2, u8 mir2)
{
    u8 h = 0, h2 = 0, pr, half;
    u32 rows[2];
#ifdef PLANE_STATS
    stat_wst++;
#endif
    clear_win_rows();
    if (img >= 0)
        h = build_rows(img, cols, mir, 0);
    if (img2 >= 0)
        h2 = build_rows(img2, cols2, mir2, h);
    win_nz = ROWS_MASK(h + h2);         /* (cleared by the full path when it takes over) */
    rows[0] = ROWS_MASK(h);
    rows[1] = ROWS_MASK(h2);
    /* rows to write: the image rows, and the other rows that hold something */
    wr_n = 0;
    for (half = 0; half < 2; half++) {
        u32 v = vram_nz[half] | rows[half];
        for (pr = half << 5; v; v >>= 1, pr++)
            if (v & 1) {
                u8 r = pr & 31;
                wr_row[wr_n] = pr;
                wr_win[wr_n] = half ? (r < h2 ? h + r : 0xFF) : (r < h ? r : 0xFF);
                wr_n++;
            }
    }
    vram_nz[0] = rows[0];
    vram_nz[1] = rows[1];
}

#define FX_CALLS 64                     /* effect calls handled per frame */
static u8 fx_ix[FX_CALLS];              /* their indices in calls[] */
#define FX_THIN 10                      /* more bomb refs than this: half of them per redraw */
static u8 thin_phase;
/* Text rows 1-27 (the score line is the window plane): on plane A while it
 * shows no large enemy and no effect (no per-line sprite limit: 16 sprites a
 * line in H32 cut long lines, e.g. the credits, the stage clear bonus), else
 * 8x8 sprites (xsp.c) */
#define ST_TEXT    (-3)                 /* st_img: plane A holds the text */
#define TEXT_ATTR  (0x8000 | (TEXT_PAL << 13) | FONT_TILE_BASE)
extern u8 text_grid[32][32];
extern u32 text_row_mask[32];
extern u8 text_pl_dirty;
u8 text_on_plane;                       /* (xsp.c: no text sprites) */

extern u8 text_map[28];                 /* screen row r shows text row text_map[r] (x68.c) */
u8 text_row0(void);                     /* (src/main.c: the rows before it in the score panel) */
static u32 text_rows(void)
{
    u32 m = 0;
    u8 r;
    for (r = text_row0(); r < 28; r++)
        if (text_row_mask[text_map[r]])
            m |= 1UL << r;
    return m;
}

static void write_text(void)
{
    u8 r, c, pr, half;
    clear_win_rows();
    for (r = text_row0(); r < 28; r++)
        if (text_row_mask[text_map[r]]) {
            u16 *d = win[r];
            const u8 *t = text_grid[text_map[r]];
            for (c = 0; c < 32; c++, t++)
                *d++ = *t ? TEXT_ATTR + (*t & 0x7F) : 0;
            win_nz |= 1UL << r;
        }
    wr_n = 0;
    for (half = 0; half < 2; half++) {
        u32 v = vram_nz[half] | (half ? 0 : win_nz);
        for (pr = half << 5; v; v >>= 1, pr++)
            if (v & 1) {
                wr_row[wr_n] = pr;
                wr_win[wr_n] = pr < WROWS && (win_nz >> pr & 1) ? pr : 0xFF;
                wr_n++;
            }
    }
    vram_nz[0] = win_nz;
    vram_nz[1] = 0;
    text_pl_dirty = 0;
}

/* x_wait (no frame built): the changed text into plane A if it shows the
 * text; 0 = the sprites show it */
u8 plane_text_refresh(void)
{
    if (st_img != ST_TEXT && (shown >= 0 || was_fx || st_img >= 0))
        return text_on_plane = 0;
    hs = vs = 0;
    split = 0;
    if (text_rows()) {
        write_text();
        st_img = ST_TEXT;
    } else if (st_img == ST_TEXT) {
        write_static(-1, 0, 0, -1, 0, 0);
        st_img = -1;
    }
    return text_on_plane = 1;
}

#ifndef FX_PERIOD
#define FX_PERIOD 3                     /* effects redrawn every FX_PERIOD frames */
#endif
static u8 held_ok, fx_tick;
static s8 held_img;
static s16 held_hs, held_vs;
static u8 fx_hold(s8 img, s16 best, u16 nfx)
{
    u16 i;
    if (++fx_tick >= FX_PERIOD)
        fx_tick = 0;
    if (!fx_tick || !held_ok || img != held_img || hs != held_hs || vs != held_vs)
        return 0;
    for (i = 0; i < held_n; i++)        /* (still in use: not replaced this frame) */
        xsp_touch(held_key[i]);
#ifndef PLANE_FX_DEBUG
    for (i = 0; i < nfx; i++)
        calls[fx_ix[i]].id = EMPTY_REF;
#endif
    if (img >= 0)
        drop_image_calls(img, &calls[best]);
    was_fx = 1;
    wr_n = 0;
    return 1;
}

#define REF_SHOT   0xB5                 /* (obj200's charging shots: between its arm calls) */

/* obj200 body call i: the arm calls after it (upper left, upper right,
 * lower left, lower right: a), and the pose of each side (closed or open:
 * by the upper arm's x, all within 2 px of the pose's, else none: moving;
 * tools/convert_sprites.py O200_POSE); -1 if not all four are there.  (Once
 * per body and frame: the last ones kept.) */
extern u16 xframe;
static s8 o200_pose(const Call *c, u16 i, u16 *a)
{
    static const s8 pose[2][4] = { { -48, -32, -56, 8 }, { -56, -36, -64, 12 } };
    static const u16 want[4] = { REF_O200 + 1, REF_O200 + 1, REF_O200 + 2, REF_O200 + 2 };
    static u16 m_i[2], m_f[2], m_a[2][4];
    static s8 m_r[2];
    static u8 m_n;
    s16 bx = c->x >> 6, by = c->y >> 6, r[4][2];
    u16 j = i + 1;
    u8 k, side[2];
    for (k = 0; k < 2; k++)
        if (m_f[k] == xframe && m_i[k] == i) {
            a[0] = m_a[k][0]; a[1] = m_a[k][1]; a[2] = m_a[k][2]; a[3] = m_a[k][3];
            return m_r[k];
        }
    k = m_n++ & 1;
    m_f[k] = xframe; m_i[k] = i; m_r[k] = -1;
    for (k = 0; k < 4; k++, j++) {
        while (j < ncalls && calls[j].id == REF_SHOT && j < i + 8)
            j++;
        if (j >= ncalls || calls[j].id != want[k] || ((calls[j].info >> 14) & 1) != (k & 1))
            return -1;
        r[k][0] = (calls[j].x >> 6) - bx;
        r[k][1] = (calls[j].y >> 6) - by;
        a[k] = j;
    }
    for (k = 0; k < 2; k++) {           /* (the right side mirrored) */
        s16 ux = k ? -r[1][0] : r[0][0], lx = k ? -r[3][0] : r[2][0];
        s16 d = -48 - ux;               /* 0 closed .. 8 open */
        const s8 *p;
        u8 m = d >= 4;
        p = pose[m];
        if ((u16)(ux - p[0] + 2) > 4 || (u16)(r[k][1] - p[1] + 2) > 4
            || (u16)(lx - p[2] + 2) > 4 || (u16)(r[2 + k][1] - p[3] + 2) > 4)
            return -1;
        side[k] = m;
    }
    k = (m_n - 1) & 1;
    m_r[k] = side[0] * 2 + side[1];
    m_a[k][0] = a[0]; m_a[k][1] = a[1]; m_a[k][2] = a[2]; m_a[k][3] = a[3];
    return m_r[k];
}

/* obj200 body call c: the whole image with its arms' poses, else the body
 * alone (the arms stay sprites); by the body's palette 8 / 7 (flashing: the
 * one shown) */
static s8 o200_image(const Call *c, u16 i)
{
    u8 p = pal_of(c);
    s8 k;
    u16 a[4];
    if (p == 9)
        return IMG_O1F0;                /* (the type $1F0 large enemy: the body alone) */
    k = o200_pose(c, i, a);
    if (p != 8 && p != 7) {             /* hit flash: as the one shown (arms: the mid boss) */
        if (k < 0 && shown == IMG_O1F0)
            return IMG_O1F0;
        p = (u8)(shown - IMG_O200P) >= 10 || (u8)(shown - IMG_O200P) < 4 || shown == IMG_O200P + 8 ? 8 : 7;
    }
    return k >= 0 ? IMG_O200P + (p == 7 ? 4 : 0) + k : IMG_O200P + 8 + (p == 7);
}

/* the image would show (some of it) on screen */
static u8 on_screen(const Call *c, s8 img)
{
    const PlaneImg *im = &plane_img[img];
    s16 x = (c->x >> 6) + im->ox, y = (c->y >> 6) + im->oy;
    return x < 256 && x + im->w * 8 > 0 && y < 224 && y + im->h * 8 > 0;
}

/* large enemies coming two at a time (the stage 5 battleships, the stage 2
 * mid boss obj200 (palettes 8 / 7), the type $1F0 large enemy, obj21f): the second one on plane A too, side by side (2-cell
 * column scroll, colmode) or one above the other (the H-int split) */
#define PAIR_IMG(m) ((u8)((m) - IMG_O21A) < 2 || (m) == IMG_O1F0 || (u8)((m) - 3) < 2 \
                     || (u8)((m) - IMG_O21F9) < 2 || (u8)((m) - IMG_O200P) < 10 || (m) == IMG_O20F)
static s8 pair_img(const Call *c)       /* (of a call in big_ix, the image it has) */
{
    u16 id = c->id;
    if ((u16)(id - REF_O21A) <= 1)
        return IMG_O21A + (id - REF_O21A);
    if (id == REF_O20F + 2)
        return IMG_O20F;
    if (id == REF_O200) {
        s8 m = o200_image(c, c - calls);    /* (palette 9: IMG_O1F0) */
        return m >= 0 ? m : IMG_O1F0;
    }
    return ((u8)(shown - IMG_O21F9) < 2 || pal_of(c) == 9 ? IMG_O21F9 : 3) + (id - REF_O21F);
}

/* the call of ref id within tol pixels of (x, y) with these flips near call
 * i, -1 if none */
static s16 call_near(u16 i, u16 id, s16 x, s16 y, u16 flip, u8 tol)
{
    u16 j = i > 8 ? i - 8 : 0, e = i + 9 < ncalls ? i + 9 : ncalls;
    for (; j < e; j++) {
        const Call *d = &calls[j];
        if (d->id == id && (d->info & 0x4000) == flip && (u16)((d->x >> 6) - x + tol) <= 2 * tol
            && (u16)((d->y >> 6) - y + tol) <= 2 * tol)
            return j;
    }
    return -1;
}

/* obj20F centre call i: its parts right after it (within 2 px of their
 * places): $B4F either side, the pods $B4E either side (the right ones
 * mirrored) -> a (if not 0).  (Once per call and frame: the last kept.) */
static u8 o20f_parts(const Call *c, u16 i, u16 *a)
{
    static const s8 dx[4] = { -23, 22, -52, 51 }, dy[4] = { 4, 4, -13, -13 };
    static const u16 id[4] = { REF_O20F + 1, REF_O20F + 1, REF_O20F, REF_O20F };
    static u16 m_i[2] = { 0xFFFF, 0xFFFF }, m_f[2];
    static u8 m_r[2], m_n;
    s16 x = c->x >> 6, y = c->y >> 6;
    u8 k, e;
    for (e = 0; e < 2; e++)
        if (m_i[e] == i && m_f[e] == xframe)
            break;
    if (e == 2) {
        e = m_n++ & 1;
        m_i[e] = i; m_f[e] = xframe; m_r[e] = 0;
        if (i + 5u > ncalls)
            return 0;
        for (k = 0; k < 4; k++) {
            const Call *d = &calls[i + 1 + k];
            if (d->id != id[k] || ((d->info >> 14) & 1) != (k & 1)
                || (u16)((d->x >> 6) - x - dx[k] + 2) > 4 || (u16)((d->y >> 6) - y - dy[k] + 2) > 4)
                return 0;
        }
        m_r[e] = 1;
    }
    if (m_r[e] && a)
        for (k = 0; k < 4; k++)
            a[k] = i + 1 + k;
    return m_r[e];
}

/* the stage 6 boss obj210 from its unflipped call i: the mirrored one at its
 * position, $B57 16 px above $B53 / $B58 16 px either side of $B52 (each
 * with its mirror) when there; o210_ix: the calls the image shows */
static s8 o210_image(const Call *c, u16 i)
{
    u16 k = c->id - REF_O210;
    s16 x = c->x >> 6, y = c->y >> 6, m, a, b;
    static const u8 base[5] = { 0, 1, 3, 4, 6 };
    s8 img = IMG_O210 + base[k];
    o210_n = 0;
    o210_ix[o210_n++] = i;
    if ((m = call_near(i, c->id, x, y, 0x4000, 0)) >= 0)
        o210_ix[o210_n++] = m;
    if (k == 2) {
        a = call_near(i, REF_O210 + 6, x, y - 16, 0, 2);   /* (2 px off: the image all */
        b = call_near(i, REF_O210 + 6, x, y - 16, 0x4000, 2);  /* the same, no reloading) */
        if (a >= 0 && b >= 0) {
            o210_ix[o210_n++] = a; o210_ix[o210_n++] = b;
            img = IMG_O210 + 5;
        }
    } else if (k == 1) {
        a = call_near(i, REF_O210 + 7, x - 16, y, 0, 2);
        b = call_near(i, REF_O210 + 7, x + 16, y, 0x4000, 2);
        if (a >= 0 && b >= 0) {
            o210_ix[o210_n++] = a; o210_ix[o210_n++] = b;
            img = IMG_O210 + 2;
        }
    }
    if ((u8)(img - (IMG_O210 + 3)) < 2 && (u8)(shown - IMG_O210) < 7 && plane_img[shown].group == loaded) {
        /* $B53 alone / $B54: the animation from one form to the next, cut (no
         * reloading: the form shown kept, the parts moving about hidden) */
        u16 j = i > 8 ? i - 8 : 0, e = i + 12u < ncalls ? i + 12 : ncalls;
        for (; j < e && o210_n < sizeof(o210_ix); j++)
            if ((u16)(calls[j].id - (REF_O210 + 6)) <= 1 && j != i)
                o210_ix[o210_n++] = j;
        img = shown;
    }
    return img;
}

/* the battleships (obj20A) of the frame: from the centre's position (in
 * pixels) the parts there in palette 6 (mask bit 0 left wing, 1 right
 * wing, 2 centre: image IMG_SHIP + mask - 1, the whole ship pre-composited
 * with the wings in front, tools/convert_sprites.py); a flashing or
 * damaged part ($B45) stays a sprite (in front of the plane) */
typedef struct { s16 x, y; u8 mask, drop; u8 part[3]; } Ship;   /* drop: parts whose sprite goes */
static Ship ship[SHIP_MAX];
static u8 nship;
static s8 shown_ship = -1;              /* the ship of the image shown */

static void ship_add(const Call *c, u8 i)
{
    s16 x = c->x >> 6, y = c->y >> 6;
    u8 bit = 0, k;
    if (c->id == REF_SHIP) {
        bit = 4;
    } else {                            /* a wing: 128 px out, 24 px up */
        y += 24;
        if (c->info & 0x4000) {
            x -= 128; bit = 2;
        } else {
            x += 128; bit = 1;
        }
    }
    if (c->id == REF_SHIP + 2 || (pal_of(c) != 6 && pal_of(c) < 14))
        bit = 0;
    for (k = 0; k < nship && (ship[k].x != x || ship[k].y != y); k++)
        ;
    if (k == nship) {
        if (nship == SHIP_MAX)
            return;
        ship[k].x = x; ship[k].y = y; ship[k].mask = ship[k].drop = 0;
        nship++;
    }
    if (bit) {                          /* (a part flashing, palette 14 / 15: in the image, */
        ship[k].mask |= bit;            /* its flash sprite drawn over it) */
        if (pal_of(c) == 6)
            ship[k].drop |= bit;
        ship[k].part[bit >> 1] = i;     /* (bit 1, 2, 4: part 0, 1, 2) */
    }
}

/* the image's cells on screen at all */
static u8 ship_on_screen(const Ship *sh)
{
    const PlaneImg *im = &plane_img[IMG_SHIP + sh->mask - 1];
    s16 x = sh->x + im->ox, y = sh->y + im->oy;
    return x < 256 && x + im->w * 8 > 0 && y < 224 && y + im->h * 8 > 0;
}

static void ship_drop(const Ship *sh)
{
    u8 b;
#ifdef SHIP_DEBUG
    return;                             /* (debug: the sprites drawn over the plane image) */
#endif
    for (b = 0; b < 3; b++)
        if (sh->drop & (1 << b))
            calls[sh->part[b]].id = EMPTY_REF;
}

static void ship_drop_shown(void)
{
    if (shown_ship >= 0)
        ship_drop(&ship[shown_ship]);
}

/* formation mode: the units placed (in the order of their tops: those
 * sharing a 2-cell column with one placed above it need a gap holding an
 * 8-line boundary below it, the line of the switch; plane rows 0..31, a
 * free row between two units of a column), their rows written when the
 * layout changes, the VSRAM events, their calls dropped */
#ifdef FM_DEBUG
u32 fm_stat[4];                         /* (debug) units, placed, why not: column gap, rows / events */
#endif
typedef struct {                        /* (a unit, in fm_write) */
    const PlaneImg *im;
    s16 x, y;                           /* its image's top left */
    s8 img;
    u8 ix, r0, nr, pr;                  /* (fm_ix, the rows on screen, in the window) */
    u8 band;
    s8 base;                            /* (its first column in the window) */
} FmU;
/* the units of the formations shown on plane A (the rest stay sprites): in
 * a column of them (64 px apart one under the other) those of the leftmost
 * column (one band); in rows of them (64 px apart side by side, level) each
 * row a band (fm_write: those not overlapping the ones placed before them,
 * from the left).  Units in neither (the formations on winding paths) stay
 * sprites. */
static void fm_select(void)
{
    s16 x[FM_MAX], cv[8], rv[8], rx[8];
    u8 ci[FM_MAX], ri[FM_MAX], cn[8], rn[8], rok[8], rb[8];
    u8 k, j, n = fm_n, m = 0, nc = 0, nr = 0, col = 0xFF;
    fm_skip = 0;
#ifndef NO_FMSKIP
    if (st_img == IMG_FM && n == fm_rawn && fm_seen >= 8) {
        for (k = 0; k < n; k++) {       /* (each one placed the same unit: as moving, the */
                                        /* same image; columns at a screen edge: fm_write) */
            const Call *c = &calls[fm_ix[k]];
            s16 dx = (c->x >> 6) - fm_lx[k], dy = (c->y >> 6) - fm_ly[k];
            const PlaneImg *im = &plane_img[fm_img[k]];
            s16 xa = (c->x >> 6) + im->ox, xb = xa + im->w * 8;
            u8 held = fm_praw >> k & 1;     /* (placed, or giving a band's scroll) */
            for (j = 0; !held && j < FM_BANDS; j++)
                held = fm_bev[j] != 0xFE && (fm_brx[j] == k || fm_bry[j] == k);
            if (held && (fm_img[k] != fm_limg[k] || (u16)(dx + 4) > 8
                                       || (u16)(dy + 4) > 8 || (dx && dy)))
                break;
            if ((xb >= 8 && xa <= 248) != (xb - dx >= 8 && xa - dx <= 248))
                break;                  /* (a column all on screen or none: as it was) */
        }
        for (j = 0; k == n && j < FM_BANDS; j++)
            if (fm_bev[j] != 0xFE
                && ((calls[fm_ix[fm_brx[j]]].x >> 6) + plane_img[fm_img[fm_brx[j]]].ox) >> 6 != fm_bblk[j])
                k = 0;
        if (k == n) {
            for (k = 0; k < n; k++) {
                fm_lx[k] = calls[fm_ix[k]].x >> 6; fm_ly[k] = calls[fm_ix[k]].y >> 6;
            }
            fm_skip = 1;
            return;
        }
    }
#endif
    for (k = 0; k < n; k++) {
        fm_lx[k] = calls[fm_ix[k]].x >> 6; fm_ly[k] = calls[fm_ix[k]].y >> 6; fm_limg[k] = fm_img[k];
    }
    if ((u8)(fm_img[0] - IMG_O212) < 2) {   /* obj212: each one a band, from the top */
        fm_col = 0;
        for (k = 0; k < n; k++) {
            u8 r = 0;
            for (j = 0; j < n; j++)
                r += fm_ly[j] < fm_ly[k] || (fm_ly[j] == fm_ly[k] && j < k);
            if (r < FM_BANDS) {
                fm_ix[m] = fm_ix[k]; fm_img[m] = fm_img[k]; fm_band[m] = r; fm_raw[m] = k;
                m++;
            }
        }
        goto selected;
    }
    for (k = 0; k < n; k++) {           /* (the columns by x, the rows by y: 20 px for one) */
        const Call *cl = &calls[fm_ix[k]];
        s16 a = cl->x >> 6, b = cl->y >> 6;
        x[k] = a;
        for (j = 0; j < nc && (u16)(a - cv[j] + 2) > 4; j++)
            ;
        if (j == nc && nc < 8) {
            cv[nc] = a; cn[nc++] = 0;
        }
        ci[k] = j;
        if (j < nc)
            cn[j]++;
        for (j = 0; j < nr && (u16)(b - rv[j] + 2) > 4; j++)
            ;
        if (j == nr && nr < 8) {
            rv[nr] = b; rx[nr] = a; rn[nr] = 0; rok[nr++] = 1;
        }
        ri[k] = j;
        if (j < nr) {
            rn[j]++;
            if (a < rx[j])
                rx[j] = a;
        }
    }
    for (j = 0; j < nc; j++)            /* a column: the leftmost of 3 at the least */
        if (cn[j] >= 3 && (col == 0xFF || cv[j] < cv[col]))
            col = j;
    fm_col = col != 0xFF;
    if (fm_col) {
        for (k = 0; k < n; k++)
            if (ci[k] == col) {
                fm_ix[m] = fm_ix[k]; fm_img[m] = fm_img[k]; fm_band[m] = 0; fm_raw[m] = k;
                m++;
            }
    } else {                            /* rows (level, x68.c x_pal_upload) of 3 at the least, */
        for (k = 0; k < n; k++)         /* 64 px apart (or a multiple: one shot down): fm_write */
            if (ri[k] < nr && ((x[k] - rx[ri[k]] + 2) & 63) > 4)
                rok[ri[k]] = 0;
        for (j = 0; j < nr; j++) {      /* (the rows from the top) */
            u8 q, r = 0;
            for (q = 0; q < nr; q++)
                r += rok[q] && rn[q] >= 3 && rv[q] < rv[j];
            rb[j] = rok[j] && rn[j] >= 3 && r < FM_BANDS ? r : 0xFF;
        }
        for (k = 0; k < n; k++) {
            u8 q = ri[k] < nr ? rb[ri[k]] : 0xFF;
            if (q != 0xFF) {
                fm_ix[m] = fm_ix[k]; fm_img[m] = fm_img[k]; fm_band[m] = q; fm_raw[m] = k;
                m++;
            }
        }
    }
  selected:
    fm_rawn = n;
    for (k = 0; k < n; k++)
        fm_rband[k] = 0xFF;
    for (k = 0; k < m; k++)
        fm_rband[fm_raw[k]] = fm_band[k];
    if (!m)
        fm_seen = 0;
    else if (fm_seen < 8)
        fm_seen++;
    fm_n = fm_seen < 8 ? 0 : m;         /* (8 frames running: not units of a winding */
}                                       /* formation lined up for a moment) */

static void fm_write(void)
{
    FmU u[FM_MAX], *ord[FM_MAX], *cu[FM_PL];
    FmRect cur[FM_PL];
    FmU *bu[FM_BANDS][FM_MAX];
    s16 tb[FM_BANDS], bb[FM_BANDS];
    u8 bn[FM_BANDS];
    u8 nb = 0, nev = 0;
    u16 n = fm_n, k, j, nn = 0, R = 0;
    s16 lo = -0x7FFF;                   /* (the bottom of the bands above; none) */
    u32 rows = 0, ch = 0;               /* (the rows of the units now, the rows changed) */
    u32 sig[FM_BANDS];
    s16 fx[FM_BANDS], fy[FM_BANDS];
    if (fm_skip) {                      /* (fm_select: the scrolls only) */
        for (k = 0; k < FM_BANDS; k++) {
            const Call *a, *b;
            s16 f, v;
            if (fm_bev[k] == 0xFE)
                continue;
            a = &calls[fm_ix[fm_brx[k]]]; b = &calls[fm_ix[fm_bry[k]]];
            f = ((a->x >> 6) + plane_img[fm_img[fm_brx[k]]].ox) & 63;
            v = fm_bR0[k] * 8 - ((b->y >> 6) + plane_img[fm_img[fm_bry[k]]].oy);
            if (fm_bev[k] == 0xFF) {
                hs = f; vs = v;
            } else {
                fm_ev[fm_bev[k]].cols = f; fm_ev[fm_bev[k]].v = (u16)v & 511;
            }
            {   /* the window column at the screen edges (in part at both: blank) another:
                 * the one there before now all on screen (the units' cells there), the
                 * new one blank */
                u8 so = fm_bf[k] & 7 ? ((256 - fm_bf[k]) >> 3) & 31 : 0xFF;
                u8 sn = f & 7 ? ((256 - f) >> 3) & 31 : 0xFF, r;
                if (so == sn)
                    goto same;
                s16 y0 = (b->y >> 6) + plane_img[fm_img[fm_bry[k]]].oy;
                if (sn != 0xFF) {
                    u8 i;
                    for (r = fm_bR0[k]; r < fm_bRe[k]; r++)
                        win[r][sn] = 0;
                    for (i = 0; i < fm_nold; i++) {     /* (the units' columns written: */
                        FmRect *q = &fm_old[i];         /* one fewer) */
                        u8 pr = (q->k >> 8) & 31, lo = q->c >> 8, hi = (u8)q->c;
                        u8 ci = (sn - (s8)q->k) & 31;
                        if (pr < fm_bR0[k] || pr >= fm_bRe[k] || ci < lo || ci > hi)
                            continue;
                        if (ci == lo)
                            lo++;
                        else
                            hi = ci - 1;
                        q->c = lo << 8 | hi;
                    }
                }
                if (so != 0xFF && so != sn) {
                    s16 cx = ((so * 8 + f) & 255);  /* (its place on screen) */
                    u8 j;
                    for (j = 0; j < fm_rawn; j++) {
                        const Call *c = &calls[fm_ix[j]];
                        const PlaneImg *im = &plane_img[fm_img[j]];
                        s16 xa = (c->x >> 6) + im->ox, hp, col, ya;
                        const u16 *m;
                        u16 attr;
                        if (!(fm_praw >> j & 1) || fm_rband[j] != k)
                            continue;
                        hp = ((xa - f + 4) >> 3) * 8 + f;   /* (as placed: fm_write) */
                        col = (cx - hp) >> 3;
                        if (cx < hp || col >= im->w)
                            continue;
                        ya = (c->y >> 6) + im->oy;
                        attr = (((u16)im->pal << 13) | img_prio(fm_img[j])) + tbase - 1;
#ifdef FM_DEBUG
                        attr &= 0x1FFF;
#endif
                        m = plane_map + im->map + col;
                        for (r = 0; r < im->h; r++, m += im->w)
                            win[fm_bR0[k] + ((ya - y0) >> 3) + r][so] = *m ? attr + *m : 0;
                        {                       /* (its columns written: one more) */
                            u32 key = (u32)(u8)fm_img[j] << 24 | (u32)im->h << 13
                                      | (u32)(fm_bR0[k] + ((ya - y0) >> 3)) << 8 | (u8)((hp - f) >> 3);
                            u8 i;
                            for (i = 0; i < fm_nold; i++)
                                if (fm_old[i].k == key) {
                                    u8 lo = fm_old[i].c >> 8, hi = (u8)fm_old[i].c;
                                    if (col < lo) lo = col;
                                    if (col > hi) hi = col;
                                    fm_old[i].c = lo << 8 | hi;
                                }
                        }
                    }
                }
                ch |= ROWS_MASK(fm_bRe[k] - fm_bR0[k]) << fm_bR0[k];
            }
          same:
            fm_bf[k] = f;
        }
        wr_n = 0;
        {
            u32 v = ch;
            u16 pp;
            for (pp = 0; v; v >>= 1, pp++)
                if (v & 1) {
                    wr_row[wr_n] = pp;
                    wr_win[wr_n] = win_nz >> pp & 1 ? pp : 0xFF;
                    wr_n++;
                }
        }
        for (k = 0; k < fm_rawn; k++)
            if (fm_praw >> k & 1) {
                Call *cl = &calls[fm_ix[k]];
                if (pal_of(cl) == FM_PAL(fm_img[k])) {
                    cl->id = EMPTY_REF;
                    if (fm_img[k] != IMG_FM) {
                        cl[-1].id = EMPTY_REF; cl[-2].id = EMPTY_REF;
                    }
                }
            }
        return;
    }
    for (k = 0; k < FM_BANDS; k++) {
        tb[k] = 224; bb[k] = 0; bn[k] = 0; sig[k] = 0; fx[k] = 0x7FFF;
    }
    for (k = 0; k < n; k++) {           /* (by x: insertion; each band's top, extent on screen) */
        FmU *p = &u[k];
        const Call *cl = &calls[fm_ix[k]];
        s16 t, e;
        u8 q = fm_band[k];
        u32 v;
        p->img = fm_img[k]; p->im = &plane_img[p->img]; p->ix = k | 0x80; p->band = q;
        p->x = (cl->x >> 6) + p->im->ox; p->y = (cl->y >> 6) + p->im->oy;
        for (j = k; j > 0 && ord[j - 1]->x > p->x; j--)
            ord[j] = ord[j - 1];
        ord[j] = p;
        if (fx[q] == 0x7FFF) {
            fx[q] = p->x; fy[q] = p->y;
        }
        v = (u32)(u16)(p->x - fx[q]) << 16 | (u16)(p->y - fy[q]);
        sig[q] = (sig[q] << 4 | sig[q] >> 28) ^ v ^ (u8)p->img ^ (u32)k << 8;
        t = p->y; e = t + p->im->h * 8;
        if (e <= 0 || t >= 224 || p->x + p->im->w * 8 <= 0 || p->x >= 256)
            continue;
        {                               /* (its columns on screen) */
            s16 l = (7 - p->x) >> 3, h = (248 - p->x) >> 3;
            if (l < 0) l = 0;
            if (h > p->im->w - 1) h = p->im->w - 1;
            sig[q] ^= (u32)(l | h << 6 | 0x1000) << 12;
        }
        if (t < 0) t = 0;
        if (e > 224) e = 224;
        if (t < tb[q]) tb[q] = t;
        if (e > bb[q]) bb[q] = e;
    }
    if (st_img == IMG_FM && n == fm_sn && fm_col == fm_scol) {
        for (k = 0; k < FM_BANDS; k++) {    /* as it was: the scrolls only */
            if (sig[k] != fm_bsig[k])
                break;
            if (fm_bev[k] != 0xFE && ((u[fm_bax[k]].x >> 6) != fm_bblk[k]
                                      || (fm_nev && u[fm_bay[k]].y != fm_by0[k])))
                break;
        }
        if (k == FM_BANDS) {
            for (k = 0; k < FM_BANDS; k++) {
                s16 f = u[fm_bax[k]].x & 63, v = fm_bR0[k] * 8 - u[fm_bay[k]].y;
                fm_bf[k] = f;
                if (fm_bev[k] == 0xFF) {
                    hs = f; vs = v;
                } else if (fm_bev[k] != 0xFE) {
                    fm_ev[fm_bev[k]].cols = f; fm_ev[fm_bev[k]].v = (u16)v & 511;
                }
            }
            wr_n = 0;
            for (k = 0; k < n; k++)
                if (fm_placed >> k & 1)
                    u[k].ix &= 0x7F;
            goto drop;
        }
    }
    for (k = 0; k < n; k++) {           /* (each band's units, from the left) */
        u8 q = ord[k]->band;
        bu[q][bn[q]++] = ord[k];
    }
    for (k = 0; k < FM_BANDS; k++) {
        fm_bsig[k] = sig[k]; fm_bev[k] = 0xFE;
    }
    fm_sn = n; fm_scol = fm_col;
    for (k = 0; k < FM_BANDS; k++) {    /* the bands (fm_select: by their tops), each in its */
        u8 q = 0xFF;                    /* window rows, its scroll from the H-int */
        u32 used = 0;                   /* (the window's columns taken in the band) */
        u8 any = 0;
        u16 R0 = R;
        s16 f = 0, y0 = 0x7FFF, L = 0, e = 0;
        for (j = 0; j < FM_BANDS; j++)
            if (bb[j] > 0 && (q == 0xFF || tb[j] < tb[q]))
                q = j;
        if (q == 0xFF)
            break;
        bb[q] = 0;                      /* (done) */
        for (j = 0; j < bn[q]; j++) {   /* (its units below those above: its top) */
            FmU *p = bu[q][j];
            if (p->y + p->im->h * 8 > 0 && p->y < 224 && p->y >= lo && p->y < y0) {
                y0 = p->y;
                fm_bay[q] = p->ix & 0x7F;
                fm_bry[q] = fm_raw[p->ix & 0x7F];
            }
        }
        if (y0 == 0x7FFF || (lo >= 0 && nev == FM_EV))
            continue;
        if (lo >= 0) {                  /* the line between: from the bottom of those above */
            L = y0 - 16;                /* (2 blank rows below them) to the top of these */
            if (L < lo)                 /* (blank rows above them: as many as it takes) */
                L = lo;
            if (L > lo + 16)
                L = lo + 16;
            j = (y0 - L + 7) >> 3;
            R0 = R - 2 + (j > 2 ? j : 2);
        }
        for (j = 0; j < bn[q]; j++) {   /* (from the left: those overlapping one placed: sprites) */
            FmU *p = bu[q][j];
            const PlaneImg *im = p->im;
            s16 dy = p->y - y0, hp;
            u16 r;
            if (p->y + im->h * 8 <= 0 || p->y >= 224 || p->x + im->w * 8 <= 0
                || p->x >= 256 || p->y < lo || (dy & 7) || nn == FM_PL)
                continue;
            r = R0 + (dy >> 3);
            if (r + im->h > 32)
                continue;
            if (!any) {                 /* (the band's horizontal scroll: all 64 px apart, */
                f = p->x & 63;          /* their places in the window kept while it moves) */
                fm_bax[q] = p->ix & 0x7F;
                fm_bblk[q] = p->x >> 6;
                fm_brx[q] = fm_raw[p->ix & 0x7F];
            }
            p->base = (p->x - f + 4) >> 3;
            hp = p->base * 8 + f;
            {
                s16 l = (7 - hp) >> 3, h = (248 - hp) >> 3;
                u32 m;
                u8 sh;
                if (l < 0) l = 0;
                if (h > im->w - 1) h = im->w - 1;
                if (l > h)
                    continue;
                m = ROWS_MASK(h - l + 1);
                sh = (p->base + l) & 31;
                if (sh)
                    m = m << sh | m >> (32 - sh);
                if (!fm_col && (used & m))
                    continue;
                used |= m;
                any = 1;
                p->ix &= 0x7F;
                p->r0 = 0; p->nr = im->h; p->pr = r;
                cur[nn].k = (u32)(u8)p->img << 24 | (u32)p->nr << 13 | (u32)p->pr << 8 | (u8)p->base;
                cur[nn].c = l << 8 | h;
                cu[nn] = p;
                rows |= ROWS_MASK(p->nr) << p->pr;
                nn++;
                if (r + im->h + 2 > R)
                    R = r + im->h + 2;
                if (p->y + im->h * 8 > e)
                    e = p->y + im->h * 8;
            }
        }
        if (!any)
            continue;
        fm_bR0[q] = R0; fm_by0[q] = y0; fm_bRe[q] = R - 2; fm_bf[q] = f;
        if (lo < 0) {                   /* (the first: from the top) */
            hs = f; vs = R0 * 8 - y0;
            fm_bev[q] = 0xFF;
        } else {
            fm_bev[q] = nev;
            fm_ev[nev].line = L;
            fm_ev[nev].cols = f; fm_ev[nev].v = (u16)(R0 * 8 - y0) & 511;
            nev++;
        }
        lo = e > 224 ? 224 : e;
        nb++;
    }
    fm_nev = nev;
    if (st_img != IMG_FM) {             /* (from another mode: all rewritten) */
        clear_win_rows();
        ch = vram_nz[0];
        fm_nold = 0;
    }
    for (k = 0; k < fm_nold; k++) {     /* (those gone or moved: cleared) */
        FmRect *q = &fm_old[k];
        if (k < nn && cur[k].k == q->k && cur[k].c == q->c) {
            cur[k].c |= 0x8000;         /* (as it was: left) */
            continue;
        }
        for (j = 0; j < nn; j++)
            if (cur[j].k == q->k && cur[j].c == q->c)
                break;
        if (j < nn) {
            cur[j].c |= 0x8000;
            continue;
        }
        {                               /* (but for the cells a new one in the same rows */
            u16 r = (u16)(q->k >> 8) & 31, e = r + ((u16)(q->k >> 13) & 63), b = (u8)q->k;
            u16 lo = q->c >> 8, cnt = (u8)q->c - lo;    /* writes over) */
            u32 cov = 0;
            for (j = 0; j < nn; j++) {
                u16 r1 = (u16)(cur[j].k >> 8) & 31;
                if (!(cur[j].c & 0x8000) && r1 <= r && r1 + ((u16)(cur[j].k >> 13) & 63) >= e) {
                    u16 l1 = cur[j].c >> 8, n1 = (u8)cur[j].c - l1 + 1;
                    u8 sh = (l1 + (u8)cur[j].k) & 31;
                    u32 m = ROWS_MASK(n1);
                    cov |= sh ? m << sh | m >> (32 - sh) : m;
                }
            }
            ch |= ROWS_MASK(e - r) << r;
            for (; r < e; r++) {
                u16 *d = win[r], cc = lo + b, i = cnt;
                do {
                    if (!(cov >> (cc & 31) & 1))
                        d[cc & 31] = 0;
                    cc++;
                } while (i--);
            }
        }
    }
    for (j = 0; j < nn; j++) {          /* (the new ones: written) */
        const FmU *p = cu[j];
        const PlaneImg *im;
        const u16 *m;
        u16 attr, r, w, lo, cnt;
        if (cur[j].c & 0x8000) {
            cur[j].c &= 0x7FFF;
            continue;
        }
        im = p->im;
        attr = (((u16)im->pal << 13) | img_prio(p->img)) + tbase - 1;
#ifdef FM_DEBUG
        attr &= 0x1FFF;                 /* (debug: in the BG palette, to tell them) */
#endif
        lo = cur[j].c >> 8; cnt = (u8)cur[j].c - lo; w = im->w;
        for (k = 0; k < fm_nold; k++)   /* (the same in the same place, its columns on screen */
            if (fm_old[k].k == cur[j].k) {  /* changed at a screen edge: the new ones only) */
                u16 ol = fm_old[k].c >> 8, oh = (u8)fm_old[k].c;
                if (ol <= lo && oh >= lo + cnt)
                    cnt = 0xFFFF;       /* (fewer: the old one's clearing left these) */
                else if (ol <= lo && oh >= lo && oh < lo + cnt) {
                    cnt -= oh + 1 - lo; lo = oh + 1;
                } else if (oh >= lo + cnt && ol > lo && ol <= lo + cnt)
                    cnt = ol - 1 - lo;
                break;
            }
        if (cnt == 0xFFFF)
            continue;
        m = plane_map + im->map + p->r0 * w + lo;
        for (r = 0; r < p->nr; r++, m += w) {
            u16 *d = win[p->pr + r];
            const u16 *s = m;
            u16 cc = lo + (u8)p->base, i = cnt;
            do {
                u16 t = *s++;
                d[cc & 31] = t ? attr + t : 0;
                cc++;
            } while (i--);
        }
        ch |= ROWS_MASK(p->nr) << p->pr;
    }
    for (j = 0; j < nn; j++)
        fm_old[j] = cur[j];
    fm_nold = nn;
    win_nz = rows;
    wr_n = 0;
    {
        u32 v = ch;
        u16 pp;
        for (pp = 0; v; v >>= 1, pp++)
            if (v & 1) {
                wr_row[wr_n] = pp;
                wr_win[wr_n] = rows >> pp & 1 ? pp : 0xFF;
                wr_n++;
            }
        for (v = vram_nz[1], pp = 32; v; v >>= 1, pp++)
            if (v & 1) {
                wr_row[wr_n] = pp;
                wr_win[wr_n] = 0xFF;
                wr_n++;
            }
    }
    vram_nz[0] = rows;
    vram_nz[1] = 0;
    st_img = IMG_FM; st_img2 = -1; st_cols = 0;
    fm_placed = 0; fm_praw = 0;
    for (k = 0; k < n; k++)
        if (!(u[k].ix & 0x80)) {
            fm_placed |= 1 << k;
            fm_praw |= 1 << fm_raw[k];
        }
drop:
    for (k = 0; k < n; k++) {           /* (a hit flash stays a sprite, over it) */
        Call *cl;
        if (u[k].ix & 0x80)
            continue;
        cl = &calls[fm_ix[k]];
        if (pal_of(cl) == FM_PAL(u[k].img)) {
            cl->id = EMPTY_REF;
            if (u[k].img != IMG_FM) {
                cl[-1].id = EMPTY_REF; cl[-2].id = EMPTY_REF;
            }
        }
    }
}

void plane_frame(void)
{
    s16 best = -1, bd = 0x7FFF, top;
    u8 nbomb = 0, big = 0;
    u16 nfx;
    s8 img = -1;
    u16 i, fx = 0;
    u8 k;
    s8 bship = -1, bship2 = -1;         /* the ship (index in ship[]) shown, the second one */
    u8 nbig = 0, big_ix[3];             /* large enemies two of which may show (call indices) */
    s16 big2 = -1, big2v = -1;
    Call *c;
    nship = 0;
    fm_n = 0;
    /* the calls xobj_set noted (src/xsp_fast.s): the large enemy, the effects */
    for (k = 0; k < pl_n; k++) {
        u16 id;
        i = pl_ix[k];
        c = &calls[i];
        id = c->id;
        if ((u16)(id - 0xB6) <= 0xBF - 0xB6 || (u16)(id - 0xAA9) <= 0xAAE - 0xAA9) {
            if (is_effect(c)) {
                if (fx < FX_CALLS)
                    fx_ix[fx] = i;
                fx++;
                nbomb += (c->info & 63) == 0x3F;
            }
            continue;
        }
        big |= id < REF_O21F && id != REF_O1A1 /* (pl_refmap: the bosses and mid bosses below obj21f) */
               && (u16)(id - REF_O21A) > 1      /* (the stage 5 battleship fires no lasers, */
               && (u16)(id - REF_O20F) > 2      /* nor obj20F, */
               && (u16)(id - REF_O212) > 1;     /* nor obj212) */
        if (BOSS_IMG(img))              /* (a boss chosen) */
            continue;
        if (id == REF_O216 && (pal_of(c) == 8 || (pal_of(c) >= 14 && (u8)(shown - IMG_O216) < 10))) {
            u8 k = 0;                   /* the stage 5 boss (hit flash: the image kept) */
            if ((u16)(c[1].id - REF_O216 - 1) <= 8 && c[1].x == c->x && c[1].y == c->y && i + 1u < ncalls)
                k = c[1].id - REF_O216;
            best = i;
            img = IMG_O216 + k;
            boss_seen = 1;
            continue;
        }
        if (id == REF_O210 + 5 && (pal_of(c) == 3 || (pal_of(c) >= 14 && shown == IMG_O210F))) {
            best = i;                   /* its last form (hit flash: the image kept) */
            img = IMG_O210F;
            boss_seen = 1;
            continue;
        }
        if ((u16)(id - REF_O210) <= 4 && !(c->info & 0x4000)
            && (pal_of(c) == 3 || (pal_of(c) >= 14 && (u8)(shown - IMG_O210) < 7))) {
            best = i;                   /* the stage 6 boss (hit flash: the image kept) */
            img = o210_image(c, i);
            boss_seen = 1;
            continue;
        }
        if (id == REF_O212 && fm_n < FM_MAX && (pal_of(c) == 7 || pal_of(c) >= 14) && i >= 2
            && c[-2].id == REF_O212 + 1 && c[-1].id == REF_O212 + 1 && (c[-1].info & 0x4000)) {
            s16 d = (c->x >> 6) - (c[-2].x >> 6);   /* obj212: its wings closed or open */
            if ((d == 28 || d == 36) && (c[-1].x >> 6) - (c->x >> 6) == d
                && (c->y >> 6) - (c[-2].y >> 6) == d - 28 && c[-1].y == c[-2].y) {
                fm_ix[fm_n] = i; fm_img[fm_n++] = IMG_O212 + (d == 36);
            }
            continue;
        }
        if (fm_n < FM_MAX && (pal_of(c) == 6 || pal_of(c) >= 14)) {   /* (hit flash: kept) */
            if (id == REF_F15) {
                fm_ix[fm_n] = i; fm_img[fm_n++] = IMG_FM;
                continue;
            }
            if (id == REF_O1A1 && i >= 2 && c[-2].id == REF_O1A1 + 1 && c[-1].id == REF_O1A1 + 1
                && (c[-1].info & 0x4000)) {
                /* the pods 16 + s px either side, s px up (s 1..12, the spawner's $6E):
                 * the image of the nearest even s */
                s16 sp = (c->x >> 6) - (c[-2].x >> 6) - 16;
                if ((u16)(sp - 1) < 12 && (c[-1].x >> 6) - (c->x >> 6) == sp + 16
                    && (c->y >> 6) - (c[-2].y >> 6) == sp && c[-1].y == c[-2].y) {
                    fm_ix[fm_n] = i;
                    fm_img[fm_n++] = sp >= 11 ? IMG_FM + 1 : IMG_FM + 1 + ((sp + 1) >> 1);
                }
            }
        }
        if ((u16)(id - REF_SHIP) <= 2) {
            ship_add(c, i);             /* (the ships: after this loop) */
            continue;
        }
        if (id == REF_BOSS || (u16)(id - REF_O205) <= 6 || (u16)(id - REF_O207) <= 9) {   /* a boss: first */
            best = i;
            img = id == REF_BOSS ? 2 : id >= REF_O207 ? IMG_O207 + (id - REF_O207)
                : id == REF_O205 + 6 ? IMG_O205 : IMG_O205 + 1 + (id - REF_O205);
            boss_seen = 1;
            continue;
        }
        {
            s8 im = -1;
            if (id == REF_CRAB + 2 && i >= 2 && c[-2].id == REF_CRAB && c[-1].id == REF_CRAB + 1
                && c[-2].y == c->y && c[-1].y == c->y)
                im = ((c->x - c[-2].x) >> 6) <= 2 ? 0 : 1;
            else if (id == REF_O20F + 2 && !(c->info & 0x4000)
                     && (pal_of(c) == 8 || (pal_of(c) >= 10 && shown == IMG_O20F)) && o20f_parts(c, i, 0))
                im = IMG_O20F;              /* (hit flash: the image kept) */
            else if ((u16)(id - REF_O21F) <= 1) {
                u8 p = pal_of(c);           /* (hit flash: the image kept) */
                if (p == 9 || (p >= 14 && (u8)(shown - IMG_O21F9) < 2))
                    im = IMG_O21F9 + (id - REF_O21F);
                else if (p == 7 || (p >= 14 && (u8)(shown - 3) < 2))
                    im = 3 + (id - REF_O21F);
            }
            else if (id == REF_O200)
                im = o200_image(c, i);
            else if ((u16)(id - REF_O21A) <= 1
                     && (pal_of(c) == 6 || (pal_of(c) >= 14 && (u8)(shown - IMG_O21A) < 2)))
                im = IMG_O21A + (id - REF_O21A);    /* (hit flash: the image kept) */
            if (im >= 0 && PAIR_IMG(im) && nbig < 3 && on_screen(c, im))
                big_ix[nbig++] = i;         /* (two of them: src/plane.c colmode / split) */
            else if (id == REF_O213 && (pal_of(c) == 3 || (pal_of(c) >= 14 && shown == IMG_O213)))
                im = IMG_O213;
            else if ((u16)(id - REF_O20E) <= 7
                     && (pal_of(c) == 9 || (pal_of(c) >= 14 && (u8)(shown - IMG_O20E) < 8)))
                im = IMG_O20E + (id - REF_O20E);    /* (hit flash, palette 15: the image kept, */
                                                    /* the flash sprite over it) */
            else if (id == REF_O1A1 && i >= 2 && pal_of(c) == 6 && c[-2].id == REF_O1A1 + 1
                     && c[-1].id == REF_O1A1 + 1 && (c[-1].info & 0x4000)
                     && c[-2].x == c->x - 28 * 64 && c[-1].x == c->x + 28 * 64
                     && c[-2].y == c->y - 12 * 64 && c[-1].y == c[-2].y)
                im = IMG_O1A1;              /* (the topmost of a formation, below) */
            if (im >= 0 && !on_screen(c, im))
                im = -1;                    /* (off screen: nothing to gain) */
            if (im >= 0) {
                /* of several: one whose tiles are in VRAM (or coming in), then the
                 * one nearest to the one shown: no switching back and forth
                 * between two image groups (each switch reloads the tiles) */
                u8 g = plane_img[im].group;
                s16 d = 0;
                if (im == IMG_O1A1) {       /* the topmost, the one shown kept */
                    s16 e = (c->x >> 6) - ax, f = (c->y >> 6) - ay;
                    d = c->y >> 6;
                    if (shown == IMG_O1A1 && (e < 0 ? -e : e) + (f < 0 ? -f : f) < 8)
                        d -= 500;
                } else if (shown >= 0) {
                    s16 e = (c->y >> 6) - ay;
                    d = (c->x >> 6) - ax;
                    d = (d < 0 ? -d : d) + (e < 0 ? -e : e);
                }
                if (PAIR_IMG(im)) {         /* (of a pair: the one more on screen first, */
                    const PlaneImg *pi = &plane_img[im];    /* the other may go with it) */
                    s16 t = (c->y >> 6) + pi->oy, b = t + pi->h * 8;
                    d += (pi->h * 8 - ((b > 224 ? 224 : b) - (t < 0 ? 0 : t))) * 4;
                }
                if (g != loaded)
                    d += g == loading ? 1000 : 2000;
                if (d < bd) {
                    bd = d; best = i; img = im;
                }
            }
        }
    }
    if (!BOSS_IMG(img))
        for (k = 0; k < nship; k++) {
            /* the one shown kept while it lasts; else the one most on screen */
            const Ship *sh = &ship[k];
            s16 d, l, r, e = sh->x - ax, f = sh->y - ay;
            s8 im;
            if (!sh->mask || !ship_on_screen(sh))
                continue;
            im = IMG_SHIP + sh->mask - 1;
            l = sh->x + plane_img[im].ox; r = l + plane_img[im].w * 8;
            d = -((r > 256 ? 256 : r) - (l < 0 ? 0 : l));
            if ((u8)(shown - IMG_SHIP) < 7 && (e < 0 ? -e : e) + (f < 0 ? -f : f) < 8)
                d -= 1000;
            if (plane_img[im].group != loaded)
                d += plane_img[im].group == loading ? 1000 : 2000;
            if (d < bd) {
                bd = d; img = im; bship = k;
                best = sh->part[sh->mask & 4 ? 2 : sh->mask & 2 ? 1 : 0];
            }
        }
    if (fx > FX_CALLS)
        fx = FX_CALLS;                  /* (the rest stay sprites) */
    if (best >= 0 && !BOSS_IMG(img) && !nbomb && !was_fx)
        fx = 0;                         /* a large enemy (not a boss: its death) shown: the */
                                        /* explosions stay sprites (composited into its tiles */
                                        /* on plane A: some 140 lines a frame) */
    xsp_pairs(big);                     /* a boss / mid boss about: its lasers paired */
    fm_on = 0;
#ifndef NO_FM
    if (fm_n >= 2 && (best < 0 || img == IMG_O1A1) && !fx && !was_fx && bg_on)
        fm_select();
    else
        fm_n = fm_seen = 0;
    if (fm_n) {
        best = fm_ix[0];                /* (formation mode: the group loaded as for an image) */
        img = fm_img[0];
        fm_on = 1;
    }
#endif
    if (best >= 0) {
        u8 g = plane_img[img].group;
        if (loaded != g) {
            if (loading != g) {
                u16 nt = plane_group[g].ntiles;
                u8 lend = nt > ENEMY_TILES ? (nt - ENEMY_TILES + 15) >> 4 : 0;   /* (as few as it needs) */
                loaded = 0xFF;              /* (its tiles are being overwritten) */
                load_pos = 0;
                loading = g;
                xsp_lend(lend);
                tbase = ENEMY_BASE - lend * 16;
            }
            if ((u8)(img - IMG_O210) < 7)
                drop_image_calls(img, &calls[best]);    /* (too large for sprites: not */
                                                        /* drawn, the few frames it takes) */
            img = -1;                   /* sprites until the tiles are in */
        }
    }
    if (img < 0)
        bship = -1;
    shown = img;
    shown_ship = bship;
    plane_behind = img_prio(img) != 0;
    plane_turrets = (u8)(img - IMG_O21A) < 2;
    split = 0;
    if (bship >= 0 && !fx && !was_fx && bg_on) {
        /* a second ship above or below the first one, with a gap holding an
         * 8-line boundary (the H-int comes every 8 lines while split) */
        const PlaneImg *im = &plane_img[img];
        s16 y1 = ship[bship].y + im->oy, h1 = im->h * 8, bd2 = 0x7FFF;
        for (k = 0; k < nship; k++) {
            const Ship *sh = &ship[k];
            const PlaneImg *im2;
            s16 y2, h2, sl, d, e, f;
            if ((s8)k == bship || !sh->mask || !ship_on_screen(sh))
                continue;
            im2 = &plane_img[IMG_SHIP + sh->mask - 1];
            y2 = sh->y + im2->oy; h2 = im2->h * 8;
            if (y2 >= y1 + h1)
                sl = (y1 + h1 + 7) & ~7;
            else if (y1 >= y2 + h2)
                sl = (y2 + h2 + 7) & ~7;
            else
                continue;
            if (sl > (y2 > y1 ? y2 : y1))
                continue;
            e = sh->x - ax2; f = sh->y - ay2;
            d = (e < 0 ? -e : e) + (f < 0 ? -f : f) < 8 ? -1000 : 0;   /* (the one shown kept) */
            if (d < bd2) {
                bd2 = d; bship2 = k; split = sl;
            }
        }
    }
    colmode = 0;
    {
        s16 best2c = -1;
        if (img >= 0 && PAIR_IMG(img) && !fx && !was_fx && bg_on)
            for (k = 0; k < nbig; k++)
                if (big_ix[k] != best && plane_img[pair_img(&calls[big_ix[k]])].group == plane_img[img].group) {
                    /* (their cells' ranges 16 px apart at least: no 2-cell
                     * column holds both) */
                    const Call *c1 = &calls[best], *c2 = &calls[big_ix[k]];
                    const PlaneImg *i1 = &plane_img[img], *i2 = &plane_img[pair_img(c2)];
                    s16 a0 = (c1->x >> 6) + i1->ox, a1 = a0 + i1->w * 8;
                    s16 b0 = (c2->x >> 6) + i2->ox, b1 = b0 + i2->w * 8;
                    if (b0 - a1 >= 16 || a0 - b1 >= 16) {
                        best2c = big_ix[k];
                        break;
                    }
                }
        if (best2c >= 0) {
            colmode = 1;
            big2 = best2c;
        } else if (img >= 0 && PAIR_IMG(img) && (u8)(img - IMG_O21A) >= 2 && !fx && !was_fx && bg_on) {
            /* one above the other (a gap holding an 8-line boundary): the
             * H-int split as for the stage 4 battleships */
            const PlaneImg *i1 = &plane_img[img];
            s16 y1 = (calls[best].y >> 6) + i1->oy, h1 = i1->h * 8;
            for (k = 0; k < nbig; k++)
                if (big_ix[k] != best && plane_img[pair_img(&calls[big_ix[k]])].group == i1->group) {
                    const PlaneImg *i2 = &plane_img[pair_img(&calls[big_ix[k]])];
                    s16 y2 = (calls[big_ix[k]].y >> 6) + i2->oy, h2 = i2->h * 8, sl;
                    if (y2 >= y1 + h1)
                        sl = (y1 + h1 + 7) & ~7;
                    else if (y1 >= y2 + h2)
                        sl = (y2 + h2 + 7) & ~7;
                    else
                        continue;
                    if (sl > (y2 > y1 ? y2 : y1) || sl <= 0 || sl >= 224)
                        continue;
                    big2v = big_ix[k]; split = sl;
                    break;
                }
        }
    }
    if (img >= 0) {
        const PlaneImg *im = &plane_img[img];
        c = &calls[best];
        if (bship >= 0) {
            ax = ship[bship].x; ay = ship[bship].y;
        } else {
            ax = c->x >> 6; ay = c->y >> 6;
        }
        mirror = 0;
        hs = ax + im->ox;
        vs = -(ay + im->oy);
        plane_frames++;
    } else {
        mirror = 0;
        hs = vs = 0;
#ifdef PLANE_FX_DEBUG
        hs = 96;                        /* effects drawn shifted, next to their sprites */
#endif
    }

    /* no effect now or last frame: the image alone, written into plane rows
     * 0..h-1 only when it or its edge columns change (plane A scrolls it) */
    if (!fx && !was_fx) {
        u32 cols = img >= 0 ? visible_cols(&plane_img[img], hs) : 0;
        wr_n = 0;
        if (img < 0 && text_rows()) {   /* plane A free: the text rows */
            st_img2 = -1;
            if (st_img != ST_TEXT || text_pl_dirty)
                write_text();
            st_img = ST_TEXT;
            text_on_plane = 1;
            return;
        }
        text_on_plane = 0;
        if (fm_on && img >= 0) {
#ifdef TEST_LINES
            {
                extern u32 lines_fm;
                u8 v0, v1;
                __asm__ volatile("move.w #0x2700,%%sr" ::: "cc");
                v0 = *(volatile u8 *)0xC00008;
                fm_write();
                v1 = *(volatile u8 *)0xC00008;
                __asm__ volatile("move.w #0x2300,%%sr" ::: "cc");
                lines_fm += (u8)(v1 - v0);
            }
#else
            fm_write();
#endif
            return;
        }
        if (colmode) {
            /* the left one and the right one; one horizontal scroll for both:
             * the fine part splits the difference (2 px off at most) */
            const Call *cl = &calls[best], *cr = &calls[big2];
            s8 il = img, ir = pair_img(cr), bl, br;
            s16 xl, xr, f, d;
            u32 kl, kr;
            if ((cr->x >> 6) < (cl->x >> 6)) {
                const Call *t = cl; s8 ti = il;
                cl = cr; cr = t; il = ir; ir = ti;
            }
            xl = (cl->x >> 6) + plane_img[il].ox;
            xr = (cr->x >> 6) + plane_img[ir].ox;
            d = (((xr - xl) & 7) + 4) % 8 - 4;
            f = (xl + (d >> 1)) & 7;
            bl = (s8)((xl - f + 4) >> 3); br = (s8)((xr - f + 4) >> 3);
            hs = f;
            vs = -((cl->y >> 6) + plane_img[il].oy);
            vsr = -((cr->y >> 6) + plane_img[ir].oy);
            {   /* the first 2-cell column holding some of the right one */
                s16 x = br * 8 + f;
                colk = x <= 0 ? 0 : x >= 256 ? 16 : (u8)(x >> 4);
            }
            kl = visible_cols(&plane_img[il], bl * 8 + f);
            kr = visible_cols(&plane_img[ir], br * 8 + f);
            if (st_img != il || st_img2 != ir || kl != st_cols || kr != st_cols2 || bl != st_base || br != st_base2)
                write_cols2(il, kl, bl, ir, kr, br);
            st_img = il; st_img2 = ir; st_cols = kl; st_cols2 = kr; st_base = bl; st_base2 = br;
            drop_image_calls(il, (Call *)cl);
            drop_image_calls(ir, (Call *)cr);
            return;
        }
        st_base = st_base2 = 0;
        if (bship2 >= 0) {
            /* the upper ship in plane rows 0.., the lower one in rows 32.. */
            const Ship *s2 = &ship[bship2];
            s8 img2 = IMG_SHIP + s2->mask - 1, iu = img, il = img2;
            const PlaneImg *im2 = &plane_img[img2];
            s16 h2 = s2->x + im2->ox, v2 = s2->y + im2->oy;
            u32 cu, cl;
            ax2 = s2->x; ay2 = s2->y;
            if (v2 < -vs) {             /* the second one above: it takes rows 0.. */
                s16 t = hs;
                hs = h2; hs2 = t;
                vs2 = 256 + vs; vs = -v2;
                iu = img2; il = img;
            } else {
                hs2 = h2; vs2 = 256 - v2;
            }
            cu = visible_cols(&plane_img[iu], hs);
            cl = visible_cols(&plane_img[il], hs2);
            if (st_img != iu || st_img2 != il || cu != st_cols || cl != st_cols2)
                write_static(iu, cu, 0, il, cl, 0);
            st_img = iu; st_img2 = il; st_cols = cu; st_cols2 = cl;
            ship_drop(&ship[bship]);
            ship_drop(s2);
            return;
        }
        if (big2v >= 0) {
            /* the upper one in plane rows 0.., the lower one in rows 32.. */
            const Call *c2 = &calls[big2v];
            s8 img2 = pair_img(c2), iu = img, il = img2;
            const PlaneImg *im2 = &plane_img[img2];
            s16 h2 = (c2->x >> 6) + im2->ox, v2 = (c2->y >> 6) + im2->oy;
            u32 cu, cl;
            if (v2 < -vs) {             /* the second one above: it takes rows 0.. */
                s16 t = hs;
                hs = h2; hs2 = t;
                vs2 = 256 + vs; vs = -v2;
                iu = img2; il = img;
            } else {
                hs2 = h2; vs2 = 256 - v2;
            }
            cu = visible_cols(&plane_img[iu], hs);
            cl = visible_cols(&plane_img[il], hs2);
            if (st_img != iu || st_img2 != il || cu != st_cols || cl != st_cols2)
                write_static(iu, cu, 0, il, cl, 0);
            st_img = iu; st_img2 = il; st_cols = cu; st_cols2 = cl;
            drop_image_calls(img, &calls[best]);
            drop_image_calls(img2, (Call *)c2);
            return;
        }
        split = 0;
        ay2 = -999;
        if (img != st_img || cols != st_cols || st_img2 >= 0)
            write_static(img, cols, 0, -1, 0, 0);
        st_img = img; st_cols = cols; st_img2 = -1;
        if (img >= 0)
            drop_image_calls(img, &calls[best]);
        return;
    }
    /* thinning: effects are redrawn every other frame; in between the plane
     * keeps the last ones (their tiles kept in the cache) when the image
     * has not moved */
    if (fx && fx_hold(img, best, fx))
        return;
    thin_phase ^= 1;
    was_fx = fx != 0;
    split = 0;
    ay2 = -999;
    st_img = -2;                        /* (the static image is rewritten afterwards) */
    st_img2 = -1;
    text_on_plane = 0;
    held_n = 0;

    /* the visible rows */
    top = vs >> 3;
    clear_win_rows();
    if (img >= 0) {
        put_image(img, top);
        drop_image_calls(img, &calls[best]);
    }
    for (k = 0, nfx = fx, fx = 0; k < nfx; k++) {
        c = &calls[fx_ix[k]];
        {
            u8 bomb = (c->info & 63) == 0x3F;
            fx++;
            if (bomb && nbomb > FX_THIN && ((fx + thin_phase) & 1))
                ;                       /* thinned: every other bomb ref, alternately */
            else if (!put_ref(c, top) && !bomb)
                continue;               /* (the meshed bomb: skipped this frame, not a sprite) */
#ifndef PLANE_FX_DEBUG
            c->id = EMPTY_REF;
#endif
        }
    }
    if (fx)
        plane_fx++;
    held_ok = fx && held_n <= HELD_MAX;
    held_img = img; held_hs = hs; held_vs = vs;

    /* rows to write: holding something now or in VRAM */
    wr_n = 0;
    {                                   /* (rows off the window: cleared) */
        u32 wm[2] = { 0, 0 }, v;
        u8 half, pr;
        for (k = 0; k < WROWS; k++) {
            pr = (top + k) & 63;
            wm[pr >> 5] |= 1UL << (pr & 31);
        }
        for (half = 0; half < 2; half++) {
            v = vram_nz[half] & ~wm[half];
            vram_nz[half] &= wm[half];
            for (pr = half << 5; v; v >>= 1, pr++)
                if (v & 1) {
                    wr_row[wr_n] = pr; wr_win[wr_n] = 0xFF; wr_n++;
                }
        }
    }
    for (k = 0; k < WROWS; k++) {
        u8 pr = (top + k) & 63, nz = win_nz >> k & 1;
        u32 bit = 1UL << (pr & 31), *v = &vram_nz[pr >> 5];
        if (nz || (*v & bit)) {
            wr_row[wr_n] = pr; wr_win[wr_n] = k; wr_n++;
            if (nz) *v |= bit; else *v &= ~bit;
        }
    }
}

/* x_screen_clear: plane A is being cleared */
void plane_fx_forget(void)
{
    held_ok = 0;
}

void plane_reset(void)
{
    u16 *w = &win[0][0], i;
    for (i = 0; i < WIN_ROWS * 32; i++)
        w[i] = 0;
    vram_nz[0] = vram_nz[1] = 0;
    win_nz = 0;
    wr_n = 0;
    shown = -1;
    st_img = -1; st_cols = 0;           /* (the cleared plane: nothing written) */
    st_img2 = -1; split = 0; ay2 = -999;
    was_fx = 0;
    boss_seen = 0;
    text_on_plane = 0;
    held_ok = 0;
}

/* DMA must not cross a 128 KB boundary of the source */
/* VDP DMA can't cross a 128 KB boundary of the source: split there (byte
 * arithmetic: the source need not be long-aligned, and an odd number of words
 * before the boundary used to shift the second part by a word) */
void dma_rom(u16 vram, const u32 *src, u16 words)
{
    const u8 *s = (const u8 *)src;
    u32 room = (0x20000 - ((u32)s & 0x1FFFF)) >> 1;
    if (words > room) {
        vdp_dma_vram(vram, s, room);
        vram += room * 2; s += room * 2; words -= room;
    }
    vdp_dma_vram(vram, s, words);
}

/* VBlank: tile loading, plane rows, scroll */
void plane_upload(void)
{
    u8 i;
    if (loading != 0xFF) {
        const PlaneGroup *g = &plane_group[loading];
        u16 n = g->ntiles - load_pos;
        if (n > (tbase < ENEMY_BASE ? 2 * LOAD_CHUNK : LOAD_CHUNK))
            n = tbase < ENEMY_BASE ? 2 * LOAD_CHUNK : LOAD_CHUNK;
        dma_rom((tbase + load_pos) * 32, plane_tiles + (g->tile + load_pos) * 8, n * 16);
        load_pos += n;
        if (load_pos >= g->ntiles) {
            loaded = loading;
            loading = 0xFF;
        }
    }
#ifdef NO_PLANE_DMA
    wr_n = 0;
#endif
#ifdef TEST_ROWS
    {
        extern u32 rows_up, rows_fr, rows_shown;
        rows_up += wr_n; rows_fr += wr_n != 0; rows_shown += shown >= 0;
    }
#endif
    for (i = 0; i < wr_n; i++)
        vdp_dma_vram(VRAM_PLANE_A + wr_row[i] * 64, wr_win[i] == 0xFF ? (const void *)zero_row : win[wr_win[i]], 32);
    wr_n = 0;
}

/* the new frame's plane A scrolls, split and bands for plane_vblank (first in
 * the VBlank, src/main.c; the rows written by plane_upload after it) */
void plane_act(void)
{
    u8 i;
    act_hs = hs; act_vs = vs;
    act_hs2 = hs2; act_vs2 = vs2; act_split = split;
    act_colmode = colmode; act_vsr = vsr; act_colk = colk;
    act_fm_on = fm_on;
    if (fm_on) {
        for (i = 0; i < fm_nev; i++)
            act_fm_ev[i] = fm_ev[i];
#ifdef NO_FMHINT
        act_fm_nev = 0;
#else
        act_fm_nev = fm_nev;
#endif
    } else
        act_fm_nev = 0;
}

/* every VBlank: plane A's scroll at the top of the frame shown, the H-int
 * for its split (src/xsp_fast.s _hint) and the sprite doubler */
void plane_vblank(void)
{
    fm_evi = 0;
    act_fm_ev[act_fm_nev].line = 0xFF;  /* (the end, for src/xsp_fast.s _hint) */
    if (act_colmode) {                  /* 2-cell column scroll: plane A by column, B all alike */
        u8 k;
        vdp_set_reg(11, 0x04);
        VDP_CTRL32 = VSRAM_W(0);
        for (k = 0; k < 16; k++) {
            VDP_DATA = (k < act_colk ? act_vs : act_vsr) & 511;
            VDP_DATA = bg_vs & 511;
        }
    } else {
        vdp_set_reg(11, 0x00);
        VDP_CTRL32 = VSRAM_W(0);
        VDP_DATA = act_vs & 511;
        if (!bg_on)                     /* (plane B shows plane A's table: on top of it) */
            VDP_DATA = act_vs & 511;
    }
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL);
    VDP_DATA = act_hs;
    if (!bg_on)
        VDP_DATA = act_hs;
    split_vs = act_vs2 & 511;
    split_hs = act_hs2;
    split_line = act_split;
    {                                   /* the H-int: the first at the first line with */
        u16 t0 = 0x100, t1 = 0x100, l;  /* something to do (src/xsp_fast.s _hint) or */
        u8 k;                           /* halfway, the second there (register 10 is */
        if (act_split)                  /* taken at each H-int: _hint sets the one */
            t0 = act_split;             /* after the next) */
        if (act_n) {
            l = 96;                     /* (DBL_A) */
            if (l < t0) { t1 = t0; t0 = l; } else if (l > t0 && l < t1) t1 = l;
        }
        for (k = fm_evi; act_fm_ev[k].line != 0xFF && act_fm_ev[k].line <= t1; k++) {
            l = act_fm_ev[k].line;
            if (l < t0) { t1 = t0; t0 = l; } else if (l > t0 && l < t1) t1 = l;
        }
        hint_nx = 0;
        hint_r1 = t1 >= 2 * t0 ? t0 : t0 >> 1;
        if (hint_r1 < 1)
            hint_r1 = 1;
        if (hint_r1 > 248)
            hint_r1 = 248;
        hint_exp = hint_r1;
        vdp_set_reg(10, hint_r1 - 1);
        vdp_set_reg(0, t0 < 0x100 ? 0x14 : 0x04);
    }
}
