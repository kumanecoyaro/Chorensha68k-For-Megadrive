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

#define ENEMY_BASE 1234                 /* VRAM tiles 1234..1535 (xsp.c cache ends at 1234, plane A at 1536) */
_Static_assert(ENEMY_BASE + PLANE_MAX_TILES <= VRAM_PLANE_A / 32, "plane A enemy tiles overlap plane A");
#define EMPTY_REF  REF_EMPTY            /* a dropped call (spr.h: a ref without pieces) */
#define REF_CRAB   0xB1E                /* obj202: left half, right half, centre */
#define REF_BOSS   0xB29                /* obj204 */
#define REF_O21F   0xB79                /* obj21f, $B7A after it fires (images 3, 4) */
#define REF_O200   0xB18                /* obj200: body, arm $B19 x2, arm $B1A x2 (right ones mirrored) */
#define REF_O205   0xB2B                /* stage 2 boss obj205: $B2B..$B30 with pods, $B31 core */
#define IMG_O200   5                    /* obj200 palette 8 arms closed, body alone; palette 7 the same */
#define IMG_O205   9                    /* core, then $B2B..$B30 */
#define REF_O207   0xB34                /* stage 3 boss obj207: $B34..$B3B, second form $B3C / $B3D */
#define IMG_O207   16                   /* $B34..$B3D */
#define IMG_O1F0   26                   /* stage 3 type $1F0: obj200 body $B18, palette 9 */
#define REF_O1A1   0xB16                /* stage 3 type $1A1 (obj1a0): centre $B16 after its pods $B17 */
#define IMG_O1A1   27                   /* the pods fully out (palette 6) */
#define REF_WING   0xB44                /* stage 4 battleship type $20A (obj20A): a wing ($B45 damaged) */
#define IMG_WING   28                   /* a wing (the right one: mirrored) */
#define REF_O20E   0xB46                /* stage 4 large ship type $20E (obj20E): frames $B46..$B4D */
#define IMG_O20E   29                   /* its frames (palette 9) */
#define LOAD_CHUNK 64                   /* tiles per VBlank */
#define WROWS      29                   /* plane rows on screen (224 lines + fine scroll) */

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
static u8 mirror;                       /* it is shown mirrored (a right wing, IMG_WING) */
static s16 ax, ay;                      /* anchor of the shown image (screen pixels) */
static s8 st_img = -2;                  /* image written by write_static (-2: none, rewrite) */
static u32 st_cols;
static u8 was_fx;                       /* effects last frame */
static s16 hs, vs;                      /* plane A scroll: col c at x = c*8 + hs, row r at y = r*8 - vs */
/* two battleships (stage 4's last part: always one above the other): the
 * wing of the second one in plane rows 32.., shown from line split on with
 * scroll hs2 / vs2 (the H-int, src/xsp_fast.s _hint) */
#define WING_MAX   8
static u16 split;                       /* 0: one image */
static s16 hs2, vs2;
static s16 ax2, ay2 = -999;             /* anchor of the second wing (kept while it lasts) */
static s8 st_img2 = -1;
static u32 st_cols2;
static s16 act_hs, act_vs, act_hs2, act_vs2;    /* of the frame shown (plane_vblank) */
static u16 act_split;
extern u16 split_line, split_vs, split_hs, act_n;

static u16 win[WROWS][32];              /* visible rows, window row k = plane row top + k */
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
    u16 attr = ((u16)im->pal << 13) + ENEMY_BASE - 1;
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
static u8 o200_arms(const Call *c, u16 i, u16 *a);

