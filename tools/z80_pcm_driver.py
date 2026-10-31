#!/usr/bin/env python3
"""Generate the Z80 sound driver as a C array.

usage: z80_pcm_driver.py OUT.c

The Z80 owns the YM2612.  Its loop is paced by YM2612 timer A (4 FM sample
periods = 13317 Hz), so the PCM rate does not depend on what else it does.
Every tick one 8-bit PCM sample (sound effect) goes to the DAC from a
256-byte ring in Z80 RAM; ticks alternate between
  A: reading ahead two (three while the ring is low) bytes from ROM into the
     ring, through the 32 KB bank window, unless the 68000 says it is doing
     VDP DMA (a Z80 bus access during DMA stalls the Z80 for the transfer);
  B: one entry of the 68000's queue: a register write (the music, the DAC
     enable) or a command ($FF start the effect set up at $1FF1, $FE stop).
The 68000 never touches the YM2612, so the two CPUs never fight over its
address latch; every DAC write sets the address again.  A tick takes
~130 T-states plus its A / B work, a pair fits in two periods (536 T).

Registers: DE ring read pointer ($1Bxx), C ring write index, B queue read
index; DE' ring write pointer, HL' ROM window pointer, BC' bytes left (bits
0-15; 16-23 at $1FF9).

Z80 RAM:
  $1B00-$1BFF  PCM ring
  $1D00-$1DFF  queue of 3-byte entries: part offset (0 / 2) or command, register, value
  $1FE2  68000 doing VDP DMA
  $1FF1  window pointer (LE, $8000 | (addr & $7FFF))
  $1FF3  bank (LE, addr >> 15)
  $1FF5  length bits 0-15 (LE)
  $1FF7  length bits 16-23
  $1FF9  work: length bits 16-23
  $1FFA  work: current bank
  $1FFC  queue read index (driver)
  $1FFD  queue write index (68000)
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
    DMA, LVL, LENHI, BANK, QRD, QWR = 0x1FE2, 0x1FE3, 0x1FF9, 0x1FFA, 0x1FFC, 0x1FFD

    def tick(n):                                # wait, restart timer A, one sample out
        a.lab('poll' + n)
        e(0x3A, *w(0x4000), 0x0F)               # ld a,($4000) ; rrca      (timer A flag)
        e(0x30, R('poll' + n))                  # jr nc,poll
        e(0x3E, 0x27, 0x32, *w(0x4000))         # $27 = $15: reload + flag A, reset A
        e(0x3E, 0x15, 0x32, *w(0x4001))
        e(0x7B, 0xB9)                           # ld a,e ; cp c           (ring empty?)
        e(0x28, R('nos' + n))                   # jr z,nos                (the DAC keeps its level)
        e(0x1A, 0x1C, 0x6F)                     # ld a,(de) ; inc e ; ld l,a
        e(0x3E, 0x2A, 0x32, *w(0x4000))         # DAC
        e(0x7D, 0x32, *w(0x4001))               # ld a,l ; ld ($4001),a
        a.lab('nos' + n)

    def fetch(n):                               # (alternate set) one byte ROM -> ring
        e(0x78, 0xB1)                           # ld a,b ; or c
        e(0x20, R('fg' + n))                    # jr nz,fg
        e(0x3A, *w(LENHI), 0xB7)                # ld a,(len hi) ; or a
        e(0x28, R('fend'))                      # jr z,fend        (all read)
        e(0x3D, 0x32, *w(LENHI))                # dec a ; ld (len hi),a
        a.lab('fg' + n)
        e(0x7E, 0x12, 0x1C)                     # ld a,(hl) ; ld (de),a ; inc e
        e(0x23, 0x0B)                           # inc hl ; dec bc
        e(0x7C, 0xB7)                           # ld a,h ; or a
        e(0xCC, A('nextbank'))                  # call z,nextbank

    e(0xF3)                                     # di
    e(0x31, *w(0x1B00))                         # ld sp,$1B00
    for reg, val in ((0x24, TIMER_A >> 2), (0x25, TIMER_A & 3), (0x27, 0x15), (0x2A, 0x80)):
        e(0x3A, *w(0x4000), 0x87, 0x38, 0xFA)   # wait while busy
        e(0x3E, reg, 0x32, *w(0x4000), 0x3E, val, 0x32, *w(0x4001))
    e(0x11, *w(0x1B00), 0x01, *w(0x0000))       # ld de,$1B00 ; ld bc,0
    e(0xD9)                                     # exx
    e(0x11, *w(0x1B00), 0x01, *w(0x0000))       # ld de,$1B00 ; ld bc,0
    e(0x21, *w(0x8000))                         # ld hl,$8000
    e(0xD9)                                     # exx
    e(0xAF, 0x32, *w(LENHI))                    # xor a ; ld (len hi),a
    # ---- tick A: read ahead
    a.lab('ta')
    tick('a')
    e(0x3A, *w(DMA), 0xB7)                      # ld a,(dma) ; or a
    e(0x20, R('tb'))                            # jr nz,tb
    e(0x79, 0x93, 0x32, *w(LVL))                # ld a,c ; sub e ; ld (lvl),a   (bytes in the ring)
    e(0xD9)                                     # exx
    fetch('1'); fetch('2')
    e(0x3A, *w(LVL), 0xFE, 0xC0)                # ld a,(lvl) ; cp $C0
    e(0x30, R('fend'))                          # jr nc,fend       (enough read ahead)
    fetch('3')
    a.lab('fend')
    e(0x7B, 0xD9, 0x4F)                         # ld a,e ; exx ; ld c,a
    # ---- tick B: one queue entry
    a.lab('tb')
    tick('b')
    e(0x3A, *w(QWR), 0xB8)                      # ld a,(wr) ; cp b
    e(0xCA, A('ta'))                            # jp z,ta
    e(0x26, 0x1D, 0x68)                         # ld h,$1D ; ld l,b
    e(0x7E, 0x2C, 0xB7)                         # ld a,(hl) ; inc l ; or a
    e(0xFA, A('cmd'))                           # jp m,cmd
    e(0x28, R('p0'))                            # jr z,p0
    a.lab('w2')
    e(0x3A, *w(0x4000), 0x87, 0x38, R('w2'))    # wait while busy
    e(0x7E, 0x32, *w(0x4002), 0x2C)             # address (part II) ; inc l
    e(0x7E, 0x32, *w(0x4003))                   # data
    e(0x18, R('qd'))
    a.lab('p0')
    e(0x3A, *w(0x4000), 0x87, 0x38, R('p0'))    # wait while busy
    e(0x7E, 0x32, *w(0x4000), 0x2C)             # address ; inc l
    e(0x7E, 0x32, *w(0x4001))                   # data
    a.lab('qd')
    e(0x2C, 0x45, 0x7D, 0x32, *w(QRD))          # inc l ; ld b,l ; ld a,l ; ld (rd),a
    e(0xC3, A('ta'))
    # ---- commands: $FF start, $FE stop
    a.lab('cmd')
    e(0x2C, 0x2C, 0x45, 0x4F)                   # inc l ; inc l ; ld b,l ; ld c,a  (c: command)
    e(0x7D, 0x32, *w(QRD))                      # ld a,l ; ld (rd),a
    e(0x79, 0xFE, 0xFF)                         # ld a,c ; cp $FF
    e(0xD9)                                     # exx
    e(0x20, R('stop'))                          # jr nz,stop
    e(0x2A, *w(0x1FF3), 0x22, *w(BANK))         # ld hl,(bank) ; ld (work bank),hl
    e(0xCD, A('setbank'))
    e(0x3A, *w(0x1FF7), 0x32, *w(LENHI))        # length bits 16-23
    e(0x2A, *w(0x1FF1))                         # ld hl,(window pointer)
    e(0xED, 0x4B, *w(0x1FF5))                   # ld bc,(length)
    e(0x18, R('flush'))
    a.lab('stop')
    e(0x01, *w(0x0000), 0xAF, 0x32, *w(LENHI))  # ld bc,0 ; xor a ; ld (len hi),a
    a.lab('flush')
    e(0x7B, 0xD9, 0x4F, 0x5F)                   # ld a,e ; exx ; ld c,a ; ld e,a   (ring: read = write)
    e(0xC3, A('ta'))
    # ---- next 32 KB bank (alternate set: HL' = 0)
    a.lab('nextbank')
    e(0xE5, 0x2A, *w(BANK), 0x23, 0x22, *w(BANK), 0xE1)   # push hl ; bank++ ; pop hl
    e(0xCD, A('setbank'))
    e(0x21, *w(0x8000), 0xC9)                   # ld hl,$8000 ; ret
    # ---- bank register: 9 serial writes of bit 0
    a.lab('setbank')
    e(0xC5, 0xE5, 0x21, *w(0x6000))             # push bc ; push hl ; ld hl,$6000
    e(0x3A, *w(BANK), 0x06, 0x08)               # ld a,(bank) ; ld b,8
    a.lab('sb')
    e(0x77, 0x0F, 0x10, R('sb'))                # ld (hl),a ; rrca ; djnz sb
    e(0x3A, *w(BANK + 1), 0x77)                 # ld a,(bank+1) ; ld (hl),a
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
