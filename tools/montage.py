"""Junta varios BMP de 32 bits (volcados fb_N.bmp) en una cuadricula PNG reducida a la mitad.
Uso: python montage.py salida.png columnas a.bmp b.bmp ..."""
import struct, sys, zlib
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
imgs = []
for f in files:
    d = open(f, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    top_down = h < 0; h = abs(h)
    rows = [d[off + y * w * 4: off + (y + 1) * w * 4] for y in range(h)]
    if not top_down: rows.reverse()
    sw, sh = w // 2, h // 2
    small = [bytes(b for x in range(sw) for b in (r[x * 8 + 2], r[x * 8 + 1], r[x * 8])) for r in rows[::2][:sh]]
    imgs.append((sw, sh, small))
sw, sh = imgs[0][0], imgs[0][1]
rowsN = (len(imgs) + cols - 1) // cols
W, H = sw * cols, sh * rowsN
raw = bytearray()
for gy in range(rowsN):
    for y in range(sh):
        raw.append(0)
        for gx in range(cols):
            i = gy * cols + gx
            raw += imgs[i][2][y] if i < len(imgs) else bytes(sw * 3)
def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
