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
 *
 * Each converted stage has its own BG set (tiles, palettes 0 / 1, chip and
 * composite tables: tools/convert_assets.py).  The map scrolls on from one
 * stage into the next; the next stage's set (which also covers the rows of
 * the previous stage still on screen) is switched in while its palette fades
 * in (bg_stage_check), with the display off for the tile upload, and every
 * cell is merged again.
 */
#include "game.h"
#include "vdp.h"
#include "orig.h"

long ocall(void *fn, long a1, long a2, long a3, long a4);

/* composite slots live in VRAM the H32 layout leaves unused (1 cell row of
 * the window plane, 64 sprites of the 80-entry table, a full-screen scroll
 * table): $D040-$D7FF, $DA00-$DBFF, $DC20-$DE5F (peak use in stage 1: 78) */
#define COMP_SLOTS 96
static u16 slot_tile(u8 s)
{
    return s < 62 ? 0xD040 / 32 + s : s < 78 ? 0xDA00 / 32 + s - 62 : 0xDC20 / 32 + s - 78;
}
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
void xsp_cache_layout(u16 bg_end);
static const BgSet *bs = &bg_sets[0];
static u8 bs_stage = 1;                     /* stage of the set in use */
static volatile u8 bs_pending;              /* VBlank: upload the set's tiles and palettes */
/* composite -> slot: open addressing (a stage has up to ~1100 composites,
 * the pool holds COMP_SLOTS of them) */
#define SOF_SIZE 128
static u16 sof_key[SOF_SIZE];               /* composite + 1, 0 = free */
static u8 sof_slot[SOF_SIZE];
static u16 slot_comp[COMP_SLOTS];
static u8 slot_ref[COMP_SLOTS];
static u8 comp_free = COMP_SLOTS;           /* slots with no reference (the pool full: 0) */
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
    xsp_cache_layout(1 + bs->ntiles);
    return 0;
}

static void irq_off(void) { __asm__ volatile("move.w #0x2700, %sr"); }
static void irq_on(void)  { __asm__ volatile("move.w #0x2300, %sr"); }

/* ------------------------------------------------------------ composites */
static inline u8 sof_home(u16 c) { return (c ^ (c >> 7)) & (SOF_SIZE - 1); }

/* composite c (0-based) -> slot + 1, 0 = not loaded */
static u8 slot_of(u16 c)
{
    u8 h = sof_home(c);
    c++;
    while (sof_key[h]) {
        if (sof_key[h] == c)
            return sof_slot[h];
        h = (h + 1) & (SOF_SIZE - 1);
    }
    return 0;
}

static void sof_put(u16 c, u8 s1)
{
    u8 h = sof_home(c);
    while (sof_key[h])
        h = (h + 1) & (SOF_SIZE - 1);
    sof_key[h] = c + 1;
    sof_slot[h] = s1;
}

/* (backward-shift deletion, as src/xsp.c hash_remove) */
static void sof_del(u16 c)
{
    u8 h = sof_home(c), j, k;
    while (sof_key[h] != c + 1) {
        if (!sof_key[h])
            return;
        h = (h + 1) & (SOF_SIZE - 1);
    }
    for (j = h;;) {
        j = (j + 1) & (SOF_SIZE - 1);
        if (!sof_key[j])
            break;
        k = sof_home(sof_key[j] - 1);
        if (h <= j ? (k > h && k <= j) : (k > h || k <= j))
            continue;
        sof_key[h] = sof_key[j];
        sof_slot[h] = sof_slot[j];
        h = j;
    }
    sof_key[h] = 0;
}

/* cells merged without some composite (the pool full: the far tile shown
 * there instead), a bit per column; merged again when a slot comes free
 * (bg_retry) */
static u8 retry_mask[16], comp_freed;

static void comp_release(u8 near, u8 far)
{
    u8 i, m, fx = bs->far_ix[far];
    u16 c;
    if (fx == 0xFF)
        return;
    for (i = 0; i < 16; i++) {
        m = bs->near_mix[near * 16 + i];
        if (m && (c = bs->comp[mulu(m - 1, bs->nfar) + fx]) != 0) {
            u8 s = slot_of((c & 0x1FFF) - 1);
            if (s && slot_ref[s - 1] && !--slot_ref[s - 1]) {
                comp_freed = 1;
                comp_free++;
            }
        }
    }
}

