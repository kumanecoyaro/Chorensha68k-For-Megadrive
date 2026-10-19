/* Chorensha68k Mega Drive port - test build: stage 1 scroll, player, two enemy types */
#include "game.h"
#include "vdp.h"

static u16 score_line[32];

static void make_score_line(void)
{
    static const u32 pow10[8] = { 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };
    static const char label[] = "SCORE";
    const u16 attr = 0x8000 | (2 << 13);
    u32 v = score;
    int i;
    for (i = 0; i < 32; i++)
        score_line[i] = 0;
    /* font tile k = index into FONT_CHARS (' ' = 0, '0' = 1, 'A' = 11 ...) */
    for (i = 0; label[i]; i++)
        score_line[1 + i] = attr | (FONT_TILE_BASE + 11 + (label[i] - 'A'));
    for (i = 0; i < 8; i++) {
        u16 d = 0;
        while (v >= pow10[i]) {
            v -= pow10[i];
            d++;
        }
        score_line[8 + i] = attr | (FONT_TILE_BASE + 1 + d);
    }
}

int main(void)
{
    vdp_init();
    vdp_set_palettes(pal_data, 64);
    vdp_load_tiles(1, bg_tiles, BG_TILE_COUNT - 1);
    vdp_load_tiles(SPR_TILE_BASE, spr_tiles, SPR_TILE_COUNT);
    vdp_load_tiles(FONT_TILE_BASE, font_tiles, FONT_TILE_COUNT);

    sound_init();
    tables_init(0x10);              /* fn_05170(B_4a0f4 = $10) */
    obj_init();
    stage_init();
    player_init();
    bg_init(0, 0x10);               /* fn_05906((stage-1)*8, $10) for stage 1 */

    spr_begin();
    spr_end();
    make_score_line();
    vdp_display(1);
    __asm__ volatile("move.w #0x2500, %sr");

    for (;;) {
        vdp_wait_vblank();
        spr_upload();
        bg_upload();
        vdp_load_words(VRAM_WINDOW, score_line, 32);

        pad_prev = pad_now;
        pad_now = pad_read();

        spr_begin();
        stage_update();
        player_update();
        shots_update();
        obj_update_all();
        bg_update();
        spr_end();
        music_update();
        sound_update();
        make_score_line();
        frame_count++;
    }
    return 0;
}
