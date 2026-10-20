/* Chorensha68k Mega Drive port: boot, frame hooks, top level (L03e60) */
#include "game.h"
#include "vdp.h"
#include "orig.h"
#include "spr.h"

long ocall(void *fn, long a1, long a2, long a3, long a4);
long x_bg_load(void);
long x_text_clear(void);
void plane_upload(void);
void bg_chips(void);

u16 frame_count;
static u16 hud_line[32];

/* text row 0 (score line) -> window plane */
static void hud_build(void)
{
    int i;
    for (i = 0; i < 32; i++) {
        u8 c = text_grid[0][i];
        hud_line[i] = c ? 0x8000 | (TEXT_PAL << 13) | (FONT_TILE_BASE + (c & 0x7F)) : 0;
    }
}

/* called by the vsync hook at the start of VBlank */
/* VBlank interrupt: show the finished frame (sprites, new tiles, BG chips,
 * scroll, score line), then the sound.  The game loop only hands over
 * complete frames (frame_ready), so a slow frame simply shows the previous
 * one again, and music / sound effects keep their own 60 Hz pace. */
volatile u8 frame_ready;
static u8 vbl_busy;

#ifdef FRAME_PROBE
u16 vprobe[64][4];
u16 stamp(void);
#endif
void vblank_work(void)
{
#ifdef FRAME_PROBE
    u16 *vp = vprobe[vblank_count & 63];
    vp[0] = stamp();
#endif
    if (vbl_busy)
        return;
    vbl_busy = 1;
    bg_chips();
    if (frame_ready) {
        xsp_upload();
        bg_upload();
        plane_upload();
        if (text_dirty) {
            hud_build();
            vdp_load_words(VRAM_WINDOW, hud_line, 32);
            text_dirty = 0;
        }
        frame_ready = 0;
    }
    input_poll();
#ifdef FRAME_PROBE
    vp[1] = stamp();
#endif
    bgm_service();
    music_update();
#ifdef FRAME_PROBE
    vp[2] = stamp();
#endif
    sound_update();
#ifdef FRAME_PROBE
    vp[3] = stamp();
#endif
    frame_count++;
    vbl_busy = 0;
}

/* fn_05224 hook: graphic palette registers -> BG palettes (static for now) */
void pal_from_orig(void)
{
}

int main(void)
{
    vdp_init();
    vdp_set_palettes(pal_data, 64);
    vdp_load_tiles(1, bg_tiles, BG_TILE_COUNT - 1);
    sound_init();
    xsp_init();
    vdp_display(1);
    __asm__ volatile("move.w #0x2500, %sr");

    x_bg_load();                            /* fn_053ba (start-up file loading) */

    /* L03e60: option defaults, then "game start" from the title menu */
    OW(0x4a106) = 0x10;                     /* enemy bullet speed */
    OW(0x4a108) = 0x30;                     /* player speed */
    OW(0x4a102) = 1;
    OW(0x4a10e) = 1;                        /* joystick */
    OW(0x4a0e0) = 1;
    OW(0x4a0e6) = 0x10;
    OW(0x4a0e8) = 0x30;
    for (;;) {
        x_text_clear();                     /* (the title screen would have cleared "GAME OVER") */
        OW(0x4a0f0) = OW(0x4a0f8);
        OW(0x4a0ee) = 1;                    /* stage 1 */
        OW(0x4a0f2) = OW(0x4a10a);
        OW(0x4a0f4) = OW(0x4a106);
        OW(0x4a0f6) = OW(0x4a108);
        OW(0x4a0a4) = 0;                    /* no replay */
#ifdef TEST_MUTEKI
        OW(0x4a104) = 1;                    /* original debug option: no death */
#endif
        OW(0x4a0ee) = (OW(0x4a0f0) << 5) | (OW(0x4a0ee) & 0x1F);
        ocall(OFN(0x038a2), 0, 0, 0, 0);    /* one game */
    }
    return 0;
}
