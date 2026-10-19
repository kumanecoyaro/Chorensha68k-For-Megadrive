#!/usr/bin/env python3
"""Finalize a Mega Drive ROM: pad, write the 0x100 header fields, fix the checksum.

usage: fixrom.py IN.bin OUT.bin
"""
import struct, sys

def field(s, n):
    b = s.encode('ascii')
    assert len(b) <= n
    return b + b' ' * (n - len(b))

def main(src, dst):
    rom = bytearray(open(src, 'rb').read())
    size = max(0x20000, (len(rom) + 0x1FFFF) & ~0x1FFFF)
    rom += b'\xFF' * (size - len(rom))
    hdr = (field('SEGA MEGA DRIVE', 16) + field('(C)FMBE 2026.OCT', 16) +
           field('CHO REN SHA 68K - MD PORT TEST', 48) + field('CHO REN SHA 68K - MD PORT TEST', 48) +
           field('GM 00000000-00', 14) + b'\0\0' + field('J', 16) +
           struct.pack('>IIII', 0, size - 1, 0xFF0000, 0xFFFFFF) +
           field('', 12) + field('', 12) + field('', 40) + field('JUE', 16))
    assert len(hdr) == 0x100
    rom[0x100:0x200] = hdr
    csum = sum(struct.unpack(f'>{(size - 0x200) // 2}H', rom[0x200:])) & 0xFFFF
    rom[0x18E:0x190] = struct.pack('>H', csum)
    open(dst, 'wb').write(rom)
    print(f'{dst}: {size} bytes, checksum {csum:04X}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
