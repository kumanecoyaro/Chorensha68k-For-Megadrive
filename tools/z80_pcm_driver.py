#!/usr/bin/env python3
"""Generate the Z80 PCM driver (YM2612 DAC, 1 channel, ~15.6 kHz) as a C array.

usage: z80_pcm_driver.py OUT.c

Mailbox in Z80 RAM (written by the 68000 while it holds the Z80 bus):
  $1FF0  command: 1 = start sample (driver clears it), 2 = stop
  $1FF1  window pointer (16-bit LE, $8000 | (addr & $7FFF))
  $1FF3  bank (16-bit LE, addr >> 15)
  $1FF5  length low 16 bits (LE)
  $1FF7  length bits 16-23
  $1FF8  status: 1 while playing
  $1FFA  current bank (driver work)
"""
import sys

# per-sample loop is ~96 T-states + delay; 3579545 / 15625 = 229 T-states
DELAY = 8           # 7 + 16*DELAY - 5 = 130 T-states

def assemble():
    code = []          # list of (bytes or ('rel'|'abs', label))
    labels = {}
    def emit(*b): code.extend(b)
    def lab(n): labels[n] = sum(1 if isinstance(x, int) else (1 if x[0] == 'rel' else 2) for x in code)
    def w(v): return [v & 0xFF, v >> 8]
    rel = lambda n: ('rel', n)
    ab = lambda n: ('abs', n)

    emit(0xF3)                                  # di
    emit(0x31, *w(0x1FF0))                      # ld sp,$1FF0
    emit(0x21, *w(0x4002))                      # ld hl,$4002   (YM2612 part II)
    emit(0x36, 0xB6, 0x23, 0x36, 0xC0)          # ld (hl),$B6 ; inc hl ; ld (hl),$C0  ch6 L+R
    emit(0x21, *w(0x4000))                      # ld hl,$4000
    emit(0x36, 0x2B)                            # ld (hl),$2B
    emit(0x23)                                  # inc hl
    emit(0x36, 0x80)                            # ld (hl),$80  DAC enable
    emit(0x2B)                                  # dec hl
    emit(0x36, 0x2A)                            # ld (hl),$2A  select DAC data
    lab('silence')
    emit(0xAF, 0x32, *w(0x1FF8))                # xor a ; ld ($1FF8),a
    emit(0x3E, 0x80, 0x32, *w(0x4001))          # ld a,$80 ; ld ($4001),a
    lab('idle')
    emit(0x3A, *w(0x1FF0))                      # ld a,($1FF0)
    emit(0xFE, 0x01)                            # cp 1
    emit(0x20, rel('idle'))                     # jr nz,idle
    lab('start')
    emit(0xAF, 0x32, *w(0x1FF0))                # xor a ; ld ($1FF0),a
    emit(0x3C, 0x32, *w(0x1FF8))                # inc a ; ld ($1FF8),a
    emit(0x2A, *w(0x1FF3))                      # ld hl,($1FF3)
    emit(0x22, *w(0x1FFA))                      # ld ($1FFA),hl
    emit(0xCD, ab('setbank'))                   # call setbank
    emit(0x2A, *w(0x1FF1))                      # ld hl,($1FF1)
    emit(0xED, 0x4B, *w(0x1FF5))                # ld bc,($1FF5)
    emit(0x3A, *w(0x1FF7), 0x57)                # ld a,($1FF7) ; ld d,a
    emit(0x3E, 0x2A, 0x32, *w(0x4000))          # ld a,$2A ; ld ($4000),a
    lab('play')
    emit(0x7E)                                  # ld a,(hl)
    emit(0x32, *w(0x4001))                      # ld ($4001),a
    emit(0x23)                                  # inc hl
    emit(0x7C, 0xB7)                            # ld a,h ; or a
    emit(0x28, rel('nextbank'))                 # jr z,nextbank
    lab('counted')
    emit(0x0B)                                  # dec bc
    emit(0x78, 0xB1)                            # ld a,b ; or c
    emit(0x28, rel('lenhi'))                    # jr z,lenhi
    lab('poll')
    emit(0x3A, *w(0x1FF0), 0xB7)                # ld a,($1FF0) ; or a
    emit(0x20, rel('cmd'))                      # jr nz,cmd
    emit(0x3E, DELAY)                           # ld a,DELAY
    lab('dl')
    emit(0x3D, 0x20, rel('dl'))                 # dec a ; jr nz,dl
    emit(0xC3, ab('play'))                      # jp play
    lab('cmd')
    emit(0xFE, 0x01)                            # cp 1
    emit(0xCA, ab('start'))                     # jp z,start
    emit(0xAF, 0x32, *w(0x1FF0))                # xor a ; ld ($1FF0),a   (stop)
    emit(0xC3, ab('silence'))                   # jp silence
    lab('lenhi')
    emit(0x7A, 0xB7)                            # ld a,d ; or a
    emit(0xCA, ab('silence'))                   # jp z,silence
    emit(0x15)                                  # dec d
    emit(0xC3, ab('poll'))                      # jp poll
    lab('nextbank')
    emit(0x2A, *w(0x1FFA), 0x23, 0x22, *w(0x1FFA))   # ld hl,(bank) ; inc hl ; ld (bank),hl
    emit(0xCD, ab('setbank'))                   # call setbank
    emit(0x21, *w(0x8000))                      # ld hl,$8000
    emit(0xC3, ab('counted'))                   # jp counted
    lab('setbank')                              # bank register: 9 serial writes of bit 0
    emit(0xC5)                                  # push bc
    emit(0x21, *w(0x6000))                      # ld hl,$6000
    emit(0x3A, *w(0x1FFA))                      # ld a,($1FFA)
    emit(0x06, 0x08)                            # ld b,8
    lab('sb')
    emit(0x77, 0x0F, 0x10, rel('sb'))           # ld (hl),a ; rrca ; djnz sb
    emit(0x3A, *w(0x1FFB), 0x77)                # ld a,($1FFB) ; ld (hl),a
    emit(0xC1, 0xC9)                            # pop bc ; ret

    out = []
    for x in code:
        if isinstance(x, int):
            out.append(x)
        elif x[0] == 'rel':
            d = labels[x[1]] - (len(out) + 1)
            assert -128 <= d < 128
            out.append(d & 0xFF)
        else:
            out += [labels[x[1]] & 0xFF, labels[x[1]] >> 8]
    return bytes(out)

def main(dst):
    b = assemble()
    lines = ['/* generated by tools/z80_pcm_driver.py - do not edit */', '#include "types.h"',
             f'const u16 z80_driver_size = {len(b)};', 'const u8 z80_driver[] = {']
    for i in range(0, len(b), 16):
        lines.append('  ' + ', '.join(f'0x{v:02X}' for v in b[i:i + 16]) + ',')
    lines.append('};')
    open(dst, 'w').write('\n'.join(lines) + '\n')
    print(f'Z80 driver: {len(b)} bytes')

if __name__ == '__main__':
    main(sys.argv[1])
