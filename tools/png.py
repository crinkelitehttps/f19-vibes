"""Minimal dependency-free paletted PNG writer."""
import struct
import zlib


def _chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def write_indexed(path, width, height, pixels, palette, scale=1):
    """pixels: bytes of palette indices, row-major. palette: list of (r, g, b)."""
    rows = bytearray()
    for y in range(height):
        row = pixels[y * width:(y + 1) * width]
        if scale > 1:
            row = bytes(p for p in row for _ in range(scale))
        for _ in range(scale):
            rows += b"\0" + row
    plte = b"".join(bytes(c) for c in palette)
    ihdr = struct.pack(">IIBBBBB", width * scale, height * scale, 8, 3, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", plte)
                + _chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + _chunk(b"IEND", b""))
