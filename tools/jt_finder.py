"""Detector de jump tables para GH:WoR (complementa a XenonAnalyse).
Reconoce, ignorando nops y con seguimiento de registros hacia atras desde cada bctr:
  ABS : lis/addi rT ; rlwinm rI,rIdx,2 ; lwzx rC,rT,rI ; mtctr rC ; bctr          -> labels = u32[i]
  SHRT: lis/addi rT ; rlwinm rI,rIdx,1 ; lhzx r0,rT,rI ; lis/addi rB ; add rC,rB,r0 -> base + u16[i]
  BYTE: lis/addi rT ; lbzx r0,rT,rIdx [; rlwinm r0,r0,s] ; lis/addi rB ; add       -> base + (u8[i] << s)
Limite: cmplwi crN,rIdx,K + bc gt (bgt -> default=destino; ble -> default=siguiente instruccion),
buscado hacia atras desde el bloque o desde los saltos (b/bc) que llegan a el.
Uso: python -I jt_finder.py SALIDA.toml  (anade solo los bctr no cubiertos por config/gh6_switch_tables.toml)
"""
import struct, re, sys
R = "G:/Guitar Hero Warrior of rock/Recomp/"
img = open(R + "xex/basefile.bin", "rb").read(); B = 0x82000000
T0, T1 = 0x820D0000, 0x820D0000 + 0x6EE6A4
w = lambda a: struct.unpack_from(">I", img, a - B)[0]
OP = lambda x: x >> 26
RT = lambda x: (x >> 21) & 31
RA = lambda x: (x >> 16) & 31
RB = lambda x: (x >> 11) & 31
XO = lambda x: (x >> 1) & 0x3FF
def simm(x): v = x & 0xFFFF; return v - 0x10000 if v & 0x8000 else v
def is_nop(x): return x == 0x60000000
def is_mtctr(x): return OP(x) == 31 and XO(x) == 467 and ((RA(x) | (RB(x) << 5)) == 9)

def window(bctr, n=14):
    out = []; a = bctr - 4
    while len(out) < n and a > bctr - 4 * 40:
        x = w(a)
        if not is_nop(x): out.append((a, x))
        a -= 4
    return out  # de mas cercano a mas lejano

def find_def(win, reg, start=0):
    """indice en win de la ultima instruccion (mas cercana al bctr) que escribe reg, a partir de start"""
    for i in range(start, len(win)):
        a, x = win[i]
        op = OP(x)
        if op in (14, 15, 32, 34, 40) and RT(x) == reg: return i
        if op in (21, 24) and RA(x) == reg: return i
        if op == 31 and XO(x) in (23, 279, 87, 266) and RT(x) == reg: return i
        if op == 31 and XO(x) == 444 and RA(x) == reg: return i
    return None

def const_value(win, reg, start):
    """valor de reg construido con lis/addi/ori (o None)"""
    i = find_def(win, reg, start)
    if i is None: return None, None
    a, x = win[i]
    if OP(x) == 14 and RA(x) == reg:  # addi reg,reg,lo
        hi, j = const_value(win, reg, i + 1)
        return (None, None) if hi is None else ((hi + simm(x)) & 0xFFFFFFFF, j)
    if OP(x) == 24 and RT(x) == reg:  # ori reg,reg,lo
        hi, j = const_value(win, reg, i + 1)
        return (None, None) if hi is None else ((hi | (x & 0xFFFF)), j)
    if OP(x) == 15 and RA(x) == 0:    # lis
        return ((x & 0xFFFF) << 16) & 0xFFFFFFFF, i
    return None, None

