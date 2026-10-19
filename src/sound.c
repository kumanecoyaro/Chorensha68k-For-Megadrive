/*
 * Sound: PCM sound effects + YM2612 write queue (replaces MCDRV).
 *
 * Samples are stored in ROM already converted to 8-bit unsigned 15.625 kHz
 * (tools/convert_pcm.py) and streamed by the Z80 straight to the YM2612 DAC
 * (tools/z80_pcm_driver.py).  The Z80 also performs every YM2612 register
 * write: ym_write() queues them and sound_update() copies the queue into the
 * Z80 ring buffer once per frame.  FM channel 6 is lent to the DAC only while
 * a sound effect plays.
 *
 * sound_request() mirrors fn_034ae: one request per frame, a request with
 * higher-or-equal priority (PR= in SZ2_PCM.CNF) replaces the pending one.
 */
#include "game.h"
#include "pcm.h"

#define Z80_RAM    ((volatile u8 *)0xA00000)
#define Z80_BUSREQ (*(volatile u16 *)0xA11100)
#define Z80_RESET  (*(volatile u16 *)0xA11200)
#define RING       0x1D00
#define RING_RD    0x1FFC
#define RING_WR    0x1FFD

extern const u8 z80_driver[];
extern const u16 z80_driver_size;

#ifdef Z80_PEEK
u8 z80_peek[64][4];
#endif
static u8 req_no;
static u16 play_pr, play_frames;

#define QMAX 600
static u8 queue[QMAX];          /* pending 3-byte writes: part offset, reg, value */
static u16 qlen;
static u8 ym_shadow[2][256];
static u8 dac_on;
static u8 music_pan6 = 0xC0;   /* FM6 pan wanted by the music ($B6, shared with the DAC) */

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

#ifdef YM_LOG
u16 ym_log_n;
u8 ym_log[6000 * 4];
#endif

static void ym_put(u8 part, u8 reg, u8 val)
{
#ifdef YM_LOG
#ifndef YM_LOG_FROM
#define YM_LOG_FROM 0
#endif
    if (ym_log_n < 6000 && frame_count >= YM_LOG_FROM) {
        u8 *l = &ym_log[ym_log_n++ * 4];
        l[0] = (u8)frame_count; l[1] = part; l[2] = reg; l[3] = val;
    }
#endif
    if (qlen + 3 > QMAX)
        return;
    queue[qlen++] = part << 1;
    queue[qlen++] = reg;
    queue[qlen++] = val;
}

/* part 0/1.  Key-on ($28) and frequency ($A0-$AF) writes are never
 * filtered: $A4-$A6 go to a latch shared by all channels that is only
 * applied by the following $A0-$A2 write, so they must always come as a pair. */
void ym_write(u8 part, u8 reg, u8 val)
{
    if (part == 1 && reg == 0xB6) {             /* the DAC uses FM6's pan */
        music_pan6 = val;
        if (dac_on)
            return;
    }
    if (reg != 0x28 && (reg & 0xF0) != 0xA0 && ym_shadow[part][reg] == val)
        return;
    ym_shadow[part][reg] = val;
    ym_put(part, reg, val);
}

void psg_write(u8 v)
{
    *(volatile u8 *)0xC00011 = v;
}

void sound_init(void)
{
    u16 i;
    /* Z80 RAM is only reachable with the bus granted and reset released */
    Z80_RESET = 0x100;
    z80_get_bus();
    for (i = 0; i < z80_driver_size; i++)
        Z80_RAM[i] = z80_driver[i];
    for (i = 0x1D00; i < 0x2000; i++)
        Z80_RAM[i] = 0;
    Z80_RESET = 0;                  /* reset pulse, then run from $0000 */
    for (i = 0; i < 32; i++)
        __asm__ volatile("nop");
    Z80_RESET = 0x100;
    z80_release_bus();
    for (i = 0; i < 256; i++)
        ym_shadow[0][i] = ym_shadow[1][i] = 0xFF;
    qlen = 0;
    dac_on = 0;
    req_no = 0;
    play_frames = 0;
    ym_put(0, 0x2B, 0x00);          /* DAC off: FM6 belongs to the music */
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

/* once per frame: start a pending effect, flush queued YM2612 writes */
void sound_update(void)
{
    const PcmEntry *e = NULL;
    u16 i, n;
    u8 rd, wr, room;

    if (play_frames && --play_frames == 0) {
        ym_put(0, 0x2B, 0x00);      /* effect over: FM6 back to the music */
        ym_put(1, 0xB6, music_pan6);
        ym_shadow[1][0xB6] = music_pan6;
        dac_on = 0;
    }
    if (req_no) {
        e = &pcm_table[req_no];
        req_no = 0;
        if (play_frames && e->priority < play_pr)
            e = NULL;
    }
    if (e) {
        ym_put(0, 0x2B, 0x80);      /* DAC on, centred */
        ym_put(1, 0xB6, 0xC0);
        ym_shadow[1][0xB6] = 0xC0;
        dac_on = 1;
    }

    z80_get_bus();
    if (e) {
        /* volatile: keep gcc from turning (len >> 8) into an odd-address word load */
        u32 len = *(const volatile u32 *)&e->length;
        u32 a = (u32)pcm_data + e->offset;
        Z80_RAM[0x1FF1] = (u8)a;
        Z80_RAM[0x1FF2] = (u8)((a >> 8) & 0x7F) | 0x80;
        Z80_RAM[0x1FF3] = (u8)(a >> 15);
        Z80_RAM[0x1FF4] = (u8)(a >> 23);
        Z80_RAM[0x1FF5] = (u8)len;
        Z80_RAM[0x1FF6] = (u8)(len >> 8);
        Z80_RAM[0x1FF7] = (u8)(len >> 16);
        Z80_RAM[0x1FF0] = 1;
        play_pr = e->priority;
        play_frames = (u16)(len >> 8) + 1;          /* ~260 samples per frame */
    }
    rd = Z80_RAM[RING_RD];
    wr = Z80_RAM[RING_WR];
#ifdef Z80_PEEK
    {
        extern u8 z80_peek[64][4];
        u8 *pk = z80_peek[frame_count & 63];
        pk[0] = Z80_RAM[0x1FF8]; pk[1] = rd; pk[2] = wr; pk[3] = (u8)qlen;
    }
#endif
    room = (u8)(rd - wr - 1);
    n = qlen;
    if (n > room)
        n = room - room % 3;
    for (i = 0; i < n; i++)
        Z80_RAM[RING + wr++] = queue[i];
    Z80_RAM[RING_WR] = wr;
    z80_release_bus();
    /* keep what did not fit for the next frame */
    for (i = n; i < qlen; i++)
        queue[i - n] = queue[i];
    qlen -= n;
}