static void drop_image_calls(s8 img, Call *c)
{

    if ((u8)(img - IMG_O200) < 4) {
        u8 p = img < IMG_O200 + 2 ? 8 : 7, k;
        u16 a[4];
        if (pal_of(c) == p)
            c->id = EMPTY_REF;
        if (!((img - IMG_O200) & 1) && o200_arms(c, c - calls, a))
            for (k = 0; k < 4; k++)
                if (pal_of(&calls[a[k]]) == p)
                    calls[a[k]].id = EMPTY_REF;
    } else if (img == IMG_O1F0) {
        if (pal_of(c) == 9)
            c->id = EMPTY_REF;
    } else if ((u8)(img - IMG_O20E) < 8) {
        if (pal_of(c) == 9)
            c->id = EMPTY_REF;
    } else if (img == IMG_WING) {
        if (pal_of(c) == 6)
            c->id = EMPTY_REF;
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
    u16 attr = ((u16)im->pal << 13) + ENEMY_BASE - 1;
    u8 r, c, lo = cols >> 16, hi = (u8)cols - 1;
    for (r = 0; r < im->h; r++, m += im->w) {
        u16 *d = win[k + r];
        if (!cols)
            continue;
        for (c = lo; c <= hi; c++) {
            u8 j = mir ? im->w - 1 - c : c;
            d[c & 31] = m[j] ? (attr + m[j]) ^ (mir ? 0x800 : 0) : 0;
        }
    }
    return im->h;
}

/* image rows into plane rows 0..h-1, a second image (img2 >= 0) into plane
 * rows 32..32+h2-1 (built in win[]), the other rows that hold something
 * cleared */
static u16 zero_row[32];
static void write_static(s8 img, u32 cols, u8 mir, s8 img2, u32 cols2, u8 mir2)
{
    u8 h = 0, h2 = 0, pr, half;
    u32 rows[2];
    clear_win_rows();
    if (img >= 0)
        h = build_rows(img, cols, mir, 0);
    if (img2 >= 0)
        h2 = build_rows(img2, cols2, mir2, h);
    win_nz = (1UL << (h + h2)) - 1;     /* (cleared by the full path when it takes over) */
    rows[0] = (1UL << h) - 1;
    rows[1] = (1UL << h2) - 1;
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
static u32 text_rows(void)
{
    u32 m = 0;
    u8 r;
    for (r = 1; r < 28; r++)
        if (text_row_mask[text_map[r]])
            m |= 1UL << r;
    return m;
}

static void write_text(void)
{
    u8 r, c, pr, half;
    clear_win_rows();
    for (r = 1; r < 28; r++)
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

/* obj200 body call c (index i): its four arm calls follow (the charging
 * shots $B5 it draws may come in between) -> their indices in a[], 1 if
 * they are there with the arms closed (48 / 56 px out) */
#define REF_SHOT   0xB5
static u8 o200_arms(const Call *c, u16 i, u16 *a)
{
    static const u16 want[4] = { REF_O200 + 1, REF_O200 + 1, REF_O200 + 2, REF_O200 + 2 };
    static const s8 dx[4] = { -48, 48, -56, 56 }, dy[4] = { -32, -32, 8, 8 };
    u16 j = i + 1;
    u8 k;
    for (k = 0; k < 4; k++, j++) {
        while (j < ncalls && calls[j].id == REF_SHOT && j < i + 8)
            j++;
        if (j >= ncalls || calls[j].id != want[k] || calls[j].x != c->x + dx[k] * 64
            || calls[j].y != c->y + dy[k] * 64)
            return 0;
        a[k] = j;
    }
    return 1;
}

/* obj200 body call c: the whole image with the arms closed, else the body
 * alone (the arms stay sprites); by the body's palette 8 / 7 (flashing: the
 * one shown) */
static s8 o200_image(const Call *c, u16 i)
{
    u8 im, p = pal_of(c);
    u16 a[4];
    if (p == 9 || (p != 8 && p != 7 && shown == IMG_O1F0))
        return IMG_O1F0;                /* (stage 3: the body alone; hit flash) */
    if (p == 8)
        im = IMG_O200;
    else if (p == 7)
        im = IMG_O200 + 2;
    else if ((u8)(shown - IMG_O200) < 4)    /* (hit flash) */
        im = IMG_O200 + ((shown - IMG_O200) & 2);
    else
        return -1;
    return o200_arms(c, i, a) ? im : im + 1;
}

/* the image's left edge from its call's x: mirrored (as src/xsp_fast.s
 * draws an H-flipped ref: x -> -x - 17) for a right wing */
static s16 img_left(const PlaneImg *im, u16 mirrored)
{
    return mirrored ? -im->ox - im->w * 8 - 16 : im->ox;
}

/* the image would show (some of it) on screen */
static u8 on_screen(const Call *c, s8 img)
{
    const PlaneImg *im = &plane_img[img];
    s16 x = (c->x >> 6) + img_left(im, img == IMG_WING && (c->info & 0x4000)), y = (c->y >> 6) + im->oy;
    return x < 256 && x + im->w * 8 > 0 && y < 224 && y + im->h * 8 > 0;
}

void plane_frame(void)
{
    s16 best = -1, bd = 0x7FFF, top;
    u8 nbomb = 0, big = 0;
    u16 nfx;
    s8 img = -1;
    u16 i, fx = 0;
    u8 k, nwing = 0;
    u8 wing_ix[WING_MAX];
    s16 wing_d[WING_MAX];
    s16 best2 = -1;
    Call *c;
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
        big |= id < REF_O21F && id != REF_O1A1; /* (pl_refmap: the bosses and mid bosses below obj21f) */
        if (img == 2 || (img >= IMG_O205 && img < IMG_O1F0))     /* (a boss chosen) */
            continue;
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
            else if ((u16)(id - REF_O21F) <= 1)
                im = 3 + (id - REF_O21F);
            else if (id == REF_O200)
                im = o200_image(c, i);
            else if (id == REF_WING && pal_of(c) == 6)
                im = IMG_WING;
            else if ((u16)(id - REF_O20E) <= 7 && pal_of(c) == 9)
                im = IMG_O20E + (id - REF_O20E);
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
                if (im == IMG_WING) {
                    /* the one shown kept while it lasts; else the wing farthest
                     * out (coming in at a screen edge: the inner ones are
                     * mostly shot down soon) */
                    s16 l = (c->x >> 6) + img_left(&plane_img[im], c->info & 0x4000);
                    s16 m = l + plane_img[im].w * 4 - 128, e = (c->x >> 6) - ax, f = (c->y >> 6) - ay;
                    d = 400 - (m < 0 ? -m : m);
                    if (shown == IMG_WING && (u16)(mirror << 14) == (c->info & 0x4000)
                        && (e < 0 ? -e : e) + (f < 0 ? -f : f) < 8)
                        d -= 1000;
                } else if (im == IMG_O1A1) {       /* the topmost, the one shown kept */
                    s16 e = (c->x >> 6) - ax, f = (c->y >> 6) - ay;
                    d = c->y >> 6;
                    if (shown == IMG_O1A1 && (e < 0 ? -e : e) + (f < 0 ? -f : f) < 8)
                        d -= 500;
                } else if (shown >= 0) {
                    s16 e = (c->y >> 6) - ay;
                    d = (c->x >> 6) - ax;
                    d = (d < 0 ? -d : d) + (e < 0 ? -e : e);
                }
                if (im == IMG_WING && nwing < WING_MAX) {
                    wing_ix[nwing] = i;
                    wing_d[nwing++] = d;
                }
                if (g != loaded)
                    d += g == loading ? 1000 : 2000;
                if (d < bd) {
                    bd = d; best = i; img = im;
                }
            }
        }
    }
    if (fx > FX_CALLS)
        fx = FX_CALLS;                  /* (the rest stay sprites) */
    xsp_pairs(big);                     /* a boss / mid boss about: its lasers paired */
    if (best >= 0) {
        u8 g = plane_img[img].group;
        if (loaded != g) {
            if (loading != g) {
                loaded = 0xFF;              /* (its tiles are being overwritten) */
                load_pos = 0;
                loading = g;
            }
            img = -1;                   /* sprites until the tiles are in */
        }
    }
    shown = img;
    split = 0;
    if (img == IMG_WING && !fx && !was_fx && bg_on) {
        /* a second battleship's wing, above or below the first one with a
         * gap holding an 8-line boundary (the H-int comes every 8 lines) */
        const PlaneImg *im = &plane_img[IMG_WING];
        s16 y1 = (calls[best].y >> 6) + im->oy, hh = im->h * 8, bd2 = 0x7FFF;
        for (k = 0; k < nwing; k++) {
            s16 y2, sl, d;
            i = wing_ix[k];
            if ((s16)i == best)
                continue;
            c = &calls[i];
            y2 = (c->y >> 6) + im->oy;
            if (y2 >= y1 + hh)
                sl = (y1 + hh + 7) & ~7;
            else if (y1 >= y2 + hh)
                sl = (y2 + hh + 7) & ~7;
            else
                continue;               /* (side by side: the wings of one ship) */
            if (sl > (y2 > y1 ? y2 : y1))
                continue;
            d = wing_d[k];
            {
                s16 e = (c->x >> 6) - ax2, f = (c->y >> 6) - ay2;
                if ((e < 0 ? -e : e) + (f < 0 ? -f : f) < 8)
                    d -= 1000;          /* the second one shown kept while it lasts */
            }
            if (d < bd2) {
                bd2 = d; best2 = i; split = sl;
            }
        }
    }
    if (img >= 0) {
        const PlaneImg *im = &plane_img[img];
        c = &calls[best];
        ax = c->x >> 6; ay = c->y >> 6;
        mirror = img == IMG_WING && (c->info & 0x4000);
        hs = ax + img_left(im, mirror);
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
        if (best2 >= 0) {
            /* the upper wing in plane rows 0.., the lower one in rows 32.. */
            const PlaneImg *im = &plane_img[IMG_WING];
            Call *c2 = &calls[best2];
            u8 mir2 = (c2->info & 0x4000) != 0, mu, ml;
            s16 h2 = (c2->x >> 6) + img_left(im, mir2), v2 = (c2->y >> 6) + im->oy;
            u32 cu, cl;
            ax2 = c2->x >> 6; ay2 = c2->y >> 6;
            if (v2 < -vs) {             /* the second one above: it takes rows 0.. */
                s16 t = hs;
                hs = h2; hs2 = t;
                vs2 = 256 + vs; vs = -v2;
                mu = mir2; ml = mirror;
            } else {
                hs2 = h2; vs2 = 256 - v2;
                mu = mirror; ml = mir2;
            }
            cu = visible_cols(im, hs) ^ ((u32)mu << 31);
            cl = visible_cols(im, hs2) ^ ((u32)ml << 31);
            if (st_img != IMG_WING || st_img2 != IMG_WING || cu != st_cols || cl != st_cols2)
                write_static(IMG_WING, cu & 0x7FFFFFFF, mu, IMG_WING, cl & 0x7FFFFFFF, ml);
            st_img = st_img2 = IMG_WING; st_cols = cu; st_cols2 = cl;
            drop_image_calls(IMG_WING, &calls[best]);
            drop_image_calls(IMG_WING, c2);
            return;
        }
        split = 0;
        ay2 = -999;
        cols ^= (u32)mirror << 31;
        if (img != st_img || cols != st_cols || st_img2 >= 0)
            write_static(img, cols & 0x7FFFFFFF, mirror, -1, 0, 0);
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
    for (i = 0; i < WROWS * 32; i++)
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
        if (n > LOAD_CHUNK)
            n = LOAD_CHUNK;
        dma_rom((ENEMY_BASE + load_pos) * 32, plane_tiles + (g->tile + load_pos) * 8, n * 16);
        load_pos += n;
        if (load_pos >= g->ntiles) {
            loaded = loading;
            loading = 0xFF;
        }
    }
#ifdef NO_PLANE_DMA
    wr_n = 0;
#endif
    for (i = 0; i < wr_n; i++)
        vdp_dma_vram(VRAM_PLANE_A + wr_row[i] * 64, wr_win[i] == 0xFF ? zero_row : win[wr_win[i]], 32);
    wr_n = 0;
    act_hs = hs; act_vs = vs;
    act_hs2 = hs2; act_vs2 = vs2; act_split = split;
}

/* every VBlank: plane A's scroll at the top of the frame shown, the H-int
 * for its split (src/xsp_fast.s _hint) and the sprite doubler */
void plane_vblank(void)
{
    VDP_CTRL32 = VSRAM_W(0);
    VDP_DATA = act_vs & 511;
    if (!bg_on)                         /* (plane B shows plane A's table: on top of it) */
        VDP_DATA = act_vs & 511;
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL);
    VDP_DATA = act_hs;
    if (!bg_on)
        VDP_DATA = act_hs;
    split_vs = act_vs2 & 511;
    split_hs = act_hs2;
    split_line = act_split;
    vdp_set_reg(10, act_split ? 8 - 1 : 96 - 1);   /* (every 8 lines; DBL_A) */
    vdp_set_reg(0, act_split || act_n ? 0x14 : 0x04);
}
