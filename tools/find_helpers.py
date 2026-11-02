"""Busca __savegprlr/__restgprlr/__savefpr/__restfpr/__savevmx/__restvmx en basefile.bin"""
import struct, sys
img = open(sys.argv[1], "rb").read()
BASE = 0x82000000
T0, T1 = 0xD0000, 0xD0000 + 0x6EE6A4
words = struct.unpack_from(">%dI" % ((T1 - T0) // 4), img, T0)
def find(seq, name):
    hits = []
    n = len(seq)
    for i in range(len(words) - n):
        if words[i] == seq[0] and list(words[i:i+n]) == seq:
            hits.append(BASE + T0 + i * 4)
    print("%-16s %s" % (name, " ".join("0x%08X" % h for h in hits) or "NO ENCONTRADO"))
    return hits
# std r14,-0x98(r1); std r15,-0x90(r1)
find([0xF9C1FF68, 0xF9E1FF70], "__savegprlr_14")
find([0xE9C1FF68, 0xE9E1FF70], "__restgprlr_14")
find([0xD9CCFF70, 0xD9ECFF78], "__savefpr_14")
find([0xC9CCFF70, 0xC9ECFF78], "__restfpr_14")
find([0x3960FEE0, 0x7DCB61CE], "__savevmx_14")
find([0x3960FEE0, 0x7DCB60CE], "__restvmx_14")
# vmx128: li r11,-0x400 seguido de stvx128/lvx128 (opcode 4)
for i in range(len(words) - 1):
    if words[i] == 0x3960FC00 and (words[i+1] >> 26) == 4:
        print("vmx_64 cand     0x%08X next=0x%08X" % (BASE + T0 + i*4, words[i+1]))
