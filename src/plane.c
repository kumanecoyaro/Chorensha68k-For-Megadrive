/*
 * Plane A: large enemies and big effects (the BG uses plane B only, src/bg.c).
 *
 * Large enemies: the stage 1 boss (obj204) and the crab (obj202, two halves
 * and a centre drawn as three refs) are pre-composited by
 * tools/convert_sprites.py into tile images (the crab's halves open 0..4 px:
 * images at 0 and 4 px).  At the end of each frame the recorded XSP calls are
 * searched for one of them; when its tiles are in VRAM its calls are dropped
 * from the sprite list and plane A shows the image at the same place instead
 * (one enemy at a time: the boss first, else the crab nearest to the one
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

#define ENEMY_BASE 1364                 /* VRAM tiles 1364..1523 (xsp.c cache ends at 1364) */
#define EMPTY_REF  REF_EMPTY            /* a dropped call (spr.h: a ref without pieces) */
#define REF_CRAB   0xB1E                /* obj202: left half, right half, centre */
#define REF_BOSS   0xB29                /* obj204 */
#define LOAD_CHUNK 64                   /* tiles per VBlank */
#define WROWS      29                   /* plane rows on screen (224 lines + fine scroll) */

typedef struct { s16 x, y; u16 id, info; } Call;
extern Call calls[];
extern u16 ncalls;
extern const s16 size_w[16], size_h[16];
u16 xsp_tile_for(u16 key);

static u8 loaded = 0xFF, loading = 0xFF;
static u16 load_pos;
static s8 shown = -1;                   /* image shown, -1 = none */
static s16 ax, ay;                      /* anchor of the shown image (screen pixels) */
static s16 hs, vs;                      /* plane A scroll: col c at x = c*8 + hs, row r at y = r*8 - vs */

static u16 win[WROWS][32];              /* visible rows, window row k = plane row top + k */
static u32 win_nz;                      /* window rows holding something */
static u32 vram_nz[2];                  /* plane rows (0..63) holding something in VRAM */
static u8 wr_row[WROWS], wr_win[WROWS]; /* rows to write: plane row, window row */
static volatile u8 wr_n;
u16 plane_frames, plane_fx;             /* (stats for tools/uctrace.py) */

static u8 pal_of(const Call *c) { return (c->info >> 8) & 15; }

/* the player's bomb (expl2 / expl3 / flare refs at the very front, $3F),
 * the large expl2 frames ($B8 / $B9: the boss's death) and flares; the small
 * expl2 frames enemies draw at their own priority stay sprites */
static u8 is_effect(const Call *c)
{
    u16 id = c->id;
#ifdef NO_PLANE_FX
    return 0;
#endif
    if (id >= 0xAA9 && id <= 0xAAE)
        return 1;
    if (id == 0xB8 || id == 0xB9)
        return 1;
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
            if (*m)
                put(c, r, top, attr + *m);
}

/* a ref into the plane cells (like the composite path of xsp_build) */
static void put_ref(const Call *cl, s16 top)
{
    u16 i = ref_start[cl->id], n = ref_start[cl->id + 1];
    u8 objfl = (cl->info >> 14) & 3, opal = pal_of(cl);   /* bit 0 = H, bit 1 = V */
    s16 px = cl->x >> 6, py = cl->y >> 6;
    for (; i < n; i++) {
        const SprPiece *p = &ref_pieces[i];
        u8 sz = p->size & 15, fl = ((p->size >> 4) & 3) ^ objfl, pal = (p->palx ^ opal) & 15;
        s16 w = size_w[sz], h = size_h[sz], dx = p->dx, dy = p->dy, tw = w >> 3, th = h >> 3, tx, ty;
        s16 c0, r0;
        u16 t, attr;
        if (objfl & 1) dx = -dx - 16 - w;
        if (objfl & 2) dy = -dy - 16 - h;
        c0 = (px + dx - hs + 4) >> 3;
        r0 = (py + dy + vs + 4) >> 3;
        if (r0 + th <= top || r0 >= top + WROWS)
            continue;
        t = xsp_tile_for((p->block << 4) | pal);
        if (!t)
            continue;
        attr = ((u16)pal_slot[pal] << 13) | ((fl & 1) << 11) | ((fl & 2) << 11);
        for (tx = 0; tx < tw; tx++)
            for (ty = 0; ty < th; ty++)
                put(c0 + ((fl & 1) ? tw - 1 - tx : tx), r0 + ((fl & 2) ? th - 1 - ty : ty), top,
                    attr | (t + tx * th + ty));
    }
}

