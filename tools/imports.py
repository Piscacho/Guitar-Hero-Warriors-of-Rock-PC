"""Resuelve imports XEX: python -I imports.py xex_info.txt basefile.bin > imports.txt"""
import struct, sys
info = open(sys.argv[1]).read().splitlines()
img = open(sys.argv[2], "rb").read()
BASE = 0x82000000
lib = None
nf = nv = 0
for l in info:
    if l.startswith("import_lib"):
        lib = l.split()[1]; print("#", l)
    elif l.strip().startswith("thunk"):
        a = int(l.split()[1], 16)
        v = struct.unpack_from(">I", img, a - BASE)[0]
        typ, ordn = v >> 24, v & 0xFFFF
        kind = "func" if typ == 1 else "var"
        if typ == 1: nf += 1
        else: nv += 1
        print("%s\tord=%d\t0x%X\t%s\t@0x%08X" % (lib, ordn, ordn, kind, a))
print("# funcs=%d vars=%d" % (nf, nv))
