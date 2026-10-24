#!/usr/bin/env python3
"""Generate the Z80 sound driver as a C array.

usage: z80_pcm_driver.py OUT.c

The Z80 owns the YM2612.  Its loop is paced by YM2612 timer A (4 FM sample
periods = 13317 Hz), so the PCM rate does not depend on what else it does:
  * per tick, one 8-bit PCM sample (sound effect) goes to the DAC from a
    256-byte ring in Z80 RAM;
  * up to two bytes are read ahead from ROM into that ring (through the 32 KB
    bank window), except while the 68000 says it is doing VDP DMA (a Z80 bus
    access during DMA would stall the Z80 for the whole transfer);
  * one queued register write: the 68000 puts the music / DAC-enable writes
    into a ring buffer, so the two CPUs never fight over the YM2612 address
    latch (every DAC write sets the address again).

Z80 RAM:
  $1B00-$1BFF  PCM ring
  $1D00-$1DFF  ring buffer of 3-byte entries: part offset (0 / 2), register, value
  $1FE0  PCM ring read index, $1FE1 write index, $1FE2 68000 DMA flag, $1FE3 playing
  $1FF0  command: 1 = start sample (cleared by driver), 2 = stop
  $1FF1  window pointer (LE, $8000 | (addr & $7FFF))
  $1FF3  bank (LE, addr >> 15)
  $1FF5  length bits 0-15 (LE)
  $1FF7  length bits 16-23
  $1FF8  status: 1 while playing
  $1FF9  work: length bits 16-23
  $1FFA  work: current bank
  $1FFC  ring read index (driver)
  $1FFD  ring write index (68000)
"""
import sys

Z80_CLOCK = 3579545
FM_CLOCK = 53693175 / 7
TIMER_A = 1020                      # 1024 - 4: one tick every 4 FM samples
RATE = FM_CLOCK / 144 / (1024 - TIMER_A)

class Asm:
    def __init__(self):
        self.code = []; self.labels = {}
    def org(self):
        return sum(1 if isinstance(x, int) else (1 if x[0] == 'rel' else 2) for x in self.code)
    def lab(self, n):
        self.labels[n] = self.org()
    def e(self, *b):
        for x in b:
            self.code.append(x)
    def link(self):
        out = []
        for x in self.code:
            if isinstance(x, int):
                out.append(x)
            elif x[0] == 'rel':
                d = self.labels[x[1]] - (len(out) + 1)
                assert -128 <= d < 128, x
                out.append(d & 0xFF)
            else:
                out += [self.labels[x[1]] & 0xFF, self.labels[x[1]] >> 8]
        return bytes(out)

def w(v):
    return [v & 0xFF, v >> 8]

