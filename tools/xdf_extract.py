#!/usr/bin/env python3
"""Extract files from an X68000 2HD (Human68k FAT12) XDF disk image."""
import os, struct, sys

def main(img_path, out_dir):
    d = open(img_path, 'rb').read()
    bps = struct.unpack_from('<H', d, 0x0B)[0] if False else 1024
    # Human68k boot sector uses big-endian-ish BPB at 0x12; use standard 2HD layout
    spc, rsv, nfat, rootent, fatsz = 1, 1, 2, 192, 2
    fat_off = rsv * bps
    root_off = (rsv + nfat * fatsz) * bps
    data_off = root_off + rootent * 32
    fat = d[fat_off:fat_off + fatsz * bps]

    def fat12(n):
        o = n * 3 // 2
        v = fat[o] | (fat[o + 1] << 8)
        return (v >> 4) if n & 1 else (v & 0xFFF)

    def read_chain(cl, size):
        out = bytearray()
        while 2 <= cl < 0xFF8:
            o = data_off + (cl - 2) * bps * spc
            out += d[o:o + bps * spc]
            cl = fat12(cl)
        return bytes(out[:size]) if size is not None else bytes(out)

    def walk(raw, path):
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0: break
            if e[0] == 0xE5: continue
            attr = e[11]
            if attr & 0x08: continue
            name = (e[0:8] + e[12:22]).rstrip(b' \0').decode('cp932', 'replace')
            ext = e[8:11].rstrip(b' ').decode('cp932', 'replace')
            fn = name + ('.' + ext if ext else '')
            if fn in ('.', '..'): continue
            cl = struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            p = os.path.join(path, fn)
            if attr & 0x10:
                os.makedirs(p, exist_ok=True)
                walk(read_chain(cl, None), p)
            else:
                open(p, 'wb').write(read_chain(cl, size))
                print(f'{size:8d}  {p}')

    os.makedirs(out_dir, exist_ok=True)
    walk(d[root_off:data_off], out_dir)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
