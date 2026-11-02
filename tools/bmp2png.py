"""Convierte un BMP de 32 bits (top-down) a PNG reducido a la mitad. python -I bmp2png.py in.bmp out.png"""
import struct, zlib, sys
d = open(sys.argv[1], "rb").read()
W = struct.unpack_from("<i", d, 18)[0]; H = abs(struct.unpack_from("<i", d, 22)[0]); px = d[54:]
sw, sh = W // 2, H // 2
rows = bytearray()
for y in range(sh):
    rows.append(0)
    for x in range(sw):
        o = ((y * 2) * W + (x * 2)) * 4
        rows += bytes((px[o + 2], px[o + 1], px[o]))
def chunk(t, data): return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", sw, sh, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))
