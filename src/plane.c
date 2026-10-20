/*
 * Large enemies on plane A (the BG uses plane B only, src/bg.c).
 *
 * The stage 1 boss (obj204) and the crab (obj202, two halves and a centre
 * drawn as three refs) are pre-composited by tools/convert_sprites.py into
 * tile images (the crab's halves open 0..4 px: images at 0 and 4 px).  At
 * the end of each frame the recorded XSP calls are searched for one of them;
 * when its tiles are in VRAM its calls are dropped from the sprite list and
 * plane A shows the image at the same place instead (one enemy at a time:
 * the boss first, else the crab nearest to the one already shown).
 *
 *   * hit flash (another palette): the plane stays, the flashing refs are
 *     drawn as sprites over it;
 *   * plane A is as wide as the screen, so a column cut by the left / right
 *     edge would wrap around: such columns are left empty;
 *   * the tiles of an image group (crab / boss: never on screen together)
 *     are loaded into VRAM over a few VBlanks the first time it appears,
 *     sprites until then.
 */
#include "game.h"
#include "vdp.h"
#include "spr.h"

#define ENEMY_BASE 1364                 /* VRAM tiles 1364..1523 (xsp.c cache ends at 1364) */
#define EMPTY_REF  0x29                 /* a ref without pieces: a dropped call */
#define REF_CRAB   0xB1E                /* obj202: left half, right half, centre */
#define REF_BOSS   0xB29                /* obj204 */
#define LOAD_CHUNK 64                   /* tiles per VBlank */

typedef struct { s16 x, y; u16 id, info; } Call;
extern Call calls[];
extern u16 ncalls;

static u8 loaded = 0xFF, loading = 0xFF;
static u16 load_pos;
static s8 shown = -1;                   /* image in the plane map, -1 = none */
static s16 ax, ay;                      /* anchor of the shown image (screen pixels) */
static u32 shown_cols;                  /* image columns written (not cut by an edge) */
static u8 map_rows;                     /* plane rows to (re)write */
static u16 mapbuf[16 * 32];
static volatile u8 map_dirty;
static s16 hs, vs;
u16 plane_frames;                       /* (stats for tools/uctrace.py) */

static u8 pal_of(const Call *c) { return (c->info >> 8) & 15; }

/* image columns fully on screen */
static u32 visible_cols(const PlaneImg *im, s16 x)
{
    u32 m = 0;
    u8 i;
    s16 sx = x + im->ox;
    for (i = 0; i < im->w; i++, sx += 8)
        if (sx >= 0 && sx <= 256 - 8)
            m |= 1UL << i;
    return m;
}

static void build_map(s8 img, u32 cols)
{
    u8 rows = map_rows, r, c;
    u16 *d = mapbuf;
    for (r = 0; r < 16; r++)
        for (c = 0; c < 32; c++)
            d[r * 32 + c] = 0;
    if (img >= 0) {
        const PlaneImg *im = &plane_img[img];
        const u16 *m = plane_map + im->map;
        u16 attr = ((u16)im->pal << 13) + ENEMY_BASE - 1;
        for (r = 0; r < im->h; r++)
            for (c = 0; c < im->w; c++, m++)
                if (*m && (cols >> c & 1))
                    d[r * 32 + c] = attr + *m;
        if (im->h > rows)
            rows = im->h;
    }
    map_rows = rows;
    map_dirty = 1;
}

void plane_frame(void)
{
    s16 best = -1, bd = 0x7FFF;
    s8 img = -1;
    u16 i;
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
    if (img >= 0) {
        const PlaneImg *im = &plane_img[img];
        u32 cols;
        c = &calls[best];
        ax = c->x >> 6; ay = c->y >> 6;
        cols = visible_cols(im, ax);
        if (img != shown || cols != shown_cols)
            build_map(img, cols);
        shown = img; shown_cols = cols;
        plane_frames++;
        hs = ax + im->ox;
        vs = -(ay + im->oy);
        /* drop the calls shown by the plane (a flashing part stays a sprite, over it) */
        if (img == 2) {
            if (pal_of(c) == 7)
                c->id = EMPTY_REF;
        } else {
            if (pal_of(c) == 5) c->id = EMPTY_REF;
            if (pal_of(c - 1) == 6) c[-1].id = EMPTY_REF;
            if (pal_of(c - 2) == 6) c[-2].id = EMPTY_REF;
        }
    } else if (shown >= 0) {
        build_map(-1, 0);
        shown = -1;
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

/* VBlank: tile loading, plane map, scroll */
void plane_upload(void)
{
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
    if (map_dirty) {
        vdp_dma_vram(VRAM_PLANE_A, mapbuf, map_rows * 32);
        map_dirty = 0;
    }
    VDP_CTRL32 = VSRAM_W(0);
    VDP_DATA = vs & 511;
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL);
    VDP_DATA = hs;
}
