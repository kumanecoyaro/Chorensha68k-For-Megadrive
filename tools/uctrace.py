#!/usr/bin/env python3
"""Run the Mega Drive ROM on unicorn's 68000 core with stub hardware (debug aid).

usage: uctrace.py game.elf ROM.bin [max_insns] [--frames N]

  * ROM is mapped read-only: writes into it are reported with the PC
  * VDP / Z80 / I/O accesses are stubbed; a fake VBlank interrupt is raised
    every --cycles instructions (bumps vblank_count via the real handler)
  * on an exception vector being taken or a bad access, prints the last PCs
    with symbol names
"""
import sys, struct, bisect, subprocess, collections
from unicorn import *
from unicorn.m68k_const import *

def symbols(elf):
    out = subprocess.run(['m68k-linux-gnu-nm', '-n', elf], capture_output=True, text=True).stdout
    s = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3 and p[1] in 'tTdDbBrR':
            s.append((int(p[0], 16), p[2]))
    return s

import re as _re
_fst = []
try:
    for ln in open('build/SZ2.lst'):
        m = _re.match(r'^fn_([0-9a-f]+):', ln)
        if m:
            _fst.append(int(m.group(1), 16))
except OSError:
    pass
_fst.sort()
def fnname(a):
    i = bisect.bisect_right(_fst, a) - 1
    return 'fn_%05x' % _fst[i] if i >= 0 else '?'