/* composite c (1-based) -> VRAM tile, 0 if the pool is full */
static u16 comp_get(u16 c)
{
    u8 s = slot_of(c - 1), n;
    if (s) {
        if (!slot_ref[s - 1]++)
            comp_free--;
        return slot_tile(s - 1);
    }
    if (!comp_free || upq_n >= UPQ) {       /* (no scan of a full pool) */
        comp_fail++;
        return 0;
    }
    for (n = 0; n < COMP_SLOTS; n++) {
        s = slot_clock;
        if (++slot_clock >= COMP_SLOTS)
            slot_clock = 0;
        if (!slot_ref[s])
            break;
    }
    if (slot_comp[s] != 0xFFFF)
        sof_del(slot_comp[s]);
    slot_comp[s] = c - 1;
    sof_put(c - 1, s + 1);
    slot_ref[s] = 1;
    comp_free--;
    if (COMP_SLOTS - comp_free > comp_peak)
        comp_peak = COMP_SLOTS - comp_free;
    irq_off();
    upq[upq_n].vram = slot_tile(s) * 32;
    upq[upq_n].comp = c - 1;
    upq_n++;
    irq_on();
    return slot_tile(s);
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
    u8 n = near_sh[p][c], f = far_sh[row_far[p]][c], fx = bs->far_ix[f], i, m;
    const u16 *nw = bs->chip_near + n * 16, *fw = bs->chip_far + f * 16;
    u16 *w;
    u8 miss = 0;
    while (chipq_n >= CHIPQ)                /* (full redraws: let VBlank drain it) */
        ;
    cell_far[p][c] = f;
    w = chipq[chipq_n].w;
    for (i = 0; i < 16; i++) {
        m = bs->near_mix[n * 16 + i];
        if (m) {
            u16 cw = fx == 0xFF ? 0 : bs->comp[mulu(m - 1, bs->nfar) + fx], t;
            w[i] = nw[i];
            if (cw) {
                if ((t = comp_get(cw & 0x1FFF)) != 0)
                    w[i] = (cw & 0xE000) | t;
                else {
                    w[i] = fw[i];           /* (pool full: the far tile, not a hole) */
                    miss = 1;
                }
            }
        } else
            w[i] = nw[i] ? nw[i] : fw[i];
    }
    if (miss)
        retry_mask[p] |= 1 << c;
    else
        retry_mask[p] &= ~(1 << c);
    irq_off();
    chipq[chipq_n].vram = VRAM_PLANE_B + (p << 8) + (c << 3);
    chipq_n++;
    irq_on();
}

