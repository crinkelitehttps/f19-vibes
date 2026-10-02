"""MicroProse LZW + RLE decompression, as used by .PIC/.SPR files."""


def lzw_decode(data, start, maxbits):
    """Variable-width LSB-first LZW; codes start at 9 bits, dictionary resets when full."""
    out = bytearray()
    pos = start * 8
    total = len(data) * 8
    width = 9
    dict_ = [bytes([i]) for i in range(256)] + [b""]
    prev = None
    while pos + width <= total:
        byte = pos >> 3
        chunk = int.from_bytes(data[byte:byte + 4].ljust(4, b"\0"), "little")
        code = (chunk >> (pos & 7)) & ((1 << width) - 1)
        pos += width
        if prev is None:
            entry = dict_[code]
        elif code < len(dict_):
            entry = dict_[code]
            dict_.append(prev + entry[:1])
        elif code == len(dict_):
            entry = prev + prev[:1]
            dict_.append(entry)
        else:
            raise ValueError(f"bad code {code:#x} at bit {pos - width}, dict {len(dict_)}")
        out += entry
        prev = entry
        if len(dict_) >= (1 << width):
            if width < maxbits:
                width += 1
            else:
                dict_ = dict_[:257]
                width = 9
                prev = None
    return bytes(out)


def rle_decode(data, esc=0x90):
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        i += 1
        if b == esc and i < len(data):
            n = data[i]
            i += 1
            if n == 0:
                out.append(esc)
            else:
                out += bytes([out[-1]]) * (n - 1)
        else:
            out.append(b)
    return bytes(out)
