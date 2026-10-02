"""Unpack Microsoft EXEPACK-compressed DOS executables.

Usage: python3 unexepack.py IN.EXE OUT.EXE
"""
import struct
import sys


def unpack(d):
    h = struct.unpack_from("<14H", d, 0)
    hdr_size, cs = h[4] * 16, h[11]
    image = bytearray(d[hdr_size:])
    eo = cs * 16
    ip, rcs, _mem, xsize, sp, ss, dest_len, skip_len, sig = struct.unpack_from("<8H2s", image, eo)
    assert sig == b"RB", "not EXEPACK"

    # Decompress backwards in place.
    buf = bytearray(max(dest_len * 16, eo))
    buf[:eo] = image[:eo]
    src, dst = eo - (skip_len - 1) * 16, dest_len * 16
    while buf[src - 1] == 0xFF:
        src -= 1
    while True:
        cmd = buf[src - 1]
        length = buf[src - 2] << 8 | buf[src - 3]
        src -= 3
        if cmd & 0xFE == 0xB0:
            val = buf[src - 1]
            src -= 1
            dst -= length
            buf[dst:dst + length] = bytes([val]) * length
        elif cmd & 0xFE == 0xB2:
            src -= length
            dst -= length
            buf[dst:dst + length] = buf[src:src + length]
        else:
            raise ValueError(f"bad command {cmd:#x}")
        if cmd & 1:
            break

    # Relocation table: follows the error message in the stub; 16 x (count, offsets...).
    stub = image[eo:eo + xsize]
    p = stub.index(b"Packed file is corrupt") + len(b"Packed file is corrupt")
    relocs = []
    for seg in range(16):
        (n,) = struct.unpack_from("<H", stub, p)
        p += 2
        for off in struct.unpack_from(f"<{n}H", stub, p):
            relocs.append((off, seg * 0x1000))
        p += 2 * n

    # Rebuild an MZ file.
    hdr_len = 0x1C + 4 * len(relocs)
    hdr_paras = (hdr_len + 15) // 16
    body = bytes(buf[:dest_len * 16])
    total = hdr_paras * 16 + len(body)
    out = bytearray(hdr_paras * 16)
    struct.pack_into("<14H", out, 0, 0x5A4D, total % 512, (total + 511) // 512, len(relocs),
                     hdr_paras, h[5], h[6], ss, sp, 0, ip, rcs, 0x1C, 0)
    for i, (off, seg) in enumerate(relocs):
        struct.pack_into("<2H", out, 0x1C + 4 * i, off, seg)
    return bytes(out) + body


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    open(sys.argv[2], "wb").write(unpack(data))
