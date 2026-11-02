"""Desensamblador de microcodigo de shaders Xenos (Xbox 360) - sesion 7.
Formato segun xenia/src/xenia/gpu/ucode.h (BSD). Entrada: .bin capturados por el runtime (dwords BE).
Uso: python -I xenos_dis.py shader.bin [...]   (o un directorio)
Es la base del futuro traductor Xenos -> HLSL.
"""
import struct, sys, os

CF = ["nop", "exec", "exec_end", "cexec", "cexec_end", "cexec_pred", "cexec_pred_end", "loop_start", "loop_end",
      "ccall", "ret", "cjmp", "alloc", "cexec_pred_clean", "cexec_pred_clean_end", "mark_vs_fetch_done"]
VEC = ["add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad", "cndeq", "cndge", "cndgt",
       "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push",
       "kill_eq", "kill_gt", "kill_ge", "kill_ne", "dst", "maxa"]
SCA = {0: "adds", 1: "adds_prev", 2: "muls", 3: "muls_prev", 4: "muls_prev2", 5: "maxs", 6: "mins", 7: "seqs", 8: "sgts",
       9: "sges", 10: "snes", 11: "frcs", 12: "truncs", 13: "floors", 14: "exp", 15: "logc", 16: "log", 17: "rcpc", 18: "rcpf",
       19: "rcp", 20: "rsqc", 21: "rsqf", 22: "rsq", 23: "maxas", 24: "maxasf", 25: "subs", 26: "subs_prev", 27: "setp_eq",
       28: "setp_ne", 29: "setp_gt", 30: "setp_ge", 31: "setp_inv", 32: "setp_pop", 33: "setp_clr", 34: "setp_rstr",
       35: "kills_eq", 36: "kills_gt", 37: "kills_ge", 38: "kills_ne", 39: "kills_one", 40: "sqrt", 42: "mulsc0", 43: "mulsc1",
       44: "addsc0", 45: "addsc1", 46: "subsc0", 47: "subsc1", 48: "sin", 49: "cos", 50: "retain_prev"}
# numero de operandos de cada opcode vectorial
VEC_SRCS = {8: 1, 9: 1, 10: 1, 11: 3, 12: 3, 13: 3, 14: 3, 17: 3, 19: 1, 29: 2}
XYZW = "xyzw"


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def swz(s, n=4):
    # swizzle relativo a componente: (s >> 2i) + i
    out = "".join(XYZW[(bits(s, 2 * i, 2) + i) & 3] for i in range(4))
    return "" if out == "xyzw" else "." + out[:n] if n < 4 else ("." + out)


def mask(m, export=False):
    return "".join(XYZW[i] if m & (1 << i) else "_" for i in range(4))


def dest_name(reg, export, is_vs):
    if not export:
        return "r%d" % reg
    if is_vs:
        if reg == 62: return "oPos"
        if reg == 63: return "oPts"
        return "o%d" % reg       # interpolador
    if reg < 4: return "oC%d" % reg
    if reg == 61: return "oDepth"
    return "oE%d" % reg


def src_name(reg, sel, neg, swiz_bits, absc):
    if sel:  # temporal
        s = "r%d" % (reg & 0x3F)
        if reg & 0x80: s = "|%s|" % s
    else:
        s = "c%d" % reg
        if absc: s = "|%s|" % s
    return ("-" if neg else "") + s + swz(swiz_bits)


def dis_alu(w, is_vs):
    w0, w1, w2 = w
    vdst, sdst = bits(w0, 0, 6), bits(w0, 8, 6)
    export = bits(w0, 15, 1)
    vmask, smask = bits(w0, 16, 4), bits(w0, 20, 4)
    sop = bits(w0, 26, 6)
    absc = bits(w0, 7, 1)
    vop = bits(w2, 24, 5)
    srcs = []
    for i, (rl, sl, swl, negb) in enumerate(((16, 31, 16, 26), (8, 30, 8, 25), (0, 29, 0, 24))):
        srcs.append(src_name(bits(w2, rl, 8), bits(w2, sl, 1), bits(w1, negb, 1), bits(w1, swl, 8), absc))
    pred = ""
    if bits(w1, 28, 1):
        pred = "(p%s) " % ("" if bits(w1, 27, 1) else "!")
    out = []
    if vmask or not smask:
        n = VEC_SRCS.get(vop, 2)
        name = VEC[vop] if vop < len(VEC) else "vop%d" % vop
        out.append("%s%s %s.%s, %s%s" % (pred, name, dest_name(vdst, export, is_vs), mask(vmask), ", ".join(srcs[:n]),
                                         " (sat)" if bits(w0, 24, 1) else ""))
    if smask or sop not in (50,):
        if smask:
            name = SCA.get(sop, "sop%d" % sop)
            sd = dest_name(vdst if export else sdst, export, is_vs)
            out.append("%s+ %s %s.%s, %s" % (pred, name, sd, mask(smask), srcs[2]))
    return " ; ".join(out) if out else "nop_alu"