void plane_frame(void)
{
    s16 best = -1, bd = 0x7FFF, top;
    s8 img = -1;
    u16 i, fx = 0;
    u8 k;
    Call *c = calls;
    for (i = 0; i < ncalls; i++, c++) {
        if (c->id == REF_BOSS) {
            best = i; img = 2;
            break;
        }
        if (c->id == REF_CRAB + 2 && i >= 2 && c[-2].id == REF_CRAB && c[-1].id == REF_CRAB + 1
            && c[-2].y == c->y && c[-1].y == c->y) {
            s16 d = (c->x >> 6) - ax, e = (c->y >> 6) - ay;
            d = (d < 0 ? -d : d) + (e < 0 ? -e : e);
            if (shown < 0 || d < bd) {
                bd = d; best = i;
                img = ((c->x - c[-2].x) >> 6) <= 2 ? 0 : 1;
            }
        }
    }
    if (best >= 0) {
        u8 g = plane_img[img].group;
        if (loaded != g) {
            if (loading != g) {
                loading = g;
                load_pos = 0;
            }
            img = -1;                   /* sprites until the tiles are in */
        }
    }
    shown = img;
    if (img >= 0) {
        const PlaneImg *im = &plane_img[img];
        c = &calls[best];
        ax = c->x >> 6; ay = c->y >> 6;
        hs = ax + im->ox;
        vs = -(ay + im->oy);
        plane_frames++;
    } else {
        hs = vs = 0;
#ifdef PLANE_FX_DEBUG
        hs = 96;                        /* effects drawn shifted, next to their sprites */
#endif
    }

    /* the visible rows */
    top = vs >> 3;
    for (k = 0; k < WROWS; k++)
        if (win_nz >> k & 1)
            for (i = 0; i < 32; i++)
                win[k][i] = 0;
    win_nz = 0;
    if (img >= 0) {
        put_image(img, top);
        /* drop the calls shown by the plane (a flashing part stays a sprite, over it) */
        c = &calls[best];
        if (img == 2) {
            if (pal_of(c) == 7)
                c->id = EMPTY_REF;
        } else {
            if (pal_of(c) == 5) c->id = EMPTY_REF;
            if (pal_of(c - 1) == 6) c[-1].id = EMPTY_REF;
            if (pal_of(c - 2) == 6) c[-2].id = EMPTY_REF;
        }
    }
    for (i = 0, c = calls; i < ncalls; i++, c++)
        if (is_effect(c)) {
            put_ref(c, top);
#ifndef PLANE_FX_DEBUG
            c->id = EMPTY_REF;
#endif
            fx++;
        }
    if (fx)
        plane_fx++;

    /* rows to write: holding something now or in VRAM */
    wr_n = 0;
    for (k = 0; k < WROWS; k++) {
        u8 pr = (top + k) & 63, nz = win_nz >> k & 1;
        u32 bit = 1UL << (pr & 31), *v = &vram_nz[pr >> 5];
        if (nz || (*v & bit)) {
            wr_row[wr_n] = pr; wr_win[wr_n] = k; wr_n++;
            if (nz) *v |= bit; else *v &= ~bit;
        }
    }
}

/* DMA must not cross a 128 KB boundary of the source */
static void dma_rom(u16 vram, const u32 *src, u16 words)
{
    u32 a = (u32)src, room = (0x20000 - (a & 0x1FFFF)) >> 1;
    if (words > room) {
        vdp_dma_vram(vram, src, room);
        vram += room * 2; src += room / 2; words -= room;
    }
    vdp_dma_vram(vram, src, words);
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
    for (i = 0; i < wr_n; i++)
        vdp_dma_vram(VRAM_PLANE_A + wr_row[i] * 64, win[wr_win[i]], 32);
    wr_n = 0;
    VDP_CTRL32 = VSRAM_W(0);
    VDP_DATA = vs & 511;
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL);
    VDP_DATA = hs;
}