def parse(bctr):
    win = window(bctr)
    if not win or not is_mtctr(win[0][1]): return None
    rc = RT(win[0][1])
    i = find_def(win, rc, 1)
    if i is None: return None
    a, x = win[i]
    first = [a]
    if OP(x) == 31 and XO(x) == 23:  # lwzx rc, rT, rI
        kind, rt_, ri = "ABS", RA(x), RB(x)
        tbl, j = const_value(win, rt_, i + 1)
        k = find_def(win, ri, i + 1)
        if tbl is None or k is None: return None
        xa, xx = win[k]
        if OP(xx) != 21 or RB(xx) != 2: return None
        ridx = RT(xx); first += [win[j][0], xa]
        count_hint = None
        return dict(kind=kind, table=tbl, ridx=ridx, start=min(first), elem=4, shift=0, base=0)
    if OP(x) == 31 and XO(x) == 266:  # add rc, rB, r0
        rA, rB_ = RA(x), RB(x)
        for rbase, roff in ((rA, rB_), (rB_, rA)):
            base, jb = const_value(win, rbase, i + 1)
            if base is None: continue
            k = find_def(win, roff, i + 1)
            if k is None: continue
            ka, kx = win[k]; shift = 0
            if OP(kx) == 21 and RT(kx) == roff and RA(kx) == roff:  # rlwinm r0,r0,s (tras lbzx)
                shift = RB(kx)
                k = find_def(win, roff, k + 1)
                if k is None: continue
                ka, kx = win[k]
            if OP(kx) != 31: continue
            if XO(kx) == 279:  # lhzx r0, rT, rI
                tbl, jt = const_value(win, RA(kx), k + 1)
                m = find_def(win, RB(kx), k + 1)
                if tbl is None or m is None or OP(win[m][1]) != 21 or RB(win[m][1]) != 1: continue
                return dict(kind="SHRT", table=tbl, ridx=RT(win[m][1]), start=min(win[jt][0], win[m][0], win[jb][0]), elem=2, shift=0, base=base)
            if XO(kx) == 87:   # lbzx r0, rT, rIdx
                tbl, jt = const_value(win, RA(kx), k + 1)
                if tbl is None: continue
                return dict(kind="BYTE", table=tbl, ridx=RB(kx), start=min(win[jt][0], win[jb][0]), elem=1, shift=shift, base=base)
    return None

def bounds_from(addr, ridx, limit=48):
    """busca hacia atras desde addr: bc sobre gt de crN y cmplwi crN,ridx,K"""
    bc = None
    regs = {ridx}
    for k in range(1, limit + 1):  # alias por mr (pueden estar antes del cmplwi)
        x = w(addr - 4 * k)
        if OP(x) == 31 and XO(x) == 444 and RT(x) == RB(x) and RA(x) in regs: regs.add(RT(x))
    a = addr - 4
    for _ in range(limit):
        x = w(a)
        if OP(x) == 31 and XO(x) == 444 and RT(x) == RB(x) and RA(x) in regs:  # mr ridx, rS
            regs.add(RT(x))
        if bc is None and OP(x) == 16 and (RA(x) & 3) == 1:  # bc sobre bit gt
            bo = RT(x)
            tgt = a + (simm(x) & ~3)
            bc = (a, RA(x) >> 2, bo, tgt)
        elif bc is not None and OP(x) == 10 and RA(x) in regs and (RT(x) >> 2) == bc[1]:
            K = x & 0xFFFF
            ba, crn, bo, tgt = bc
            if bo == 12: default = tgt          # bgt default
            elif bo == 4: default = ba + 4      # ble tabla -> default = siguiente
            else: return None
            return K + 1, default
        elif OP(x) == 18 and not (x & 1) and bc is None and a != addr - 4:
            return None  # otro b incondicional en medio: no seguir
        a -= 4
    return None

_branch_index = None
def branches_to(target):
    global _branch_index
    if _branch_index is None:
        _branch_index = {}
        for a in range(T0, T1, 4):
            x = w(a)
            if OP(x) == 18 and not (x & 1):
                li = x & 0x3FFFFFC; li = li - 0x4000000 if li & 0x2000000 else li
                _branch_index.setdefault(a + li, []).append(a)
            elif OP(x) == 16 and not (x & 1):
                _branch_index.setdefault(a + (simm(x) & ~3), []).append(a)
    return _branch_index.get(target, [])

