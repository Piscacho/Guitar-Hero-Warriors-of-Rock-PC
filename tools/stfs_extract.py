"""Extrae un paquete STFS (CON/LIVE/PIRS) de Xbox 360 a una carpeta de contenido del runtime (sesion 23).
Uso: python stfs_extract.py <paquete> <carpeta userdata\\content>
Crea <content>\\<tipo>_<archivo>\\... y <content>\\<tipo>_<archivo>.xcd (XCONTENT_DATA de 0x134 bytes) para el enumerador.
Formato segun la documentacion publica de STFS (Velocity / wxPirs): tablas de hash cada 0xAA bloques de 4 KB."""
import os, struct, sys

def u24le(b, o): return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)
def u24be(b, o): return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]

class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0xA000)
        self.magic = h[0:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError("no es un paquete STFS: %r" % self.magic)
        self.header_size = struct.unpack_from(">I", h, 0x340)[0]
        self.content_type = struct.unpack_from(">I", h, 0x344)[0]
        self.title_id = struct.unpack_from(">I", h, 0x360)[0]
        vd = 0x379
        self.block_sep = h[vd + 2]
        self.ft_blocks = struct.unpack_from("<H", h, vd + 3)[0]
        self.ft_start = u24le(h, vd + 5)
        self.alloc_blocks = struct.unpack_from(">I", h, vd + 0x1C)[0]
        self.display_name = h[0x411:0x411 + 0x100].decode("utf-16-be", "replace").split("\0")[0]
        self.sex = (~self.block_sep) & 1
        self.first_hash = (self.header_size + 0xFFF) & 0xFFFFF000
        self.block_step = (0xAB, 0x718F) if self.sex == 0 else (0xAC, 0x723A)

    def backing_block(self, n):
        r = (((n + 0xAA) // 0xAA) << self.sex) + n
        if n < 0xAA: return r
        if n < 0x70E4: return r + (((n + 0x70E4) // 0x70E4) << self.sex)
        return (1 << self.sex) + r + (((n + 0x70E4) // 0x70E4) << self.sex)

    def block_offset(self, n): return (self.backing_block(n) << 12) + self.first_hash

    def level0_hash_block(self, n):
        if n < 0xAA: return 0
        num = (n // 0xAA) * self.block_step[0]
        num += ((n // 0x70E4) + 1) << self.sex
        if n // 0x70E4 == 0: return num
        return num + (1 << self.sex)

    def next_block(self, n):
        # Entrada de hash de nivel 0 (0x18 bytes: SHA1, estado, siguiente bloque BE de 24 bits). Se usa la primera copia de la tabla.
        addr = (self.level0_hash_block(n) << 12) + self.first_hash + (n % 0xAA) * 0x18
        self.f.seek(addr); e = self.f.read(0x18)
        return u24be(e, 0x15)

    def read_blocks(self, start, count, size, consecutive):
        out = bytearray(); n = start
        for i in range(count):
            self.f.seek(self.block_offset(n)); out += self.f.read(0x1000)
            n = n + 1 if consecutive else self.next_block(n)
            if len(out) >= size: break
        return bytes(out[:size])

    def files(self):
        table = self.read_blocks(self.ft_start, self.ft_blocks, self.ft_blocks * 0x1000, True)
        entries = []
        for i in range(0, len(table), 0x40):
            e = table[i:i + 0x40]
            flags = e[0x28]
            nlen = flags & 0x3F
            if nlen == 0: continue
            entries.append({
                "name": e[:nlen].decode("latin-1"), "dir": bool(flags & 0x80), "consecutive": bool(flags & 0x40),
                "blocks": u24le(e, 0x29), "start": u24le(e, 0x2F),
                "parent": struct.unpack_from(">h", e, 0x32)[0], "size": struct.unpack_from(">I", e, 0x34)[0]})
        return entries

def main():
    pkg, content_dir = sys.argv[1], sys.argv[2]
    s = Stfs(pkg)
    fname = os.path.basename(pkg)[:42]
    folder = ("%08x_%s" % (s.content_type, fname)).lower()
    out = os.path.join(content_dir, folder)
    os.makedirs(out, exist_ok=True)
    ents = s.files()
    print("%s: %s, tipo 0x%X, titulo %08X, %d entradas, nombre '%s'" % (fname, s.magic.decode(), s.content_type, s.title_id, len(ents), s.display_name))
    def path_of(i):
        parts = []; e = ents[i]
        while True:
            parts.append(e["name"])
            if e["parent"] < 0 or e["parent"] >= len(ents): break
            e = ents[e["parent"]]
        return os.path.join(*reversed(parts))
    total = 0; nonconsec = 0
    for i, e in enumerate(ents):
        p = os.path.join(out, path_of(i))
        if e["dir"]: os.makedirs(p, exist_ok=True); continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not e["consecutive"]: nonconsec += 1
        data = s.read_blocks(e["start"], e["blocks"], e["size"], e["consecutive"])
        with open(p, "wb") as f: f.write(data)
        total += len(data)
    # XCONTENT_DATA: device_id, content_type, display_name (UTF-16BE x128), file_name (char[42]), relleno hasta 0x134
    xcd = struct.pack(">II", 1, s.content_type) + s.display_name[:127].encode("utf-16-be").ljust(256, b"\0") + fname.encode("ascii").ljust(42, b"\0")
    xcd = xcd.ljust(0x134, b"\0")
    with open(os.path.join(content_dir, folder + ".xcd"), "wb") as f: f.write(xcd)
    print("extraidos %d bytes en %s (%d archivos no consecutivos)" % (total, out, nonconsec))

if __name__ == "__main__":
    main()
