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
void plane_act(void);
void sound_dma(u8 on);
void bg_chips(void);

u16 frame_count;
/* the score panel (window plane, screen cells 32..39 on the right, H40):
 * the game's text rows 0..3 (the score line, the bombs, the ships) laid out
 * in 8 columns, in opaque glyphs (the panel covers the sprites running off
 * the play area: they are low priority) put in the unused part of the window
 * table (src/vdp.h) */
static const char hud_chars[] = " 0123456789SCOREHIBMP*\x04\x06\x07";
#define HUD_ATTR (0x8000 | (TEXT_PAL << 13))

static u16 hud_tile(u8 i)               /* glyph i: +$20 / +$60 of window row i / 2 */
{
    return (VRAM_WINDOW + (i >> 1) * 128 + ((i & 1) ? 0x60 : 0x20)) / 32;
}

static u16 hud_cell(u8 c)
{
    u8 i;
    if ((u8)(c - '0') < 10)
        return HUD_ATTR | hud_tile(c - '0' + 1);
    for (i = 11; hud_chars[i] && (u8)hud_chars[i] != c; i++)
        ;
    return HUD_ATTR | hud_tile(hud_chars[i] ? i : 0);
}

static void hud_row(u8 r, const u8 *s, u8 n)    /* panel row r: n characters (the rest blank) */
{
    u8 i;
    VDP_CTRL32 = VRAM_W(VRAM_WINDOW + r * 128 + 64);
    for (i = 0; i < 8; i++)
        VDP_DATA = hud_cell(i < n ? s[i] : ' ');
}

static void hud_init(void)
{
    u8 i, r, k;
    for (i = 0; hud_chars[i]; i++) {    /* the glyphs, background colour 2 (black) */
        const u32 *t = &text_font[(u8)hud_chars[i] * 8];
        VDP_CTRL32 = VRAM_W(hud_tile(i) * 32);
        for (r = 0; r < 8; r++) {
            u32 v = t[r], o = 0;
            for (k = 0; k < 8; k++) {
                u32 p = (v >> (k * 4)) & 15;
                o |= (p ? p : 2) << (k * 4);
            }
            VDP_DATA32 = o;
        }
    }
    for (r = 0; r < 28; r++)
        hud_row(r, (const u8 *)"", 0);
}

/* the text rows the panel shows in a game (its score line in row 0): 0..3,
 * the text sprites / plane A text from row text_row0() on */
u8 text_row0(void)
{
    return text_grid[0][1] == 'S' && text_grid[0][2] == 'C' ? 4 : 0;
}

/* HI SCORE / SCORE on top, the bombs (5 at most) below them, the ships at
 * the bottom (2 rows each, 8 to a row pair, more pairs upwards: the game
 * gives 10 and more) */
static void hud_build(void)
{
    static const u8 score[] = "SCORE", hi[] = "HI SCORE";
    u8 game = text_row0() != 0, n = 0, r, row[8], i;
    hud_row(1, hi, game ? 8 : 0);
    hud_row(2, &text_grid[0][23], game ? 8 : 0);
    hud_row(4, score, game ? 5 : 0);
    hud_row(5, &text_grid[0][4], game ? 8 : 0);
    if (game)
        while (n < 31 && text_grid[2][1 + n] == 0x06)
            n++;
#ifdef HUD_TEST
    n = HUD_TEST;
#endif
    for (r = 0; r < 9; r++) {
        u8 k = n > r * 8 ? n - r * 8 : 0;
        if (k > 8)
            k = 8;
        hud_row(26 - r * 2, (const u8 *)"\x06\x06\x06\x06\x06\x06\x06\x06", k);
        hud_row(27 - r * 2, (const u8 *)"\x07\x07\x07\x07\x07\x07\x07\x07", k);
    }
    for (i = 0; i < 8; i++)
        row[i] = game && i < 5 && text_grid[1][1 + i] == 0x04 ? 0x04 : ' ';
    if (OW(0x4a104))                    /* invincible (the title's up + START) */
        row[7] = '*';
    hud_row(7, row, 8);
}

/* called by the vsync hook at the start of VBlank */
/* VBlank interrupt: show the finished frame (sprites, new tiles, BG chips,
 * scroll, score line), then the sound.  The game loop only hands over
 * complete frames (frame_ready), so a slow frame simply shows the previous
 * one again, and music / sound effects keep their own 60 Hz pace. */
volatile u8 frame_ready;
void xsp_lag(void);
void plane_vblank(void);
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
    /* first the scrolls and the H-int of the frame shown (while still in the
     * VBlank: the H-int counter takes register 10 only there; set later, after
     * the DMA ran into the display, the first H-int came anywhere and the
     * band switches of src/plane.c went late), then the DMA */
    if (frame_ready) {
        sound_dma(1);
        xsp_upload();
#ifdef PLANE_ONLY
        VDP_CTRL32 = VRAM_W(VRAM_SPRITES);
        VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0;
#endif
        sound_dma(0);
        plane_act();
    } else
        xsp_lag();
    plane_vblank();                     /* plane A scroll, the H-int for the frame shown */
    bg_chips();
    if (frame_ready) {
        sound_dma(1);
        bg_upload();
        plane_upload();
        sound_dma(0);
        if (text_dirty) {
            hud_build();
            text_dirty = 0;
        }
        frame_ready = 0;
    }
    /* the VDP work is done: the H-int may come in during the rest (the
     * sound, which can run into the display: level 4 let in, else the band
     * switches of src/plane.c came late and the rows below shook) */
    __asm__ volatile("move.w #0x2300, %sr");
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
    hud_init();
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
