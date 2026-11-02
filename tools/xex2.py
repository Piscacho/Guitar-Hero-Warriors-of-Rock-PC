"""Parser/descifrador XEX2 minimo. Uso: python -I xex2.py default.xex salida_dir
Genera: basefile.bin (imagen PE descifrada/descomprimida), xex_info.txt
AES via openssl (MSYS2). LZX via lzx.py (puerto de libmspack lzxd).
"""
import struct, sys, os, subprocess

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def aes(data, key, ecb=False):
    args = ["openssl", "enc", "-d", "-nopad", "-K", key.hex()]
    args += ["-aes-128-ecb"] if ecb else ["-aes-128-cbc", "-iv", "00" * 16]
    r = subprocess.run(args, input=data, stdout=subprocess.PIPE, check=True)
    return r.stdout


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def main():
    xex = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    info = []
    assert xex[:4] == b"XEX2", xex[:4]
    mod_flags, pe_off, _, sec_off, nhdr = struct.unpack_from(">IIIII", xex, 4)
    info.append("module_flags=0x%X pe_data_offset=0x%X security_offset=0x%X opt_headers=%d" % (mod_flags, pe_off, sec_off, nhdr))
    hdrs = {}
    for i in range(nhdr):
        k, v = struct.unpack_from(">II", xex, 0x18 + i * 8)
        hdrs[k] = v
        info.append("  hdr 0x%08X = 0x%08X" % (k, v))
    # security info
    s = sec_off
    image_size = be32(xex, s + 4)
    load_addr = be32(xex, s + 0x110)
    img_flags = be32(xex, s + 0x10C)
    file_key = xex[s + 0x150:s + 0x160]
    page_count = be32(xex, s + 0x180)
    info.append("image_size=0x%X load_address=0x%X image_flags=0x%X pages=%d" % (image_size, load_addr, img_flags, page_count))
    # paginas (secciones logicas del XEX: code/data/readonly)
    page_size = 0x10000 if not (img_flags & 0x10000000) else 0x1000
    addr = load_addr
    pages = []
    for i in range(page_count):
        d = be32(xex, s + 0x184 + i * 24)
        cnt, fl = d >> 4, d & 0xF
        pages.append((addr, cnt * page_size, fl))
        addr += cnt * page_size
    # agrupar paginas contiguas del mismo tipo
    groups = []
    for a, sz, fl in pages:
        if groups and groups[-1][2] == fl:
            groups[-1][1] += sz
        else:
            groups.append([a, sz, fl])
    names = {1: "code", 2: "data", 3: "readonly"}
    for a, sz, fl in groups:
        info.append("  region 0x%08X-0x%08X %s" % (a, a + sz, names.get(fl, str(fl))))
    if 0x00010100 in hdrs:
        info.append("entry_point=0x%08X" % hdrs[0x00010100])
    if 0x00010201 in hdrs:
        info.append("image_base(hdr)=0x%08X" % hdrs[0x00010201])
    ffi = hdrs[0x000003FF]
    ffi_size = be32(xex, ffi)
    enc_type, comp_type = struct.unpack_from(">HH", xex, ffi + 4)
    info.append("encryption=%d compression=%d" % (enc_type, comp_type))
    data = xex[pe_off:]
    if enc_type == 1:
        session = aes(file_key, RETAIL_KEY, ecb=True)
        data = aes(data[: len(data) & ~15], session) + data[len(data) & ~15:]
    if comp_type == 1:  # basic
        nblocks = (ffi_size - 8) // 8
        img = bytearray()
        p = 0
        for i in range(nblocks):
            ds, zs = struct.unpack_from(">II", xex, ffi + 8 + i * 8)
            img += data[p:p + ds] + bytes(zs)
            p += ds
    elif comp_type == 2:  # LZX
        window = be32(xex, ffi + 8)
        blk_size = be32(xex, ffi + 12)
        comp = bytearray()
        p = 0
        while blk_size:
            blk = data[p:p + blk_size]
            nxt = be32(blk, 0)
            q = 24
            while True:
                cs = struct.unpack_from(">H", blk, q)[0]
                q += 2
                if cs == 0:
                    break
                comp += blk[q:q + cs]
                q += cs
            p += blk_size
            blk_size = nxt
        info.append("lzx window=0x%X compressed=%d" % (window, len(comp)))
        open(os.path.join(out, "lzx_stream.bin"), "wb").write(comp)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import lzx
        img = lzx.decompress(bytes(comp), window.bit_length() - 1, image_size)
    else:
        img = data
    open(os.path.join(out, "basefile.bin"), "wb").write(img)
    info.append("basefile=%d bytes, MZ=%r" % (len(img), bytes(img[:2])))
    # import libraries header (0x000103FF)
    if 0x000103FF in hdrs:
        o = hdrs[0x000103FF]
        size, strsz, nlib = struct.unpack_from(">III", xex, o)
        strs = xex[o + 12:o + 12 + strsz].split(b"\0")
        strs = [x.decode() for x in strs if x]
        q = o + 12 + strsz
        for i in range(nlib):
            lsz = be32(xex, q)
            ver, minver = be32(xex, q + 0x1C), be32(xex, q + 0x20)
            name_idx, cnt = struct.unpack_from(">HH", xex, q + 0x24)
            info.append("import_lib %s ver=0x%X count=%d" % (strs[name_idx], ver, cnt))
            for j in range(cnt):
                info.append("  thunk 0x%08X" % be32(xex, q + 0x28 + j * 4))
            q += lsz
    open(os.path.join(out, "xex_info.txt"), "w").write("\n".join(info) + "\n")
    print("\n".join(info[:60]))


if __name__ == "__main__":
    main()
