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


def write_rgb(path, width, height, rgb):
    """rgb: bytes, 3 per pixel, row-major."""
    rows = bytearray()
    for y in range(height):
        rows += b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + _chunk(b"IEND", b""))


def ppm_to_png(src, dst, scale=1):
    d = open(src, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    if scale > 1:
        out = bytearray()
        for y in range(h):
            row = px[y * w * 3:(y + 1) * w * 3]
            srow = b"".join(row[x * 3:x * 3 + 3] * scale for x in range(w))
            out += srow * scale
        px, w, h = bytes(out), w * scale, h * scale
    write_rgb(dst, w, h, px)
