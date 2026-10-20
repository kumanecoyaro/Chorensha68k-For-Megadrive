#ifndef GAME_H
#define GAME_H
/*
 * Chorensha68k -> Mega Drive port.
 *
 * The game logic is the original SZ2.X 68000 code, relocated into ROM/RAM by
 * tools/relocate.py.  The C side replaces the X68000 libraries it calls:
 * XSP sprites (xsp.c), graphic-VRAM BG (bg.c), text VRAM, input, palettes
 * (x68.c), MCDRV/ADPCM sound (sound.c, music.c).
 *
 * Coordinates in the original are 16-bit, 1/64 pixel, XSP space: the screen
 * pixel of a 16x16 sprite's top-left is (v >> 6) - 16.  The X68000 screen is
 * 256x256; the Mega Drive shows the top 224 lines.
 */
#include "types.h"
#include "assets.h"

extern u16 frame_count;
extern u16 pad_now, pad_prev;

/* ---- x68.c */
void input_poll(void);
extern u8 text_grid[32][32];
extern u8 text_row_n[32];
extern u32 text_row_mask[32];
extern u8 text_dirty;
extern u8 text_spr_dirty;      /* a text row shown by sprites changed (x68.c) */

/* ---- main.c */
void pal_from_orig(void);
void bgm_service(void);

/* ---- xsp.c (sprites) */
void xsp_init(void);
void xsp_flush(void);           /* end of frame: build the hardware list */
void xsp_upload(void);          /* VBlank: DMA tiles + sprite table */

/* ---- bg.c */
void bg_scroll(s16 x0, s16 x1, s16 y0, s16 y1);
void bg_upload(void);

/* ---- sound.c / music.c */
void sound_init(void);
void sound_request(u8 no);
void sound_update(void);
void music_play(u8 no);
void music_stop(void);
void music_update(void);

#endif
