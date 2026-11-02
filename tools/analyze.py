"""Analisis: funciones de .pdata y candidatos setjmp/longjmp. python -I analyze.py basefile.bin outdir"""
import struct, sys, os
img = open(sys.argv[1], "rb").read(); out = sys.argv[2]
BASE = 0x82000000
P0, PSZ = 0x9C400, 0x257B8
funcs = []
for i in range(PSZ // 8):
    beg, data = struct.unpack_from(">II", img, P0 + i * 8)
    if beg == 0: continue
    length = ((data >> 8) & 0x3FFFFF) * 4
    funcs.append((beg, length))
with open(os.path.join(out, "functions_pdata.txt"), "w") as f:
    for b, l in funcs: f.write("0x%08X\t0x%X\n" % (b, l))
print("funciones .pdata:", len(funcs), "primera 0x%08X ultima 0x%08X" % (funcs[0][0], funcs[-1][0]))
def w(a): return struct.unpack_from(">I", img, a - BASE)[0]
# setjmp: muchas std rN,d(r3) (r14..r31) ; longjmp: muchas ld rN,d(r3)
for b, l in funcs:
    ws = [w(b + k) for k in range(0, min(l, 0x200), 4)]
    std = sum(1 for x in ws if (x >> 26) == 62 and ((x >> 16) & 31) == 3 and ((x >> 21) & 31) >= 14 and (x & 3) == 0)
    ld = sum(1 for x in ws if (x >> 26) == 58 and ((x >> 16) & 31) == 3 and ((x >> 21) & 31) >= 14 and (x & 3) == 0)
    if std >= 18: print("setjmp cand 0x%08X len=0x%X std=%d" % (b, l, std))
    if ld >= 18: print("longjmp cand 0x%08X len=0x%X ld=%d" % (b, l, ld))
