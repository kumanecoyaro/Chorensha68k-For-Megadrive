#!/usr/bin/env python3
"""Generate the Z80 sound driver as a C array.

usage: z80_pcm_driver.py OUT.c

The Z80 owns the YM2612:
  * streams one 8-bit PCM sample (sound effect) from ROM to the DAC, ~15.6 kHz,
    switching the 32 KB bank window as needed
  * between samples (and while idle) writes one queued register write; the
    68000 puts the music / DAC-enable writes into a ring buffer, so the two
    CPUs never fight over the YM2612 address latch.

Z80 RAM:
  $1D00-$1DFF  ring buffer of 3-byte entries: part offset (0 / 2), register, value
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
RATE = 15625
DELAY = 4

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
    e(0xF3)                                     # di
    e(0x31, *w(0x1D00))                         # ld sp,$1D00
    e(0x3E, 0x2A, 0x32, *w(0x4000))             # ld a,$2A ; ld ($4000),a
    a.lab('silence')
    e(0xAF, 0x32, *w(0x1FF8))                   # xor a ; ld ($1FF8),a
    e(0x3E, 0x80, 0x32, *w(0x4001))             # ld a,$80 ; ld ($4001),a
    a.lab('idle')
    e(0xCD, A('svc'))                           # call svc
    e(0x3A, *w(0x1FF0), 0xFE, 0x01)             # ld a,($1FF0) ; cp 1
    e(0x20, R('idle'))                          # jr nz,idle
    a.lab('start')
    e(0xAF, 0x32, *w(0x1FF0))                   # xor a ; ld ($1FF0),a
    e(0x3C, 0x32, *w(0x1FF8))                   # inc a ; ld ($1FF8),a
    e(0x2A, *w(0x1FF3), 0x22, *w(0x1FFA))       # ld hl,($1FF3) ; ld ($1FFA),hl
    e(0xCD, A('setbank'))                       # call setbank
    e(0x3A, *w(0x1FF7), 0x32, *w(0x1FF9))       # ld a,($1FF7) ; ld ($1FF9),a
    e(0x2A, *w(0x1FF1))                         # ld hl,($1FF1)
    e(0xED, 0x4B, *w(0x1FF5))                   # ld bc,($1FF5)
    # ---- per-sample loop (T-states in comments)
    a.lab('play')
    e(0x7E)                                     # ld a,(hl)          7 (+68k bus wait)
    e(0x32, *w(0x4001))                         # ld ($4001),a      13
    e(0x23)                                     # inc hl             6
    e(0x7C, 0xB7)                               # ld a,h ; or a      8
    e(0x28, R('nextbank'))                      # jr z,nextbank      7
    a.lab('counted')
    e(0x0B)                                     # dec bc             6
    e(0x78, 0xB1)                               # ld a,b ; or c      8
    e(0x28, R('lenhi'))                         # jr z,lenhi         7
    a.lab('poll')
    e(0xCD, A('svc'))                           # call svc     17 + 45 (empty)
    e(0x3A, *w(0x1FF0), 0xB7)                   # ld a,(cmd) ; or a 17
    e(0x20, R('cmd'))                           # jr nz,cmd          7
    e(0x3E, DELAY)                              # ld a,DELAY         7
    a.lab('dl')
    e(0x3D, 0x20, R('dl'))                      # dec a ; jr nz,dl  16n-5
    e(0xC3, A('play'))                          # jp play           10
    a.lab('cmd')
    e(0xFE, 0x01, 0xCA, A('start'))             # cp 1 ; jp z,start
    e(0xAF, 0x32, *w(0x1FF0))                   # xor a ; ld ($1FF0),a  (stop)
    e(0xC3, A('silence'))
    a.lab('lenhi')
    e(0x3A, *w(0x1FF9), 0xB7)                   # ld a,($1FF9) ; or a
    e(0xCA, A('silence'))                       # jp z,silence
    e(0x3D, 0x32, *w(0x1FF9))                   # dec a ; ld ($1FF9),a
    e(0xC3, A('poll'))
    a.lab('nextbank')
    e(0x2A, *w(0x1FFA), 0x23, 0x22, *w(0x1FFA))  # ld hl,(bank) ; inc hl ; ld (bank),hl
    e(0xCD, A('setbank'))
    e(0x21, *w(0x8000))                         # ld hl,$8000
    e(0xC3, A('counted'))
    # ---- one queued YM2612 write
    a.lab('svc')
    e(0x3A, *w(0x1FFC), 0x5F)                   # ld a,(rd) ; ld e,a      17
    e(0x3A, *w(0x1FFD), 0xBB)                   # ld a,(wr) ; cp e        17
    e(0xC8)                                     # ret z                   11
    e(0x16, 0x1D)                               # ld d,$1D
    e(0xE5)                                     # push hl
    e(0x1A, 0x6F, 0x26, 0x40)                   # ld a,(de) ; ld l,a ; ld h,$40
    e(0x1C, 0x1A, 0x77)                         # inc e ; ld a,(de) ; ld (hl),a   address
    e(0x1C, 0x1A, 0x2C, 0x77)                   # inc e ; ld a,(de) ; inc l ; ld (hl),a  data
    e(0x1C, 0x7B, 0x32, *w(0x1FFC))             # inc e ; ld a,e ; ld (rd),a
    e(0x3E, 0x2A, 0x32, *w(0x4000))             # ld a,$2A ; ld ($4000),a  (DAC latch)
    e(0xE1, 0xC9)                               # pop hl ; ret
    # ---- bank register: 9 serial writes of bit 0
    a.lab('setbank')
    e(0xC5, 0x21, *w(0x6000))                   # push bc ; ld hl,$6000
    e(0x3A, *w(0x1FFA), 0x06, 0x08)             # ld a,($1FFA) ; ld b,8
    a.lab('sb')
    e(0x77, 0x0F, 0x10, R('sb'))                # ld (hl),a ; rrca ; djnz sb
    e(0x3A, *w(0x1FFB), 0x77)                   # ld a,($1FFB) ; ld (hl),a
    e(0xC1, 0xC9)                               # pop bc ; ret
    return a.link()

def main(dst):
    b = assemble()
    cycles = 41 + 21 + 62 + 24 + 7 + (16 * DELAY - 5) + 10
    lines = ['/* generated by tools/z80_pcm_driver.py - do not edit */', '#include "types.h"',
             f'/* sample loop: {cycles} T-states (+ROM wait) = {Z80_CLOCK / cycles:.0f} Hz */',
             f'const u16 z80_driver_size = {len(b)};', 'const u8 z80_driver[] = {']
    for i in range(0, len(b), 16):
        lines.append('  ' + ', '.join(f'0x{v:02X}' for v in b[i:i + 16]) + ',')
    lines.append('};')
    open(dst, 'w').write('\n'.join(lines) + '\n')
    print(f'Z80 driver: {len(b)} bytes, sample loop {cycles} T-states ({Z80_CLOCK / cycles:.0f} Hz)')

if __name__ == '__main__':
    main(sys.argv[1])
