#!/usr/bin/env python3
"""Fast profile of the Mega Drive ROM on unicorn's 68000 (a lean tools/uctrace.py).

usage: ucprof.py game.elf ROM.bin --range A,B [--pad HEX] [--hot FN] [--fn FN] [--save FILE]

Counts executed bytes per basic block (a block hook, not one Python call per
instruction: some 20x faster than uctrace --profile) over the game frames
A..B and prints the share per function, --hot FN its hottest addresses.
Code hooks are only on the few addresses the VBlank emulation needs: the
waits for a frame (the fake VBlank is taken there, as in uctrace) and the
handler's rte.  --pad: buttons held (bit 5 = C: fire), A presses START on
the title.
"""
import sys, struct, bisect, subprocess, collections
from unicorn import *
from unicorn.m68k_const import *

def symbols(elf):
    out = subprocess.run(['m68k-linux-gnu-nm', '-n', elf], capture_output=True, text=True).stdout
    return [(int(p[0], 16), p[2]) for p in (ln.split() for ln in out.splitlines())
            if len(p) == 3 and p[1] in 'tTdDbBrR']

def main():
    elf, rom = sys.argv[1], sys.argv[2]
    arg = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    lo, hi = (int(v) for v in arg('--range').split(','))
    padv = int(arg('--pad', '20'), 16)
    period = int(arg('--period', '60000'))       # bytes between fake VBlanks (~20000 insns)
    syms = symbols(elf); sa = [a for a, _ in syms]; sym = {n: a for a, n in syms}
    fst = []
    try:
        import re
        for ln in open('build/SZ2.lst'):
            m = re.match(r'^(fn_|L)([0-9a-f]+):', ln)
            if m and m.group(1) == 'fn_':
                fst.append(int(m.group(2), 16))
    except OSError:
        pass
    def name(a):
        if 0x10000 <= a < 0x5a000:
            i = bisect.bisect_right(fst, a - 0x10000) - 1
            return 'orig_fn_%05x' % fst[i] if i >= 0 else 'orig'
        i = bisect.bisect_right(sa, a) - 1
        return syms[i][1] if i >= 0 else hex(a)
    data = open(rom, 'rb').read()
    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(UC_CPU_M68K_M68000)
    uc.mem_map(0, 0x400000, UC_PROT_READ | UC_PROT_EXEC)
    uc.mem_write(0, data[:0x400000])
    uc.mem_map(0xFF0000, 0x10000)
    uc.mem_map(0xA00000, 0x20000)
    uc.mem_map(0xC00000, 0x10000)
    vint = struct.unpack('>I', data[0x78:0x7C])[0]
    rte_addr = data.index(b'\x4e\x73', vint)
    waits = [(sym['x_vsync'], sym.get('x_xsp_out', sym['x_vsync'] + 0x40))]
    w0 = sym.get('merge_cell', sym['x_bg_chip']); waits.append((w0, w0 + 0x180))   # (chip queue full: waits for VBlank)
    waits += [(sym[k], sym[k] + 0x60) for k in ('x_xsp_out0', 'x_wait', 'x_screen_clear', 'x_xsp_out') if k in sym]
    st = {'n': 0, 'next': period, 'pend': False, 'in_int': False, 'vbl': 0, 'gf': 0, 'jump': None}
    prof = collections.Counter()
    bsize = {}
    dbg = '--debug' in sys.argv

    fn_addr = sym.get(arg('--fn', ''), None)       # --fn FN: bytes per call of FN (callees included)
    fn_pend = []; fn_calls = []
    def block(uc, addr, size, ud):
        if fn_pend and addr == fn_pend[-1][0] and uc.reg_read(UC_M68K_REG_A7) == fn_pend[-1][1]:
            r_, s_, n0 = fn_pend.pop()
            if lo <= st['gf'] <= hi:
                fn_calls.append(st['n'] - n0)
        if addr == fn_addr:
            s_ = uc.reg_read(UC_M68K_REG_A7)
            fn_pend.append((struct.unpack('>I', uc.mem_read(s_, 4))[0], s_ + 4, st['n']))
        st['n'] += size
        if dbg and st['n'] % 3000000 < size:
            print('n', st['n'], 'vbl', st['vbl'], 'gf', st['gf'], name(addr), hex(addr), st['pend'], st['in_int'], hex(uc.reg_read(UC_M68K_REG_SR)), flush=True)
        if st['n'] >= st['next']:
            st['next'] += period
            st['pend'] = True
        if lo <= st['gf'] <= hi:
            prof[addr] += size
            bsize[addr] = size

    def at_wait(uc, addr, size, ud):
        if addr == sym['x_vsync']:
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            st['gf'] = gf
            if gf > hi:
                uc.emu_stop()
                st['done'] = True
                return
        if st['pend'] and not st['in_int']:
            if (uc.reg_read(UC_M68K_REG_SR) >> 8) & 7 < 6:
                st['pend'] = False
                st['jump'] = ('int', addr)
                uc.emu_stop()

    def at_rte(uc, addr, size, ud):
        st['jump'] = ('rte', addr)
        uc.emu_stop()

    def rd(uc, access, addr, size, value, ud):
        if 0xC00004 <= addr < 0xC00008:
            uc.mem_write(0xC00004, struct.pack('>H', 0x3408 if st['n'] & 2 else 0x3400))
        elif addr == 0xA01FFC:
            uc.mem_write(0xA01FFC, bytes(uc.mem_read(0xA01FFD, 1)))
        elif addr == 0xA10003:
            th = uc.mem_read(0xA10003, 1)[0] & 0x40
            p = padv | (0x40 if 150 <= st['vbl'] < 156 else 0)
            v = 0x40 | (~p & 0x3F) if th else (~p & 0x03) | ((~p >> 2) & 0x10) | ((~p >> 2) & 0x20)
            uc.mem_write(0xA10003, bytes([v & 0x7F]))
        elif addr == 0xA11100:
            uc.mem_write(0xA11100, b'\x00\x00')

    uc.hook_add(UC_HOOK_BLOCK, block)
    for a, b in waits:
        uc.hook_add(UC_HOOK_CODE, at_wait, begin=a, end=b - 1)
    uc.hook_add(UC_HOOK_CODE, at_rte, begin=rte_addr, end=rte_addr)
    uc.hook_add(UC_HOOK_MEM_READ, rd, begin=0xA00000, end=0xC0FFFF)
    uc.mem_write(0xA10003, b'\x7f')
    sp, pc = struct.unpack('>II', data[:8])
    uc.reg_write(UC_M68K_REG_A7, sp)
    uc.reg_write(UC_M68K_REG_SR, 0x2700)
    while not st.get('done'):
        st['jump'] = None
        try:
            uc.emu_start(pc, 0x7FFFFFFF)
        except UcError as e:
            print('UcError', e, hex(uc.reg_read(UC_M68K_REG_PC)), name(uc.reg_read(UC_M68K_REG_PC)))
            break
        j = st['jump']
        if not j:
            break
        kind, at = j
        real = uc.reg_read(UC_M68K_REG_PC)
        if kind == 'rte':
            if real != rte_addr:
                pc = real
                continue
            s_ = uc.reg_read(UC_M68K_REG_A7)
            sr, pc = struct.unpack('>HI', uc.mem_read(s_, 6))
            uc.reg_write(UC_M68K_REG_A7, s_ + 6)
            uc.reg_write(UC_M68K_REG_SR, sr)
            st['in_int'] = False
        else:
            sr = uc.reg_read(UC_M68K_REG_SR)
            s_ = uc.reg_read(UC_M68K_REG_A7) - 6
            uc.mem_write(s_, struct.pack('>HI', sr, real))
            uc.reg_write(UC_M68K_REG_A7, s_)
            uc.reg_write(UC_M68K_REG_SR, (sr & 0xF8FF) | 0x2600)
            pc = vint
            st['in_int'] = True
            st['vbl'] += 1
    if '--save' in sys.argv:                     # --save FILE: block address, size, times run
        with open(arg('--save'), 'w') as f:
            for a, c in sorted(prof.items()):
                f.write(f'{a:x} {bsize[a]} {c // bsize[a]}\n')
    byname = collections.Counter()
    for a, c in prof.items():
        byname[name(a)] += c
    tot = sum(byname.values()) or 1
    print(f'game frames {lo}..{hi}, {st["vbl"]} vblanks, {tot} bytes executed in the range')
    for n, c in byname.most_common(int(arg('--top', '40'))):
        print(f'{c * 100 / tot:6.2f}% {n}')
    if fn_calls:
        v = sorted(fn_calls)
        print(f'--fn: {len(v)} calls, bytes per call median {v[len(v) // 2]} p90 {v[len(v) * 9 // 10]} mean {sum(v) // len(v)}')
    if '--hot' in sys.argv:
        fn = arg('--hot')
        hot = sorted(((c, a) for a, c in prof.items() if name(a) == fn), reverse=True)
        for c, a in hot[:40]:
            print(f'   {c * 100 / tot:6.2f}% {a:#x} {fn}+{a - sym.get(fn, a):x}')

if __name__ == '__main__':
    main()
