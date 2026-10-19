/*
 * Background: the original scroll logic (fn_05574 / fn_056ae, run unchanged)
 * renders 32x32 "chips" into two 512x512 graphic pages.  Here the two pages
 * are plane A (layer 0, map.bg0, near) and plane B (layer 1, map.bg1, far),
 * 32x64 cells each; the chip drawing call (fn_301f6) writes the chip's 4x4
 * tiles into the plane, and the CRTC scroll values become VSRAM / HSCROLL.
 */
#include "game.h"
#include "vdp.h"
#include "orig.h"

static s16 scroll_x[2], scroll_y[2];
static const u16 *const chips_of[2] = { chip_near, chip_far };
static const u16 plane_of[2] = { VRAM_PLANE_A, VRAM_PLANE_B };

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
    return 0;
}

/* chip writes are queued and done by the VBlank interrupt (VDP owner) */
#define CHIPQ 32
static struct { const u16 *w; u16 vram; } chipq[CHIPQ];
static volatile u8 chipq_n;

static void chip_draw(const u16 *w, u16 base)
{
    int r;
    for (r = 0; r < 4; r++) {
        VDP_CTRL32 = VRAM_W(base + (r << 6));
        VDP_DATA = w[0]; VDP_DATA = w[1]; VDP_DATA = w[2]; VDP_DATA = w[3];
        w += 4;
    }
}

/* fn_301f6(pcg, chip_ptr, layer, x, y): chip_ptr = B_8824a + set * $2000 + chip * 32 */
long x_bg_chip(long pcg, long chip_ptr, long layer, long x, long y)
{
    u16 chip = (u16)((chip_ptr - OADDR(0x8824a)) >> 5) & 0xFF;
    const u16 *w = chips_of[layer & 1] + (chip << 4);
    u16 base = plane_of[layer & 1] + ((((u16)y >> 3) & 63) << 6) + ((((u16)x >> 3) & 31) << 1);
    (void)pcg;
    while (chipq_n >= CHIPQ)            /* (full screen redraws: let VBlank drain it) */
        ;
    __asm__ volatile("move.w #0x2700, %sr");
    chipq[chipq_n].w = w;
    chipq[chipq_n].vram = base;
    chipq_n++;
    __asm__ volatile("move.w #0x2500, %sr");
    return 0;
}

/* VBlank: queued chips (all of them: a full redraw is 128 chips) */
void bg_chips(void)
{
    u8 i, n = chipq_n;
    for (i = 0; i < n; i++)
        chip_draw(chipq[i].w, chipq[i].vram);
    chipq_n = 0;
}

/* scroll values handed to the XSP flush (graphic page origin in pixels) */
void bg_scroll(s16 x0, s16 x1, s16 y0, s16 y1)
{
    scroll_x[0] = x0; scroll_x[1] = x1;
    scroll_y[0] = y0; scroll_y[1] = y1;
}

void bg_upload(void)
{
    VDP_CTRL32 = VSRAM_W(0);
    VDP_DATA = scroll_y[0] & 511;
    VDP_DATA = scroll_y[1] & 511;
    VDP_CTRL32 = VRAM_W(VRAM_HSCROLL);
    VDP_DATA = -scroll_x[0];
    VDP_DATA = -scroll_x[1];
}
