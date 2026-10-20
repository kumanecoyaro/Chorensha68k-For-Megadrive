/*
 * Background: the original scroll logic (fn_05574 / fn_056ae, run unchanged)
 * renders 32x32 "chips" into two 512x512 graphic pages, layer 0 (map.bg0,
 * near) over layer 1 (map.bg1, far), each with its own scroll speed.
 *
 * Here both layers share plane B (plane A is free for a large enemy, see
 * src/plane.c) and scroll together at the near layer's speed:
 *   * far chip draws are only remembered (far_sh, by page cell);
 *   * a near chip draw puts the near chip over a far chip into the plane:
 *     opaque near tiles as they are, empty ones show the far tile, mixed
 *     ones use a composite tile (tools/convert_assets.py) loaded on demand
 *     into a small reference-counted VRAM pool;
 *   * the far row under a near row is the one the original would show there
 *     at that moment, moved by a multiple of the far pattern's period so the
 *     far layer stays continuous from row to row (the far maps are mostly
 *     repeating patterns).
 */
#include "game.h"
#include "vdp.h"
#include "orig.h"

long ocall(void *fn, long a1, long a2, long a3, long a4);

#define COMP_BASE  BG_TILE_COUNT            /* VRAM tile of the first composite slot */
#define COMP_SLOTS 128
#define CHIPQ 32
#define UPQ   64

static s16 scroll_x, scroll_y[2];

/* page shadows: chip numbers per 32x32 cell (16 rows x 8 columns) */
static u8 near_sh[16][8], far_sh[16][8];
static u8 cell_far[16][8];                  /* far chip merged into each cell */
static u8 row_far[16];                      /* far page row under each near page row */
static s8 far_d;                            /* far row - near row */
static u8 last_p = 0xFF;

/* composite tile pool */
static u8 slot_of[BG_NCOMP];                /* composite -> slot + 1, 0 = not loaded */
static u16 slot_comp[COMP_SLOTS];
static u8 slot_ref[COMP_SLOTS];
static u8 slot_clock;
u16 comp_peak, comp_fail;                   /* (stats for tools/uctrace.py) */

/* chip writes and composite tile uploads, done by the VBlank interrupt */
static struct { u16 vram; u16 w[16]; } chipq[CHIPQ];
static volatile u8 chipq_n;
static struct { u16 vram; u16 comp; } upq[UPQ];
static volatile u8 upq_n;

/* fn_053ba: the original loads bg_pcg / map.chp / map.bg0-1 here.  The maps
 * are already in ROM (relocate.py maps B_8c252 there); set up the
 * map geometry the loader would have read from the file headers. */
long x_bg_load(void)
{
    int l;
    for (l = 0; l < 2; l++) {
        OW(0x90252 + 2 * l) = 8 - 1;        /* width mask */
        OW(0x90256 + 2 * l) = 0x400 - 1;    /* height mask */
        OW(0x9025a + 2 * l) = 3;            /* row shift (log2 width) */
    }
    for (l = 0; l < COMP_SLOTS; l++)
        slot_comp[l] = 0xFFFF;
    for (l = 0; l < 16 * 8; l++)
        (&cell_far[0][0])[l] = 0xFF;        /* (no far chip: nothing to release) */
    return 0;
}

static void irq_off(void) { __asm__ volatile("move.w #0x2700, %sr"); }
static void irq_on(void)  { __asm__ volatile("move.w #0x2500, %sr"); }

/* ------------------------------------------------------------ composites */
static void comp_release(u8 near, u8 far)
{
    u8 i, m, fx = bg_far_ix[far];
    u16 c;
    if (fx == 0xFF)
        return;
    for (i = 0; i < 16; i++) {
        m = bg_near_mix[near * 16 + i];
        if (m && (c = bg_comp[(m - 1) * BG_NFAR + fx]) != 0) {
            u8 s = slot_of[(c & 0x1FFF) - 1];
            if (s && slot_ref[s - 1])
                slot_ref[s - 1]--;
        }
    }
}

/* composite c (1-based) -> VRAM tile, 0 if the pool is full */
static u16 comp_get(u16 c)
{
    u8 s = slot_of[c - 1], n;
    if (s) {
        slot_ref[s - 1]++;
        return COMP_BASE + s - 1;
    }
    for (n = 0; n < COMP_SLOTS; n++) {
        s = slot_clock;
        if (++slot_clock >= COMP_SLOTS)
            slot_clock = 0;
        if (!slot_ref[s])
            break;
    }
    if (slot_ref[s] || upq_n >= UPQ) {
        comp_fail++;
        return 0;
    }
    if (slot_comp[s] != 0xFFFF)
        slot_of[slot_comp[s]] = 0;
    slot_comp[s] = c - 1;
    slot_of[c - 1] = s + 1;
    slot_ref[s] = 1;
    {
        u16 k, used = 0;
        for (k = 0; k < COMP_SLOTS; k++)
            used += slot_ref[k] != 0;
        if (used > comp_peak)
            comp_peak = used;
    }
    irq_off();
    upq[upq_n].vram = (COMP_BASE + s) * 32;
    upq[upq_n].comp = c - 1;
    upq_n++;
    irq_on();
    return COMP_BASE + s;
}

