#!/usr/bin/env python3
"""Run the Mega Drive ROM on unicorn's 68000 core with stub hardware (debug aid).

usage: uctrace.py game.elf ROM.bin [max_insns] [--frames N]

  * ROM is mapped read-only: writes into it are reported with the PC
  * VDP / Z80 / I/O accesses are stubbed; a fake VBlank interrupt is raised
    every --cycles instructions (bumps vblank_count via the real handler)
  * on an exception vector being taken or a bad access, prints the last PCs
    with symbol names
"""
import os, sys, struct, bisect, subprocess, collections
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
    chipcount = collections.Counter()
    pcghist = collections.Counter() if '--calls' in sys.argv else None
    pcgspan = {}
    bglog = int(sys.argv[sys.argv.index('--bglog') + 1]) if '--bglog' in sys.argv else 0   # BG scroll every N frames + chip draws
    bglog_map = 0
    if bglog:
        for ln in open('build/gen/orig.h'):
            if 'ORIG_BLOB' in ln and '#define' in ln:
                bglog_map = int(ln.split()[2], 16)
    drawlog = [int(v) for v in sys.argv[sys.argv.index('--drawlog') + 1].split(',')] if '--drawlog' in sys.argv else None
    explc = collections.Counter() if '--explcount' in sys.argv else None   # expl4 draws per game frame
    # --fnlog A,B,..: log every call of these original functions (game frame, caller)
    fnlog = {0x10000 + int(v, 16) for v in sys.argv[sys.argv.index('--fnlog') + 1].split(',')} if '--fnlog' in sys.argv else set()
    bombs = [int(v) for v in sys.argv[sys.argv.index('--bomb') + 1].split(',')] if '--bomb' in sys.argv else []
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
    span = None; spans = []
    if '--span' in sys.argv:
        a, b = sys.argv[sys.argv.index('--span') + 1].split(',')
        span = (sym[a], sym[b])
    vint = struct.unpack('>I', data[0x78:0x7C])[0]
    rte_addr = data.index(b'\x4e\x73', vint)     # rte of _vint
    wait_lo = sym.get('merge_cell', sym['x_bg_chip']); wait_hi = wait_lo + 0x80   # (chip queue full: waits for VBlank)
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
        if '--t056' in sys.argv and addr in (0x156da, 0x1576c, 0x15862, 0x15644):   # TRACE056
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            if 281 <= gf <= 283 and state.get('t5', 0) < 60:
                state['t5'] = state.get('t5', 0) + 1
                print('t056', name(addr), 'gf', gf, 'd3', uc.reg_read(UC_M68K_REG_D3), 'd5', uc.reg_read(UC_M68K_REG_D5),
                      'd6', hex(uc.reg_read(UC_M68K_REG_D6)), 'sp', hex(uc.reg_read(UC_M68K_REG_A7)))
        if addr == sym.get('h_xobj_set') and explc is not None and 0xC0 <= uc.reg_read(UC_M68K_REG_D2) & 0xFFFF < 0x210:
            explc[struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]] += 1
        if bglog and addr == sym.get('bg_scroll'):
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            if gf % bglog == 0:
                a = struct.unpack('>iiii', uc.mem_read(uc.reg_read(UC_M68K_REG_A7) + 4, 16))
                print(f'bgscroll gf {gf} x0 {a[0]} x1 {a[1]} y0 {a[2]} y1 {a[3]}')
        if bglog and addr == 0x10000 + 0x55d8:          # fn_05574: map byte read (a1 = column, d0 = row address)
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            off = uc.reg_read(UC_M68K_REG_D0) - bglog_map
            col = uc.reg_read(UC_M68K_REG_A1) & 0xFFFF
            if col == 0:
                print(f'bgrow gf {gf} layer {off >> 13} row {(off & 0x1fff) >> 3}')
        if bglog and addr == sym.get('x_bg_chip'):
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            a = struct.unpack('>iiiii', uc.mem_read(uc.reg_read(UC_M68K_REG_A7) + 4, 20))
            print(f'bgchip gf {gf} chip {(a[1] - 0xFF0000) & 0xffffff:#x} layer {a[2]} x {a[3]} y {a[4]}')
        if drawlog and addr in (sym.get('h_xsp_set'), sym.get('h_xobj_set')):
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            if drawlog[0] <= gf <= drawlog[1]:
                sp = uc.reg_read(UC_M68K_REG_A7)
                ret = struct.unpack('>I', uc.mem_read(sp, 4))[0]
                if sym.get('h_xsp_set_obj', 0) <= ret < sym.get('h_xsp_set', 0):
                    ret = struct.unpack('>I', uc.mem_read(sp + 16, 4))[0]
                r = [uc.reg_read(x) & 0xFFFF for x in (UC_M68K_REG_D0, UC_M68K_REG_D1, UC_M68K_REG_D2, UC_M68K_REG_D3)]
                r = [v - 0x10000 if v & 0x8000 else v for v in r[:2]] + r[2:]
                print(f'draw gf {gf} {fnname(ret - 0x10000)} {"pcg" if addr == sym.get("h_xsp_set") else "ref"} '
                      f'x {r[0] / 64:.1f} y {r[1] / 64:.1f} id {r[2]:#x} info {r[3]:#06x}')
        if addr in fnlog:
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            ret = struct.unpack('>I', uc.mem_read(uc.reg_read(UC_M68K_REG_A7), 4))[0]
            print(f'fnlog gf {gf} fn_{addr - 0x10000:05x} from {fnname(ret - 0x10000)} ret {ret - 0x10000:#x}')
        if addr in (sym.get('h_xsp_set'), sym.get('h_xobj_set')) and pcghist is not None:
            sp = uc.reg_read(UC_M68K_REG_A7)
            ret = struct.unpack('>I', uc.mem_read(sp, 4))[0]
            if sym.get('h_xsp_set_obj', 0) <= ret < sym.get('h_xsp_set', 0):   # via xsp_set_obj
                ret = struct.unpack('>I', uc.mem_read(sp + 16, 4))[0]
            kind = 'pcg' if addr == sym.get('h_xsp_set') else 'ref'
            k = (fnname(ret - 0x10000), kind, uc.reg_read(UC_M68K_REG_D2) & 0xFFFF)
            pcghist[k] += 1
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            sp_ = pcgspan.setdefault(k, [gf, gf]); sp_[1] = gf
        if span:
            if addr == span[0]:
                state['span0'] = state['n']
                state['sh'] = collections.Counter()
            elif addr == span[1] and 'span0' in state:
                ln = state['n'] - state.pop('span0')
                spans.append(ln)
                if ln > state.get('spanmax', 0):
                    state['spanmax'] = ln
                    state['spanhist'] = state['sh']
                if 3500 < ln < 5000:
                    state['spanmid'] = state['sh']
                state.setdefault('spanall', collections.Counter()).update(state['sh'])
            elif 'span0' in state:
                state['sh'][name(addr).split('+')[0]] += 1
        if addr == sym.get('x_bg_chip'):
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            chipcount[gf // 50] += 1
            if chipcount[gf // 50] in (100, 101, 102, 2000, 2001):
                sp = uc.reg_read(UC_M68K_REG_A7)
                ret, a1, a2, a3, a4, a5 = struct.unpack('>IIIIII', uc.mem_read(sp, 24))
                print('chip gf', gf, 'ret', name(ret), 'chip', hex(a2), 'layer', a3, 'x', a4, 'y', a5,
                      'B_90462', hex(struct.unpack('>H', uc.mem_read(0xFF749A + 0x90462 - 0x90252, 2))[0]),
                      'stack', uc.mem_read(sp, 40).hex())
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
        # control transfers done by hand: stop here (before this instruction
        # runs) and let the outer loop change PC and restart
        if addr == rte_addr:               # rte of the VBlank handler
            state['jump'] = ('rte', addr)
            uc.emu_stop()
            return
        if state['n'] % period == 0:
            state['pend'] = True
        # (unicorn evaluates the condition codes lazily, so SR read here can be
        # stale: interrupts are only taken where the program waits for them)
        if state.get('pend') and not state.get('in_int') and (vs_lo <= addr < vs_hi or wait_lo <= addr < wait_hi):
            sr = uc.reg_read(UC_M68K_REG_SR)
            if (sr >> 8) & 7 < 6:          # take the level-6 interrupt
                state['pend'] = False
                state['jump'] = ('int', addr)
                uc.emu_stop()
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
            if bombs and any(b <= gf < b + 6 for b in bombs):     # --bomb F,F..: B pressed at these game frames
                p |= 0x10
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
    if '--watch' in sys.argv:
        wa = int(sys.argv[sys.argv.index('--watch') + 1], 16)
        def watch(uc, access, addr, size, value, ud):
            pc = uc.reg_read(UC_M68K_REG_PC)
            gf = struct.unpack('>H', uc.mem_read(0xFF0116, 2))[0]
            if state.get('wn', 0) < int(os.environ.get('WATCH_N', 30)):
                state['wn'] = state.get('wn', 0) + 1
                print(f'watch write {addr:#x} = {value:#x} size {size} pc {name(pc)} gf {gf} last', [name(a) for a in list(last)[-6:]])
        uc.hook_add(UC_HOOK_MEM_WRITE, watch, begin=wa, end=wa + 1)
    sp, pc = struct.unpack('>II', data[:8])
    uc.reg_write(UC_M68K_REG_A7, sp)
    uc.reg_write(UC_M68K_REG_SR, 0x2700)
    try:
        while state['n'] < maxi:
            state.pop('jump', None)
            uc.emu_start(pc, 0x7FFFFFFF, count=maxi - state['n'])
            j = state.get('jump')
            if not j:
                break
            kind, at = j
            real = uc.reg_read(UC_M68K_REG_PC)
            if state.get('dbg', 0) < 6:
                state['dbg'] = state.get('dbg', 0) + 1
                print('jump', kind, 'hooked', name(at), 'pc after stop', name(real))
            at = real
            if kind == 'rte' and at != rte_addr:
                pc = at
                continue
            if kind == 'rte':
                sp = uc.reg_read(UC_M68K_REG_A7)
                sr, pc = struct.unpack('>HI', uc.mem_read(sp, 6))
                uc.reg_write(UC_M68K_REG_A7, sp + 6)
                uc.reg_write(UC_M68K_REG_SR, sr)
                state['in_int'] = False
            else:
                sr = uc.reg_read(UC_M68K_REG_SR)
                sp = uc.reg_read(UC_M68K_REG_A7) - 6
                uc.mem_write(sp, struct.pack('>HI', sr, at))
                uc.reg_write(UC_M68K_REG_A7, sp)
                uc.reg_write(UC_M68K_REG_SR, (sr & 0xF8FF) | 0x2600)
                pc = vint
                state['in_int'] = True
                state['vbl'] += 1
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
    for v in ('stat_rec', 'stat_calls', 'stat_pieces', 'stat_out', 'xsp_misses', 'xsp_fail', 'comp_peak', 'comp_fail', 'plane_frames', 'plane_fx'):
        if v in sym:
            w = 2 if v.startswith(('xsp_', 'comp_', 'plane_')) else 4
            print(v, int.from_bytes(uc.mem_read(sym[v], w), 'big'))
    if explc:
        h = collections.Counter(explc.values())
        print('expl4 explosions drawn per game frame (count: frames):', sorted(h.items()))
        print('frames with >= 3:', sorted(f for f, n in explc.items() if n >= 3)[:60])
    if chipcount:
        print('BG chips per 50 game frames:', sorted(chipcount.items())[:40])
    if pcghist:
        agg = collections.defaultdict(lambda: [0, set()])
        for (f, k, v), c in pcghist.items():
            agg[(f, k)][0] += c; agg[(f, k)][1].add(v)
        for (f, k), (c, vs) in sorted(agg.items(), key=lambda t: -t[1][0])[:40]:
            print(f'  {f} {k} {c:6d} ids {min(vs):#x}-{max(vs):#x}')
        if '--calls-dump' in sys.argv:
            with open(sys.argv[sys.argv.index('--calls-dump') + 1], 'w') as f:
                for (fn, k, v), c in sorted(pcghist.items()):
                    a, b = pcgspan[(fn, k, v)]
                    f.write(f'{fn} {k} {v:#x} {c} {a} {b}\n')
    if spans:
        spans.sort()
        print('biggest span:', state['spanhist'].most_common(12))
        print('all spans:', state['spanall'].most_common(12))
        if 'spanmid' in state:
            print('a 3500-5000 span:', state['spanmid'].most_common(12))
        print('span calls', len(spans), 'median', spans[len(spans) // 2], 'p90', spans[len(spans) * 9 // 10],
              'max', spans[-1], 'top', spans[-10:])
    if 'xsp_misses' in sym:
        print('cache misses', struct.unpack('>H', uc.mem_read(sym['xsp_misses'], 2))[0])
    print('stopped after', state['n'], 'insns,', state['vbl'], 'vblanks, pc', name(uc.reg_read(UC_M68K_REG_PC)))

if __name__ == '__main__':
    main()
