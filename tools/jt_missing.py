"""Lista bctr con carga indexada previa que no estan cubiertos por config/gh6_switch_tables.toml."""
import struct, re, collections, sys
R = "G:/Guitar Hero Warrior of rock/Recomp/"
img = open(R + "xex/basefile.bin", "rb").read(); B = 0x82000000
T0, N = 0xD0000, 0x6EE6A4 // 4
ws = struct.unpack_from(">%dI" % N, img, T0)
bases = [int(m, 16) for m in re.findall(r"base = 0x(\w+)", open(R + "config/gh6_switch_tables.toml").read())]
covered = set()
for b in bases:
    for k in range(0, 0x40, 4): covered.add(b + k)
def m(x):
    op = x >> 26
    if op == 15: return "lis" if ((x >> 16) & 31) == 0 else "addis"
    if op == 14: return "li" if ((x >> 16) & 31) == 0 else "addi"
    if op == 21: return "rlwinm"
    if op == 24: return "nop" if x == 0x60000000 else "ori"
    if op == 10: return "cmplwi"
    if op == 16: return "bc"
    if op == 18: return "b"
    if op == 31:
        xo = (x >> 1) & 0x3FF
        return {23: "lwzx", 279: "lhzx", 87: "lbzx", 343: "lhax", 266: "add", 467: "mtctr", 444: "mr"}.get(xo, "x%d" % xo)
    return {32: "lwz", 40: "lhz", 34: "lbz"}.get(op, "op%d" % op)
c = collections.Counter(); ex = {}
for i, x in enumerate(ws):
    if x != 0x4E800420: continue
    a = B + T0 + i * 4
    if a in covered: continue
    seq = tuple(m(y) for y in ws[i - 9:i])
    if not any(s in seq for s in ("lwzx", "lhzx", "lbzx", "lhax")): continue
    c[seq[-6:]] += 1; ex.setdefault(seq[-6:], []).append(a)
tot = 0
for s, n in c.most_common():
    tot += n; print(n, " ".join(s), "bctr  ej:", " ".join("0x%08X" % a for a in ex[s][:3]))
print("total sin cubrir:", tot)