def main():
    elf, rom = sys.argv[1], sys.argv[2]
    maxi = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('-') else 50_000_000
    period = 20000
    brk = {}
    prof = collections.Counter() if '--profile' in sys.argv else None
    fl = []
    padv = int(sys.argv[sys.argv.index('--pad') + 1], 16) if '--pad' in sys.argv else 0
    for i, a in enumerate(sys.argv):
        if a == '--break':
            v = sys.argv[i + 1].split(':')
            brk[int(v[0], 16)] = int(v[1]) if len(v) > 1 else 3
    if '--period' in sys.argv:
        period = int(sys.argv[sys.argv.index('--period') + 1])
    syms = symbols(elf); sa = [a for a, _ in syms]
    def name(a):
        if 0x10000 <= a < 0x5a000:
            return f'orig+{a - 0x10000:05x}'
        i = bisect.bisect_right(sa, a) - 1
        return f'{syms[i][1]}+{a - syms[i][0]:x}' if i >= 0 else hex(a)
    data = open(rom, 'rb').read()
    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    try:
        uc.ctl_set_cpu_model(UC_CPU_M68K_M68000)
    except Exception as e:
        print('cpu model', e)
    uc.mem_map(0, 0x400000, UC_PROT_READ | UC_PROT_EXEC)
    uc.mem_write(0, data[:0x400000])
    uc.mem_map(0xFF0000, 0x10000)
    uc.mem_map(0xA00000, 0x20000)          # Z80 RAM, I/O ($A10000), bus ($A11100)
    uc.mem_map(0xC00000, 0x10000)          # VDP
    last = collections.deque(maxlen=64)
    sym = dict((n, a) for a, n in syms)
    vint = struct.unpack('>I', data[0x78:0x7C])[0]
    rte_addr = data.index(b'\x4e\x73', vint)     # rte of _vint
    vs_addr = sym['x_vsync']; vs_lo = vs_addr; vs_hi = sym.get('x_xsp_out', vs_addr + 0x40)
    counters = [sym['vblank_count'], sym['ORIG_VBLCNT']] if 'ORIG_VBLCNT' in sym else [sym['vblank_count']]
    state = {'n': 0, 'vbl': 0}
    vint = struct.unpack('>I', data[0x78:0x7C])[0]
    def code(uc, addr, size, ud):
        if addr & 1 or (addr < 0x200 and addr not in exc_addrs) or (0x5a000 <= addr < 0x5e000):
            report(f'bad PC {addr:#x}')
            uc.emu_stop()
            return
        last.append(addr)
        if prof is not None:
            prof[addr] += 1
        if addr == vs_addr:
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            fl.append((gf, state['n'] - state.get('vs', 0) - state.get('wait', 0))); state['vs'] = state['n']; state['wait'] = 0
        if vs_lo <= addr < vs_hi:
            state['wait'] = state.get('wait', 0) + 1
        if addr in brk and brk[addr] > 0:
            brk[addr] -= 1
            sp = uc.reg_read(UC_M68K_REG_A7)
            regs = [uc.reg_read(getattr(unicorn.m68k_const, f'UC_M68K_REG_{r}')) for r in
                    ['D0', 'D1', 'D2', 'D3', 'A0', 'A1', 'A2', 'A3']]
            print(f'break {name(addr)}: d0-d3/a0-a3', ' '.join(f'{r:08x}' for r in regs),
                  'stack', uc.mem_read(sp, 32).hex())
        state['n'] += 1
        if addr == rte_addr:               # rte of the VBlank handler (done by hand)
            sp = uc.reg_read(UC_M68K_REG_A7)
            sr, pc = struct.unpack('>HI', uc.mem_read(sp, 6))
            uc.reg_write(UC_M68K_REG_A7, sp + 6)
            uc.reg_write(UC_M68K_REG_SR, sr)
            uc.reg_write(UC_M68K_REG_PC, pc)
            state['in_int'] = False
            return
        if state['n'] % period == 0:
            state['pend'] = True
        if state.get('pend') and not state.get('in_int'):
            sr = uc.reg_read(UC_M68K_REG_SR)
            if (sr >> 8) & 7 < 6:          # take the level-6 interrupt
                state['pend'] = False
                state['in_int'] = True
                sp = uc.reg_read(UC_M68K_REG_A7) - 6
                uc.mem_write(sp, struct.pack('>HI', sr, addr))
                uc.reg_write(UC_M68K_REG_A7, sp)
                uc.reg_write(UC_M68K_REG_SR, (sr & 0xF8FF) | 0x2600)
                uc.reg_write(UC_M68K_REG_PC, vint)
                state['vbl'] += 1
                return
        if addr in exc_addrs:
            report(f'exception handler {exc_addrs[addr]} reached')
            uc.emu_stop()
    def report(msg):
        print(msg, 'after', state['n'], 'insns,', state['vbl'], 'vblanks')
        for a in list(last)[-40:]:
            print('   ', hex(a), name(a))
        regs = [uc.reg_read(getattr(unicorn.m68k_const, f'UC_M68K_REG_{r}')) for r in
                ['D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'A0', 'A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7']]
        print('   regs', ' '.join(f'{r:08x}' for r in regs))
    def wr(uc, access, addr, size, value, ud):
        if addr < 0x400000:
            report(f'WRITE to ROM {addr:#x} = {value:#x}')
            uc.emu_stop()
    def bad(uc, access, addr, size, value, ud):
        report(f'bad access {access} at {addr:#x}')
        return False
    def rd(uc, access, addr, size, value, ud):
        if 0xC00004 <= addr < 0xC00008:     # VDP status: alternate vblank bit
            uc.mem_write(0xC00004, struct.pack('>H', 0x3408 if state['n'] & 1 else 0x3400))
        if addr == 0xA01FFC:                # Z80 ring: everything consumed
            uc.mem_write(0xA01FFC, bytes(uc.mem_read(0xA01FFD, 1)))
        if addr == 0xA10003:                # 3-button pad, active low
            th = uc.mem_read(0xA10003, 1)[0] & 0x40
            p = padv
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]     # B_4a0ca game frame
            if '--autofire' in sys.argv and (gf // 3) % 2:
                p &= ~0x60
            if th:
                v = 0x40 | (~p & 0x3F)
            else:
                v = (~p & 0x03) | ((~p >> 2) & 0x10) | ((~p >> 2) & 0x20)
            uc.mem_write(0xA10003, bytes([v & 0x7F]))
        if addr == 0xA11100:                # Z80 bus granted
            uc.mem_write(0xA11100, b'\x00\x00')
    import unicorn
    exc_addrs = {}
    for v, n in ((2, 'bus'), (3, 'addr'), (4, 'illegal')):
        exc_addrs[struct.unpack('>I', data[v * 4:v * 4 + 4])[0]] = n
    uc.hook_add(UC_HOOK_CODE, code)
    uc.hook_add(UC_HOOK_MEM_WRITE, wr, begin=0, end=0x3FFFFF)
    uc.hook_add(UC_HOOK_MEM_READ, rd, begin=0xA00000, end=0xC0FFFF)
    uc.mem_write(0xA10003, b'\x7f')
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, bad)
    sp, pc = struct.unpack('>II', data[:8])
    uc.reg_write(UC_M68K_REG_A7, sp)
    uc.reg_write(UC_M68K_REG_SR, 0x2700)
    try:
        uc.emu_start(pc, 0x7FFFFFFF, count=maxi)
    except UcError as e:
        report(f'UcError {e} pc={uc.reg_read(UC_M68K_REG_PC):#x}')
    if prof is not None:
        byname = collections.Counter()
        for a, c in prof.items():
            n = name(a)
            byname[n.split('+')[0] if not n.startswith('orig') else 'orig_' + fnname(a - 0x10000)] += c
        tot = sum(byname.values())
        if '--hot' in sys.argv:
            fn = sys.argv[sys.argv.index('--hot') + 1]
            hot = sorted(((c, a) for a, c in prof.items() if name(a).split('+')[0] == fn), reverse=True)
            for c, a in hot[:40]:
                print(f'   {c:8d} {name(a)}')
        for n, c in byname.most_common(40):
            print(f'{c * 100 / tot:6.2f}% {n}')
    if fl:
        byf = dict(fl[1:])
        for a in range(0, max(byf) + 1, 200):
            v = [byf[f] for f in range(a, a + 200) if f in byf]
            if v:
                print(f'game frames {a:5d}-{a + 199:5d}: avg {sum(v) // len(v):6d} max {max(v):6d} insns')
    for v in ('stat_rec', 'stat_calls', 'stat_pieces', 'stat_out'):
        if v in sym:
            print(v, struct.unpack('>I', uc.mem_read(sym[v], 4))[0])
    if 'xsp_misses' in sym:
        print('cache misses', struct.unpack('>H', uc.mem_read(sym['xsp_misses'], 2))[0])
    print('stopped after', state['n'], 'insns,', state['vbl'], 'vblanks, pc', name(uc.reg_read(UC_M68K_REG_PC)))

if __name__ == '__main__':
    main()
