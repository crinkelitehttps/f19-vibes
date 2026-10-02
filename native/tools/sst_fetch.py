"""Fetch SingleStepTests 8088 v2 vectors and convert them for tests/sst_runner.

Usage: python3 sst_fetch.py OUTDIR [OPCODE ...]   (default: every normal opcode)
Writes OUTDIR/<name>.bin per test file. Tests are MIT-licensed:
https://github.com/SingleStepTests/8088

Binary format (little endian), per file:
  u32 count; then per test:
    u16 name_len, name; u16 flags_mask (defined flags only)
    initial: u16 regs[14]; u32 n; {u32 addr; u8 value}[n]
    final:   u16 present_mask; u16 regs[14]; u32 n; {u32 addr; u8 value}[n]
  regs order: ax bx cx dx cs ss ds es sp bp si di ip flags
"""
import gzip
import json
import struct
import sys
import urllib.request
from pathlib import Path

BASE = "https://raw.githubusercontent.com/SingleStepTests/8088/main/v2/"
REGS = ["ax", "bx", "cx", "dx", "cs", "ss", "ds", "es", "sp", "bp", "si", "di", "ip", "flags"]
PREFIXES = {0x26, 0x2E, 0x36, 0x3E, 0xF0, 0xF1, 0xF2, 0xF3}


def fetch(url, dest):
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, dest)
    return dest


def file_list():
    with urllib.request.urlopen("https://api.github.com/repos/SingleStepTests/8088/contents/v2") as r:
        return [e["name"] for e in json.load(r) if e["name"].endswith(".json.gz")]


def convert(meta, name, src, dst):
    stem = name.split(".json")[0]          # "F6" or "F6.4"
    parts = stem.split(".")
    info = meta["opcodes"][parts[0]]
    if len(parts) > 1:
        info = info["reg"][parts[1]]
    if info.get("status") != "normal":
        return None
    mask = info.get("flags-mask", 0xFFFF)
    tests = json.load(gzip.open(src))
    out = bytearray(struct.pack("<I", len(tests)))
    for t in tests:
        nm = t["name"].encode()[:200]
        out += struct.pack("<H", len(nm)) + nm + struct.pack("<H", mask)
        ini, fin = t["initial"], t["final"]
        out += struct.pack("<14H", *(ini["regs"][r] for r in REGS))
        out += struct.pack("<I", len(ini["ram"])) + b"".join(struct.pack("<IB", a, v) for a, v in ini["ram"])
        present = sum(1 << i for i, r in enumerate(REGS) if r in fin["regs"])
        out += struct.pack("<H", present) + struct.pack("<14H", *(fin["regs"].get(r, 0) for r in REGS))
        out += struct.pack("<I", len(fin["ram"])) + b"".join(struct.pack("<IB", a, v) for a, v in fin["ram"])
    dst.write_bytes(out)
    return len(tests)


def main(outdir, *opcodes):
    out = Path(outdir)
    meta = json.loads(fetch(BASE + "metadata.json", out / "metadata.json").read_text())
    names = file_list()
    if opcodes:
        want = {o.upper() for o in opcodes}
        names = [n for n in names if n.split(".")[0] in want]
    for n in sorted(names):
        dst = out / (n.split(".json")[0] + ".bin")
        if dst.exists():
            continue
        src = fetch(BASE + n, out / "json" / n)
        count = convert(meta, n, src, dst)
        print(f"{n}: {'skipped (' + 'not normal' + ')' if count is None else count}", flush=True)


if __name__ == "__main__":
    main(*sys.argv[1:])