#ifdef TEST_BGLOG
u8 bglog[4][2][32];                         /* chips drawn per stage & 3, layer */
#endif
/* fn_301f6(pcg, chip_ptr, layer, x, y): chip_ptr = B_8824a + set * $2000 + chip * 32 */
long x_bg_chip(long pcg, long chip_ptr, long layer, long x, long y)
{
    u8 chip = (u8)((chip_ptr - OADDR(0x8824a)) >> 5);
    u8 p = ((u16)y >> 5) & 15, c = ((u16)x >> 5) & 7;
    (void)pcg;
#ifdef TEST_BGLOG
    {
        u8 st = OW(0x4a0ee) & 7;
        bglog[st & 3][layer & 1][chip >> 3] |= 1 << (chip & 7);
    }
#endif
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

/* ------------------------------------------------------------ BG sets */
/* the current stage (B_4a0ee: 1..) -> its set (the last one past the converted stages) */
static u8 cur_stage(void)
{
    u8 st = OW(0x4a0ee) & 7;
    return st < 1 ? 1 : st > BG_NSETS ? BG_NSETS : st;
}

/* make stage st's set the current one: the composite pool is emptied, the
 * tiles and palettes go up in the next VBlank (before any chip write) */
static void set_select(u8 st)
{
    u16 k;
    irq_off();
    bs = &bg_sets[st - 1];
    bs_stage = st;
    chipq_n = 0;
    upq_n = 0;
    bs_pending = 1;
    irq_on();
    for (k = 0; k < COMP_SLOTS; k++) {
        slot_comp[k] = 0xFFFF;
        slot_ref[k] = 0;
    }
    comp_free = COMP_SLOTS;
    for (k = 0; k < SOF_SIZE; k++)
        sof_key[k] = 0;
    for (k = 0; k < 16 * 8; k++)
        (&cell_far[0][0])[k] = 0xFF;        /* (nothing to release) */
    for (k = 0; k < 16; k++)
        retry_mask[k] = 0;
    xsp_cache_layout(1 + bs->ntiles);       /* the sprite cache follows the set's tiles */
}

/* every near cell merged again over the far layer in place */
static void merge_all(void)
{
    u8 p, c;
    far_d = 0;
    last_p = 0xFF;
    for (p = 0; p < 16; p++) {
        row_far[p] = far_row_for(p);
        for (c = 0; c < 8; c++) {
            comp_release(near_sh[p][c], cell_far[p][c]);
            merge_cell(p, c);
        }
    }
}

/* fn_05574 (full redraw of both pages, near layer first, at a game start):
 * with the current stage's set, then merge every near cell again over the
 * far layer now in place */
long orig_05574(void);
extern u8 start_stage;                      /* (src/title.c) */
long x_bg_full(void)
{
    u8 st;
    if (start_stage > 1 && (OW(0x4a0ee) & 7) == 1) {  /* the title's stage select */
        OW(0x4a0ee) += start_stage - 1;
        ocall(OFN(0x05906), (start_stage - 1) * 8, 0x10, 0, 0);
    }
    start_stage = 0;
    st = cur_stage();
    if (st != bs_stage)
        set_select(st);
    ocall(orig_05574, 0, 0, 0, 0);
    merge_all();
    return 0;
}

/* once a frame (x_vsync): a cell merged without some composite is merged
 * again once a pool slot has come free (one cell a frame) */
void bg_retry(void)
{
    static u8 rp;
    u8 k, c;
    if (!comp_freed)
        return;
    for (k = 0; k < 16; k++, rp = (rp + 1) & 15)
        if (retry_mask[rp]) {
            for (c = 0; !(retry_mask[rp] & (1 << c)); c++)
                ;
            comp_freed = 0;
            comp_release(near_sh[rp][c], cell_far[rp][c]);
            merge_cell(rp, c);
            return;
        }
    comp_freed = 0;
}

/* once a frame (x_vsync): a new stage switches its set in while its palette
 * fades in (stage script state $100, timer $40 of the $80-frame fade
 * fn_0479a, the screen then shows the end of the previous stage's map
 * and the start of the new one).  Back to an earlier stage (after the last
 * converted one, tools/relocate.py): the map restarts at the stage's first
 * segment, as at a game start (fn_05906, full redraw). */
void bg_stage_check(void)
{
    u8 st = cur_stage();
    if (st == bs_stage || OW(0x59328) != 0x100)
        return;
    if (st < bs_stage) {
        ocall(OFN(0x05906), (st - 1) * 8, 0x10, 0, 0);
        x_bg_full();
    } else if (OW(0x59326) >= 0x40) {
        set_select(st);
        merge_all();
    }
}

/* the original's graphic screen on / off (IOCS $B2, x_screen_clear): off
 * while a stage is being drawn, on when it is complete.  Off, plane B shows
 * plane A's name table instead, scrolled as plane A (plane_upload): only
 * plane A's content (text, nothing else then) shows. */
volatile u8 bg_on;                          /* (plane.c: plane B follows plane A while off) */
void bg_show(u8 on)
{
    bg_on = on;
}

/* x_screen_clear: drop the queued writes (the planes are being cleared) */
void bg_reset(void)
{
    irq_off();
    chipq_n = 0;
    upq_n = 0;
    irq_on();
}

void dma_rom(u16 vram, const u32 *src, u16 words);
void sound_dma(u8 on);

/* VBlank: a new set's tiles and palettes (display off: longer than the
 * VBlank), composite tiles, then the queued chips */
void bg_chips(void)
{
    u8 i, r, n;
    if (bs_pending) {
        vdp_display(0);
        sound_dma(1);
        dma_rom(32, bs->tiles, bs->ntiles * 16);
        sound_dma(0);
        vdp_set_palettes(bs->pal, 32);
        vdp_display(1);
        bs_pending = 0;
    }
    n = upq_n;
    for (i = 0; i < n; i++) {
        const u32 *t = bs->comp_tiles + upq[i].comp * 8;
        VDP_CTRL32 = VRAM_W(upq[i].vram);
        for (r = 0; r < 8; r++)
            VDP_DATA32 = t[r];
    }
    upq_n = 0;
    n = chipq_n;
#ifdef PLANE_ONLY
    n = 0;
#endif
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
    VDP_CTRL = 0x8400 | (bg_on ? VRAM_PLANE_B >> 13 : VRAM_PLANE_A >> 13);
    VDP_CTRL32 = VSRAM_W(2);                /* plane B */
    VDP_DATA = scroll_y[0] & 511;
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL + 2);
    VDP_DATA = -scroll_x;
}