def main():
    out_path = sys.argv[1]
    existing = open(R + "config/gh6_switch_tables.toml").read()
    covered = set()
    for b in re.findall(r"base = 0x(\w+)", existing):
        for k in range(0, 0x40, 4): covered.add(int(b, 16) + k)
    # destinos de tablas existentes: label -> (registro, numero, default)
    label_of = {}
    for blk in existing.split("[[switch]]")[1:]:
        r_ = int(re.search(r"r = (\d+)", blk).group(1)); d_ = int(re.search(r"default = 0x(\w+)", blk).group(1), 16)
        ls = [int(v, 16) for v in re.findall(r"^\s+0x(\w+),", blk, re.M)]
        for l in ls: label_of.setdefault(l, (r_, len(ls), d_))
    res, fails = [], []
    pending = []
    for a in range(T0, T1, 4):
        if w(a) != 0x4E800420 or a in covered: continue
        p = parse(a)
        if p is None: continue
        bd = bounds_from(p["start"], p["ridx"])
        if bd is None:
            for br in branches_to(p["start"]):
                bd = bounds_from(br + 4, p["ridx"]) if OP(w(br)) == 18 else None
                if bd: break
        if bd is None: pending.append((a, p)); continue
        emit(res, fails, label_of, p, a, bd)
    # segunda pasada: bloques que son destino de otra tabla sobre el mismo registro (switch anidado)
    for _ in range(3):
        still = []
        for a, p in pending:
            lo = label_of.get(p["start"])
            if lo and lo[0] == p["ridx"]: emit(res, fails, label_of, p, a, (lo[1], lo[2]))
            else: still.append((a, p))
        pending = still
    # plan B (sesion 10): tabla ABS en linea justo detras del bctr -> contar entradas que apuntan dentro de la funcion
    fn = [(int(x, 16), int(y, 16)) for x, y in (l.split() for l in open(R + "xex/functions_pdata.txt"))]
    import bisect
    starts = [f[0] for f in fn]
    still = []
    for a, p in pending:
        if p["kind"] == "ABS" and p["table"] == a + 4:
            i = bisect.bisect_right(starts, a) - 1
            lo, hi = fn[i][0], fn[i][0] + fn[i][1]
            cnt = 0
            while cnt < 256:
                v = w(p["table"] + cnt * 4)
                if not (lo <= v < hi) or v & 3: break
                cnt += 1
            if cnt >= 2:
                emit(res, fails, label_of, p, a, (cnt, w(p["table"])))
                print("  plan B 0x%08X: %d entradas" % (a, cnt))
                continue
        still.append((a, p))
    pending = still
    fails += [(a, p["kind"]) for a, p in pending]
    write(out_path, res, fails)

def emit(res, fails, label_of, p, a, bd):
        count, default = bd
        labels = []
        for i in range(count):
            ea = p["table"] + i * p["elem"] - B
            if p["kind"] == "ABS": labels.append(struct.unpack_from(">I", img, ea)[0])
            elif p["kind"] == "SHRT": labels.append(p["base"] + struct.unpack_from(">H", img, ea)[0])
            else: labels.append(p["base"] + (img[ea] << p["shift"]))
        if not all(T0 <= l < T1 for l in labels): fails.append((a, p["kind"] + "(labels fuera)")); return
        res.append((p, a, default, labels))
        for l in labels: label_of.setdefault(l, (p["ridx"], count, default))

def write(out_path, res, fails):
    with open(out_path, "w") as f:
        f.write("# Generado por tools/jt_finder.py\n")
        for p, a, default, labels in res:
            f.write("[[switch]]\nbase = 0x%X\nr = %d\ndefault = 0x%X\nlabels = [\n" % (p["start"], p["ridx"], default))
            for l in labels: f.write("    0x%X,\n" % l)
            f.write("]\n\n")
    kinds = {}
    for p, *_ in res: kinds[p["kind"]] = kinds.get(p["kind"], 0) + 1
    print("nuevas:", len(res), kinds, "fallidas:", len(fails))
    for a, k in fails: print("  sin resolver 0x%08X %s" % (a, k))

if __name__ == "__main__":
    main()
