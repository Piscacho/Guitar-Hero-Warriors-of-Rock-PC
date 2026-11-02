"""Desensamblador PPC minimo para inspeccion (subset)."""
import struct
def dis(x, a):
    op = x >> 26; rt = (x >> 21) & 31; ra = (x >> 16) & 31; rb = (x >> 11) & 31
    si = x & 0xFFFF; si = si - 0x10000 if si & 0x8000 else si
    if x == 0x60000000: return "nop"
    if x == 0x4E800420: return "bctr"
    if x == 0x4E800020: return "blr"
    if op == 15: return ("lis r%d,0x%X" % (rt, x & 0xFFFF)) if ra == 0 else "addis r%d,r%d,0x%X" % (rt, ra, x & 0xFFFF)
    if op == 14: return ("li r%d,%d" % (rt, si)) if ra == 0 else "addi r%d,r%d,%d" % (rt, ra, si)
    if op == 24: return "ori r%d,r%d,0x%X" % (ra, rt, x & 0xFFFF)
    if op == 21: return "rlwinm r%d,r%d,%d,%d,%d" % (ra, rt, rb, (x >> 6) & 31, (x >> 1) & 31)
    if op == 10: return "cmplwi cr%d,r%d,%d" % (rt >> 2, ra, x & 0xFFFF)
    if op == 11: return "cmpwi cr%d,r%d,%d" % (rt >> 2, ra, si)
    if op == 16:
        bd = x & 0xFFFC; bd = bd - 0x10000 if bd & 0x8000 else bd
        return "bc %d,%d,0x%08X" % (rt, ra, a + bd)
    if op == 18:
        li = x & 0x3FFFFFC; li = li - 0x4000000 if li & 0x2000000 else li
        return ("bl" if x & 1 else "b") + " 0x%08X" % (a + li)
    if op in (32, 40, 34, 36, 44, 38): return "%s r%d,%d(r%d)" % ({32:"lwz",40:"lhz",34:"lbz",36:"stw",44:"sth",38:"stb"}[op], rt, si, ra)
    if op == 31:
        xo = (x >> 1) & 0x3FF
        n = {23:"lwzx",279:"lhzx",87:"lbzx",343:"lhax",266:"add",235:"mullw",40:"subf"}.get(xo)
        if n: return "%s r%d,r%d,r%d" % (n, rt, ra, rb)
        if xo == 467: return "mtspr %d,r%d" % (ra | (rb << 5), rt)
        if xo == 444: return "or r%d,r%d,r%d" % (ra, rt, rb)
        if xo == 954: return "extsb r%d,r%d" % (ra, rt)
        return "x31_%d" % xo
    return "op%d %08X" % (op, x)
