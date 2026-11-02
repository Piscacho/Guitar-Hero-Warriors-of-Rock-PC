"""Extractor minimo XDVDFS (ISO Xbox 360 XGD2).
Uso: python -I xdvdfs.py ISO list [salida.txt]
     python -I xdvdfs.py ISO extract RUTA_EN_ISO DESTINO
"""
import struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
CANDIDATE_OFFSETS = [0xFD90000, 0x2080000, 0x18300000, 0]


def find_base(f):
    for base in CANDIDATE_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("No se encontro XDVDFS")


def read_dir(f, base, sector, size):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    out = []

    def walk(off):
        if off * 4 + 14 > len(data):
            return
        left, right, sec, sz, attr, nl = struct.unpack_from("<HHIIBB", data, off * 4)
        if left == 0xFFFF:
            return
        name = data[off * 4 + 14: off * 4 + 14 + nl].decode("latin-1")
        if left:
            walk(left)
        out.append((name, sec, sz, attr))
        if right:
            walk(right)

    if size:
        walk(0)
    return out


def walk_tree(f, base, sector, size, prefix=""):
    for name, sec, sz, attr in read_dir(f, base, sector, size):
        path = prefix + name
        if attr & 0x10:
            yield path + "/", sec, sz, True
            yield from walk_tree(f, base, sec, sz, path + "/")
        else:
            yield path, sec, sz, False


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_base(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sec, root_sz = struct.unpack("<II", f.read(8))
        if cmd == "list":
            lines = ["base=0x%X" % base]
            for p, sec, sz, d in walk_tree(f, base, root_sec, root_sz):
                lines.append("%s\t%d\t0x%X" % (p, sz, base + sec * SECTOR))
            txt = "\n".join(lines)
            if len(sys.argv) > 3:
                open(sys.argv[3], "w", encoding="utf-8").write(txt + "\n")
            print(txt[:3000])
        elif cmd == "extract":
            want = sys.argv[3].lower()
            for p, sec, sz, d in walk_tree(f, base, root_sec, root_sz):
                if p.lower() == want:
                    f.seek(base + sec * SECTOR)
                    with open(sys.argv[4], "wb") as o:
                        left = sz
                        while left:
                            chunk = f.read(min(left, 1 << 20))
                            o.write(chunk)
                            left -= len(chunk)
                    print("extraido", p, sz)
                    return
            raise SystemExit("no encontrado")


if __name__ == "__main__":
    main()