def dis_fetch(w):
    w0, w1, w2 = w
    op = bits(w0, 0, 5)
    src, dst, cidx = bits(w0, 5, 6), bits(w0, 12, 6), bits(w0, 20, 5)
    dswz = bits(w1, 0, 12)
    comps = "".join("xyzw01?_"[bits(dswz, 3 * i, 3)] for i in range(4))
    if op == 0:
        fmt, stride, off = bits(w1, 16, 6), bits(w2, 0, 8), bits(w2, 8, 23)
        mini = bits(w1, 30, 1)
        return "%s r%d.%s, r%d, vf%d (fmt %d, stride %d, off %d)" % ("vfetch_mini" if mini else "vfetch", dst, comps, src,
                                                                     cidx * 3 + bits(w0, 25, 2), fmt, stride * 4, off * 4)
    if op == 1:
        return "tfetch r%d.%s, r%d, tf%d" % (dst, comps, src, cidx)
    return "fetch_op%d r%d, r%d, tf%d" % (op, dst, src, cidx)


def disassemble(words, is_vs):
    lines = []
    cfs = []
    # control de flujo: 3 dwords -> 2 instrucciones de 48 bits; termina con *_end
    i = 0
    end = False
    while not end and i + 3 <= len(words):
        d0, d1, d2 = words[i:i + 3]
        pair = [(d0, d1 & 0xFFFF), ((d1 >> 16) | ((d2 << 16) & 0xFFFFFFFF), d2 >> 16)]
        for a, b in pair:
            op = bits(b, 12, 4)
            cfs.append((op, a, b))
            if op in (2, 4, 6, 14):
                end = True
        i += 3
    for n, (op, a, b) in enumerate(cfs):
        name = CF[op]
        if op in (1, 2, 3, 4, 5, 6, 13, 14):
            addr, count, seq = bits(a, 0, 12), bits(a, 12, 3), bits(a, 16, 12)
            extra = ""
            if op in (3, 4):
                extra = " b%d==%d" % (bits(b, 2, 8), bits(b, 10, 1))
            elif op in (5, 6, 13, 14):
                extra = " p==%d" % bits(b, 10, 1)
            lines.append("cf%-3d %s addr=%d count=%d%s" % (n, name, addr, count, extra))
            for k in range(count):
                iw = words[(addr + k) * 3:(addr + k) * 3 + 3]
                if len(iw) < 3:
                    lines.append("      !! fuera de rango"); break
                is_fetch = bits(seq, 2 * k, 1)
                lines.append("      %3d  %s" % (addr + k, dis_fetch(iw) if is_fetch else dis_alu(iw, is_vs)))
        elif op == 12:
            sizes = {1: "position", 2: "interpolators", 3: "colors/memexport"}
            lines.append("cf%-3d alloc %s size=%d" % (n, sizes.get(bits(b, 9, 2) if False else bits(b, 8, 2), "?"), bits(a, 0, 3) + 1))
        elif op == 0:
            lines.append("cf%-3d nop" % n)
        else:
            lines.append("cf%-3d %s a=%08X b=%04X" % (n, name, a, b))
    return lines


def main():
    paths = []
    for p in sys.argv[1:]:
        if os.path.isdir(p):
            paths += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.endswith(".bin")]
        else:
            paths.append(p)
    for p in paths:
        data = open(p, "rb").read()
        words = list(struct.unpack(">%dI" % (len(data) // 4), data))
        is_vs = os.path.basename(p).startswith("vs")
        print("==== %s (%d dwords)" % (os.path.basename(p), len(words)))
        for l in disassemble(words, is_vs):
            print(l)
        print()


if __name__ == "__main__":
    main()
