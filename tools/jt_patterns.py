"""Agrupa las secuencias previas a bctr para ver patrones de jump tables."""
import struct, collections
img=open("G:/Guitar Hero Warrior of rock/Recomp/xex/basefile.bin","rb").read();B=0x82000000
ws=struct.unpack_from(">%dI"%(0x6EE6A4//4),img,0xD0000)
def m(x):
    op=x>>26
    if op==15: return "lis" if ((x>>16)&31)==0 else "addis"
    if op==14: return "li" if ((x>>16)&31)==0 else "addi"
    if op==21: return "rlwinm"
    if op==32: return "lwz"
    if op==40: return "lhz"
    if op==34: return "lbz"
    if op==10: return "cmplwi"
    if op==16: return "bc"
    if op==31:
        xo=(x>>1)&0x3FF
        return {23:"lwzx",279:"lhzx",87:"lbzx",343:"lhax",266:"add",467:"mtspr",922:"extsh",954:"extsb",444:"or",24:"slw"}.get(xo,"x31_%d"%xo)
    return "op%d"%op
c=collections.Counter();ex={}
for i,x in enumerate(ws):
    if x==0x4E800420:
        seq=tuple(m(y) for y in ws[i-6:i])
        if any(s in seq for s in("lwzx","lhzx","lbzx","lhax")):
            c[seq]+=1; ex.setdefault(seq,B+0xD0000+i*4)
for s,n in c.most_common(15): print(n,"0x%08X"%ex[s]," ".join(s),"bctr")
