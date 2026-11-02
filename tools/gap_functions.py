"""Genera entradas 'functions' del TOML para switches en huecos sin .pdata.
Inicio = ultimo destino de bl (o inicio del hueco) antes del switch; fin = siguiente destino de bl
tras el ultimo caso (o fin del hueco). Lee los switches de config/gh6_switch_tables.toml.
python -I gap_functions.py basefile.bin functions_pdata.txt switch_tables.toml recomp.log"""
import struct, bisect, re, sys
img = open(sys.argv[1], "rb").read(); B = 0x82000000
fn = [(int(a, 16), int(b, 16)) for a, b in (l.split() for l in open(sys.argv[2]))]
st = [f[0] for f in fn]
w = lambda a: struct.unpack_from(">I", img, a - B)[0]
T0, T1 = 0x820D0000, 0x820D0000 + 0x6EE6A4
bl = set()
for a in range(T0, T1, 4):
    x = w(a)
    if x >> 26 == 18 and x & 3 == 1:
        d = x & 0x3FFFFFC
        if d & 0x2000000: d -= 0x4000000
        bl.add(a + d)
bl = sorted(bl)
# switches: base -> labels
sw = {}; cur = None
for l in open(sys.argv[3]):
    m = re.match(r"base = 0x(\w+)", l)
    if m: cur = int(m.group(1), 16); sw[cur] = []
    m = re.match(r"\s+0x(\w+),", l)
    if m and cur is not None: sw[cur].append(int(m.group(1), 16))
bad = set(int(m.group(1), 16) for m in (re.match(r"ERROR: Switch case at (\w+)", l) for l in open(sys.argv[4])) if m)
out = {}
for a in sorted(bad):
    i = bisect.bisect_right(st, a) - 1
    s, e = fn[i][0] + fn[i][1], fn[i + 1][0]
    if not (s <= a < e): continue
    while w(s) == 0: s += 4
    while w(e - 4) == 0: e -= 4
    j = bisect.bisect_right(bl, a) - 1
    if j >= 0 and bl[j] > s: s = bl[j]
    hi = max(sw.get(a, [a]))
    k = bisect.bisect_right(bl, hi)
    if k < len(bl) and bl[k] < e: e = bl[k]
    out[s] = max(out.get(s, 0), e - s)
print("functions = [")
for s in sorted(out): print("    { address = 0x%08X, size = 0x%X }," % (s, out[s]))
print("]")
