#!/usr/bin/env python3
"""Call-graph helper over a dis68 listing: cg.py LIST FUNC [depth]"""
import sys, re
lines = open(sys.argv[1]).read().split('\n')
funcs = {}; cur = None; order = []
for ln in lines:
    m = re.match(r'^(fn_[0-9a-f]+|start):', ln)
    if m:
        cur = m.group(1); funcs[cur] = []; order.append(cur); continue
    if cur: funcs[cur].append(ln)
def callees(f):
    out = []
    for ln in funcs.get(f, []):
        m = re.search(r'(?:jsr|bsr\.\w|lea\.l|pea\.l|move\.l)\s+#?(fn_[0-9a-f]+)', ln)
        if m and m.group(1) not in out: out.append(m.group(1))
    return out
def size(f): return sum(1 for l in funcs.get(f, []) if re.match(r'  [0-9a-f]{5}  ', l))
def walk(f, d, maxd, seen):
    print('  ' * d + f'{f} ({size(f)})')
    if d >= maxd or f in seen: return
    seen.add(f)
    for c in callees(f): walk(c, d + 1, maxd, seen)
walk(sys.argv[2], 0, int(sys.argv[3]) if len(sys.argv) > 3 else 2, set())
