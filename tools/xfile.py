#!/usr/bin/env python3
"""Human68k .X executable loader (base address 0)."""
import struct

class XFile:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:2] == b'HU'
        (self.base, self.entry, self.text_size, self.data_size, self.bss_size,
         self.reloc_size, self.sym_size) = struct.unpack_from('>7I', d, 4)
        o = 0x40
        self.image = bytearray(d[o:o + self.text_size + self.data_size])
        o += self.text_size + self.data_size
        rel = d[o:o + self.reloc_size]
        self.relocs = []          # offsets of longwords holding addresses
        pos, i = 0, 0
        while i < len(rel):
            w = struct.unpack_from('>H', rel, i)[0]; i += 2
            if w == 1:
                w = struct.unpack_from('>I', rel, i)[0]; i += 4
            pos += w
            self.relocs.append(pos)
        self.text_end = self.text_size
        self.data_end = self.text_size + self.data_size
        self.bss_end = self.data_end + self.bss_size

    def l(self, a):
        return struct.unpack_from('>I', self.image, a)[0]

    def w(self, a):
        return struct.unpack_from('>H', self.image, a)[0]