/* ------------------------------------------------------------ far row */
static u8 far_row_for(u8 p)
{
    u8 ft = (u8)(((p << 5) - scroll_y[0] + scroll_y[1]) >> 5) & 15;
    u8 fc = (p + far_d) & 15, per, k, f;
    for (per = 1; per <= 8; per <<= 1) {
        for (k = 0; k < 6; k++) {
            u8 a = (ft + k) & 15, b = (ft + k + per) & 15, c;
            for (c = 0; c < 8; c++)
                if (far_sh[a][c] != far_sh[b][c])
                    break;
            if (c < 8)
                break;
        }
        if (k == 6)
            break;
    }
    f = per <= 8 ? (ft + ((fc - ft) & (per - 1))) & 15 : ft;
    far_d = (s8)((f - p) & 15);
    return f;
}

/* near chip at page cell (p, c) over its far chip -> queued plane write
 * (the cell's previous composites must have been released) */
static void merge_cell(u8 p, u8 c)
{
    u8 n = near_sh[p][c], f = far_sh[row_far[p]][c], fx = bg_far_ix[f], i, m;
    const u16 *nw = chip_near + n * 16, *fw = chip_far + f * 16;
    u16 *w;
    while (chipq_n >= CHIPQ)                /* (full redraws: let VBlank drain it) */
        ;
    cell_far[p][c] = f;
    w = chipq[chipq_n].w;
    for (i = 0; i < 16; i++) {
        m = bg_near_mix[n * 16 + i];
        if (m) {
            u16 cw = fx == 0xFF ? 0 : bg_comp[(m - 1) * BG_NFAR + fx], t;
            w[i] = nw[i];
            if (cw && (t = comp_get(cw & 0x1FFF)) != 0)
                w[i] = (cw & 0xE000) | t;
        } else
            w[i] = nw[i] ? nw[i] : fw[i];
    }
    irq_off();
    chipq[chipq_n].vram = VRAM_PLANE_B + (p << 8) + (c << 3);
    chipq_n++;
    irq_on();
}

/* fn_301f6(pcg, chip_ptr, layer, x, y): chip_ptr = B_8824a + set * $2000 + chip * 32 */
long x_bg_chip(long pcg, long chip_ptr, long layer, long x, long y)
{
    u8 chip = (u8)((chip_ptr - OADDR(0x8824a)) >> 5);
    u8 p = ((u16)y >> 5) & 15, c = ((u16)x >> 5) & 7;
    (void)pcg;
    if (layer & 1) {
        far_sh[p][c] = chip;
        return 0;
    }
    comp_release(near_sh[p][c], cell_far[p][c]);
    near_sh[p][c] = chip;
    if (p != last_p) {                      /* a row's chips come one after another */
        last_p = p;
        row_far[p] = far_row_for(p);
    }
    merge_cell(p, c);
    return 0;
}

/* fn_05574 (full redraw of both pages, near layer first): then merge every
 * near cell again over the far layer now in place */
long orig_05574(void);
long x_bg_full(void)
{
    u8 p, c;
    ocall(orig_05574, 0, 0, 0, 0);
    far_d = 0;
    last_p = 0xFF;
    for (p = 0; p < 16; p++) {
        row_far[p] = far_row_for(p);
        for (c = 0; c < 8; c++) {
            comp_release(near_sh[p][c], cell_far[p][c]);
            merge_cell(p, c);
        }
    }
    return 0;
}

/* VBlank: composite tiles, then the queued chips */
void bg_chips(void)
{
    u8 i, r, n = upq_n;
    for (i = 0; i < n; i++) {
        const u32 *t = bg_comp_tiles + upq[i].comp * 8;
        VDP_CTRL32 = VRAM_W(upq[i].vram);
        for (r = 0; r < 8; r++)
            VDP_DATA32 = t[r];
    }
    upq_n = 0;
    n = chipq_n;
    for (i = 0; i < n; i++) {
        const u16 *w = chipq[i].w;
        u16 base = chipq[i].vram;
        for (r = 0; r < 4; r++) {
            VDP_CTRL32 = VRAM_W(base + (r << 6));
            VDP_DATA = w[0]; VDP_DATA = w[1]; VDP_DATA = w[2]; VDP_DATA = w[3];
            w += 4;
        }
    }
    chipq_n = 0;
}

/* scroll values handed to the XSP flush (graphic page origin in pixels) */
void bg_scroll(s16 x0, s16 x1, s16 y0, s16 y1)
{
    (void)x1;
    scroll_x = x0;
    scroll_y[0] = y0; scroll_y[1] = y1;
}

void bg_upload(void)
{
    VDP_CTRL32 = VSRAM_W(2);                /* plane B */
    VDP_DATA = scroll_y[0] & 511;
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL + 2);
    VDP_DATA = -scroll_x;
}
