/* Chorensha68k Mega Drive port: boot, frame hooks, top level (L03e60) */
#include "game.h"
#include "vdp.h"
#include "orig.h"
#include "spr.h"

/* the original's RAM must end below the port's (src/md.ld: $FF9DC0) */
_Static_assert(ORIG_RAM + ORIG_RAM_SIZE <= 0xFF9DC0, "original RAM overlaps the port's");

long ocall(void *fn, long a1, long a2, long a3, long a4);
void text_map_set(u8 kind);
long x_bg_load(void);
long x_text_clear(void);
void plane_upload(void);
void sound_dma(u8 on);
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
u16 vprobe[32][4];
u16 stamp(void);
#endif
void vblank_work(void)
{
#ifdef FRAME_PROBE
    u16 *vp = vprobe[vblank_count & 31];
    vp[0] = stamp();
#endif
    if (vbl_busy)
        return;
    vbl_busy = 1;
    bg_chips();
    if (frame_ready) {
        sound_dma(1);
        xsp_upload();
#ifdef PLANE_ONLY
        VDP_CTRL32 = VRAM_W(VRAM_SPRITES);
        VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0;
#endif
        bg_upload();
        plane_upload();
        sound_dma(0);
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
#ifndef NO_MUSIC
    bgm_service();
    music_update();
#endif
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
    vdp_load_tiles(1, bg_sets[0].tiles, bg_sets[0].ntiles);
    sound_init();
    xsp_init();
    text_map_set(0);
    vdp_display(1);
    __asm__ volatile("move.w #0x2300, %sr");      /* (H-int: the sprite doubler) */

    x_bg_load();                            /* fn_053ba (start-up file loading) */

    /* L03e60: option defaults, the title menu (fn_02dac) and the games it
     * starts (patched by tools/relocate.py: "exit" and the idle replay demo
     * stay on the title), forever */
    ocall(OFN(0x03e60), 0, 0, 0, 0);
    return 0;
}