def assemble():
    a = Asm(); e = a.e; R = lambda n: ('rel', n); A = lambda n: ('abs', n)
    RD, WR, DMA, PLAY = 0x1FE0, 0x1FE1, 0x1FE2, 0x1FE3

    def ym0(reg, val):                          # part 0 register write (a destroyed)
        e(0xCD, A('wbusy'))
        e(0x3E, reg, 0x32, *w(0x4000))
        e(0x3E, val, 0x32, *w(0x4001))

    e(0xF3)                                     # di
    e(0x31, *w(0x1B00))                         # ld sp,$1B00
    ym0(0x24, TIMER_A >> 2)                     # timer A
    ym0(0x25, TIMER_A & 3)
    ym0(0x27, 0x15)                             # load + flag A, reset A
    a.lab('silence')
    e(0xAF, 0x32, *w(0x1FF8), 0x32, *w(PLAY))   # xor a ; status = playing = 0
    ym0(0x2A, 0x80)
    # ---- one tick
    a.lab('tick')
    e(0x3A, *w(0x4000), 0x0F)                   # ld a,($4000) ; rrca   (timer A flag)
    e(0x30, R('tick'))                          # jr nc,tick
    ym0(0x27, 0x15)                             # restart timer A, clear its flag
    e(0x3A, *w(PLAY), 0xB7)                     # ld a,(playing) ; or a
    e(0x28, R('svc_cmd'))                       # jr z,svc_cmd
    # sample out
    e(0x3A, *w(RD), 0x5F)                       # ld a,(rd) ; ld e,a
    e(0x3A, *w(WR), 0xBB)                       # ld a,(wr) ; cp e
    e(0x28, R('empty'))                         # jr z,empty
    e(0x16, 0x1B, 0x1A, 0x57)                   # ld d,$1B ; ld a,(de) ; ld d,a  (sample)
    e(0x1C, 0x7B, 0x32, *w(RD))                 # inc e ; ld a,e ; ld (rd),a
    e(0xCD, A('wbusy'))
    e(0x3E, 0x2A, 0x32, *w(0x4000))             # DAC
    e(0x7A, 0x32, *w(0x4001))                   # ld a,d ; ld ($4001),a
    # read ahead: two bytes unless the 68000 is doing DMA
    a.lab('fetch')
    e(0x3A, *w(DMA), 0xB7)                      # ld a,(dma) ; or a
    e(0x20, R('svc_cmd'))                       # jr nz,svc_cmd
    e(0xCD, A('fetch1'))
    e(0xCD, A('fetch1'))
    a.lab('svc_cmd')
    e(0xCD, A('svc'))                           # one queued register write
    e(0x3A, *w(0x1FF0), 0xB7)                   # ld a,(cmd) ; or a
    e(0x28, R('tick'))                          # jr z,tick
    e(0xFE, 0x01, 0xCA, A('start'))             # cp 1 ; jp z,start
    e(0xAF, 0x32, *w(0x1FF0))                   # stop: cmd = 0
    e(0xC3, A('silence'))
    # ring empty: the effect is over when nothing is left to read
    a.lab('empty')
    e(0x78, 0xB1)                               # ld a,b ; or c
    e(0x20, R('fetch'))                         # jr nz,fetch  (underrun: just read on)
    e(0x3A, *w(0x1FF9), 0xB7)                   # ld a,(len hi) ; or a
    e(0x20, R('fetch'))
    e(0xC3, A('silence'))
    # ---- start: parameters from the 68000
    a.lab('start')
    e(0xAF, 0x32, *w(0x1FF0))                   # cmd = 0
    e(0x32, *w(RD), 0x32, *w(WR))               # empty ring
    e(0x3C, 0x32, *w(0x1FF8), 0x32, *w(PLAY))   # status = playing = 1
    e(0x2A, *w(0x1FF3), 0x22, *w(0x1FFA))       # ld hl,(bank) ; ld (work bank),hl
    e(0xCD, A('setbank'))
    e(0x3A, *w(0x1FF7), 0x32, *w(0x1FF9))       # length bits 16-23
    e(0x2A, *w(0x1FF1))                         # ld hl,(window pointer)
    e(0xED, 0x4B, *w(0x1FF5))                   # ld bc,(length)
    e(0xC3, A('tick'))
    # ---- one byte ROM -> ring (hl = source, bc + (len hi) = bytes left)
    a.lab('fetch1')
    e(0x78, 0xB1)                               # ld a,b ; or c
    e(0x20, R('f_have'))
    e(0x3A, *w(0x1FF9), 0xB7)                   # ld a,(len hi) ; or a
    e(0xC8)                                     # ret z            (all read)
    e(0x3D, 0x32, *w(0x1FF9))                   # dec a ; ld (len hi),a
    a.lab('f_have')
    e(0x3A, *w(WR), 0x5F, 0x3C)                 # ld a,(wr) ; ld e,a ; inc a
    e(0x57, 0x3A, *w(RD), 0xBA)                 # ld d,a ; ld a,(rd) ; cp d
    e(0xC8)                                     # ret z            (ring full)
    e(0x7E)                                     # ld a,(hl)        (ROM)
    e(0x16, 0x1B, 0x12)                         # ld d,$1B ; ld (de),a
    e(0x1C, 0x7B, 0x32, *w(WR))                 # inc e ; ld a,e ; ld (wr),a
    e(0x0B)                                     # dec bc
    e(0x23, 0x7C, 0xB7, 0xC0)                   # inc hl ; ld a,h ; or a ; ret nz
    e(0x2A, *w(0x1FFA), 0x23, 0x22, *w(0x1FFA))  # next bank
    e(0xCD, A('setbank'))
    e(0x21, *w(0x8000), 0xC9)                   # ld hl,$8000 ; ret
    # ---- wait for the YM2612
    a.lab('wbusy')
    e(0x3A, *w(0x4000), 0x87)                   # ld a,($4000) ; add a,a
    e(0x38, R('wbusy'), 0xC9)                   # jr c,wbusy ; ret
    # ---- one queued YM2612 write
    a.lab('svc')
    e(0x3A, *w(0x1FFC), 0x5F)                   # ld a,(rd) ; ld e,a
    e(0x3A, *w(0x1FFD), 0xBB)                   # ld a,(wr) ; cp e
    e(0xC8)                                     # ret z
    e(0x16, 0x1D)                               # ld d,$1D
    e(0xE5)                                     # push hl
    e(0xCD, A('wbusy'))
    e(0x1A, 0x6F, 0x26, 0x40)                   # ld a,(de) ; ld l,a ; ld h,$40
    e(0x1C, 0x1A, 0x77)                         # inc e ; ld a,(de) ; ld (hl),a   address
    e(0x1C, 0x1A, 0x2C, 0x77)                   # inc e ; ld a,(de) ; inc l ; ld (hl),a  data
    e(0x1C, 0x7B, 0x32, *w(0x1FFC))             # inc e ; ld a,e ; ld (rd),a
    e(0xE1, 0xC9)                               # pop hl ; ret
    # ---- bank register: 9 serial writes of bit 0
    a.lab('setbank')
    e(0xC5, 0xE5, 0x21, *w(0x6000))             # push bc ; push hl ; ld hl,$6000
    e(0x3A, *w(0x1FFA), 0x06, 0x08)             # ld a,(bank) ; ld b,8
    a.lab('sb')
    e(0x77, 0x0F, 0x10, R('sb'))                # ld (hl),a ; rrca ; djnz sb
    e(0x3A, *w(0x1FFB), 0x77)                   # ld a,(bank+1) ; ld (hl),a
    e(0xE1, 0xC1, 0xC9)                         # pop hl ; pop bc ; ret
    return a.link()

def main(dst):
    b = assemble()
    lines = ['/* generated by tools/z80_pcm_driver.py - do not edit */', '#include "types.h"',
             f'/* PCM rate: YM2612 timer A, {RATE:.0f} Hz */',
             f'const u16 z80_driver_size = {len(b)};', 'const u8 z80_driver[] = {']
    for i in range(0, len(b), 16):
        lines.append('  ' + ', '.join(f'0x{v:02X}' for v in b[i:i + 16]) + ',')
    lines.append('};')
    open(dst, 'w').write('\n'.join(lines) + '\n')
    print(f'Z80 driver: {len(b)} bytes, PCM {RATE:.0f} Hz')

if __name__ == '__main__':
    main(sys.argv[1])
