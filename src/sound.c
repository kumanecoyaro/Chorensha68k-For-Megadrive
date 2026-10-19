/*
 * PCM sound effects (replaces MCDRV ADPCM playback).
 *
 * Samples are stored in ROM already converted to 8-bit unsigned 15.625 kHz
 * (tools/convert_pcm.py) and streamed by a small Z80 driver straight to the
 * YM2612 DAC (tools/z80_pcm_driver.py).  sound_request() mirrors fn_034ae:
 * one request per frame, a higher-or-equal priority (PR= in SZ2_PCM.CNF)
 * request replaces the pending one.
 */
#include "game.h"
#include "pcm.h"

#define Z80_RAM    ((volatile u8 *)0xA00000)
#define Z80_BUSREQ (*(volatile u16 *)0xA11100)
#define Z80_RESET  (*(volatile u16 *)0xA11200)

extern const u8 z80_driver[];
extern const u16 z80_driver_size;

static u8 req_no;
static u16 play_pr, play_frames;

static void z80_get_bus(void)
{
    Z80_BUSREQ = 0x100;
    while (Z80_BUSREQ & 0x100)
        ;
}

static void z80_release_bus(void)
{
    Z80_BUSREQ = 0;
}

void sound_init(void)
{
    u16 i;
    /* Z80 RAM is only reachable with the bus granted and reset released */
    Z80_RESET = 0x100;
    z80_get_bus();
    for (i = 0; i < z80_driver_size; i++)
        Z80_RAM[i] = z80_driver[i];
    for (i = 0x1FF0; i < 0x2000; i++)
        Z80_RAM[i] = 0;
    Z80_RESET = 0;                  /* reset pulse, then run from $0000 */
    for (i = 0; i < 32; i++)
        __asm__ volatile("nop");
    Z80_RESET = 0x100;
    z80_release_bus();
    req_no = 0;
    play_frames = 0;
}

/* fn_034ae */
void sound_request(u8 no)
{
    if (!pcm_table[no].length)
        return;
    if (req_no && pcm_table[no].priority < pcm_table[req_no].priority)
        return;
    req_no = no;
}

/* once per frame: hand the pending request to the Z80 */
void sound_update(void)
{
    const PcmEntry *e;
    u32 a;
    if (play_frames)
        play_frames--;
    if (!req_no)
        return;
    e = &pcm_table[req_no];
    req_no = 0;
    if (play_frames && e->priority < play_pr)
        return;
    a = (u32)pcm_data + e->offset;
    z80_get_bus();
    Z80_RAM[0x1FF1] = (u8)a;
    Z80_RAM[0x1FF2] = (u8)((a >> 8) & 0x7F) | 0x80;
    Z80_RAM[0x1FF3] = (u8)(a >> 15);
    Z80_RAM[0x1FF4] = (u8)(a >> 23);
    Z80_RAM[0x1FF5] = (u8)e->length;
    Z80_RAM[0x1FF6] = (u8)(e->length >> 8);
    Z80_RAM[0x1FF7] = (u8)(e->length >> 16);
    Z80_RAM[0x1FF0] = 1;
    z80_release_bus();
    play_pr = e->priority;
    play_frames = (u16)(e->length >> 8);       /* ~260 samples per frame */
}
