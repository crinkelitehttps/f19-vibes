"""Decode STREAM.DTA, the F-19 demo's recorded input stream.

Reverse engineered from DGAME.EXE (recorder FUN_1000_ddfc / ddae / df22,
player FUN_1000_df79). Three input channels are run-length encoded into one
shared sequence of 4-byte records (s16 value, s16 count). A channel holds a
value for count + 1 reads, then takes the *next record in the sequence*; the
recorder reserves a record whenever a channel starts a new run, so records
appear in the order runs start. Records 0, 1, 2 are the first runs of
channels 0, 1, 2. (The recorder stores count + (value >> 15), so a negative
value is held one read less; the player uses the stored count as-is.)

Channels, and when the game reads them each frame (main loop FUN_1000_1b8c):
  0  key code (BIOS scan<<8 | ascii; 0 = none)   FUN_1000_200a
  1  joystick: high nibble X, low nibble Y (8 = centre)   FUN_1000_200a
  2  button 0 (fire) - only when frame counter [0x5442] is odd   FUN_1000_4874
     (the counter is then incremented)
  2  button 1   FUN_1000_d298 (key handler, every frame)

Usage: python3 replay.py STREAM.DTA [OUT.json]
"""
import json
import struct
import sys

KEY_NAMES = {
    0x5200: "Insert (end recording)", 0x0D3D: "= (throttle up)", 0x0C2D: "- (throttle down)",
    0x0D2B: "+ (throttle full)", 0x0C5F: "_ (throttle zero)", 0x0B30: "0", 0x1000: "Q",
}


def read_records(data):
    return [struct.unpack_from("<hh", data, i) for i in range(0, len(data) - 3, 4)]


def decode(records, start_odd, max_frames=200000):
    """Replay the read order. Returns per-frame dicts, stops at the terminator."""
    pos = 0
    value = [0, 0, 0]
    count = [0, 0, 0]

    def read(ch):
        nonlocal pos
        count[ch] -= 1
        if count[ch] < 0:
            if pos >= len(records):
                raise EOFError
            value[ch], count[ch] = records[pos]
            pos += 1
        return value[ch]

    frames = []
    counter = 1 if start_odd else 0
    try:
        for _ in range(max_frames):
            f = {"key": read(0) & 0xFFFF, "stick": read(1) & 0xFF}
            f["fire"] = read(2) if counter & 1 else None
            counter += 1
            f["button1"] = read(2)
            frames.append(f)
            if f["key"] == 0x5200:
                break
    except EOFError:
        pass
    return frames, pos


def plausibility(frames):
    """Count values that don't fit their channel: keys are 0 or >= 0x100,
    stick nibbles are 0..15 (anything fits), buttons are small."""
    bad = 0
    for f in frames:
        if f["key"] and f["key"] < 0x100:
            bad += 1
        for b in (f["fire"], f["button1"]):
            if b is not None and not 0 <= b <= 4:
                bad += 1
    return bad


def main(path, out=None):
    records = read_records(open(path, "rb").read())
    results = []
    for start_odd in (False, True):
        frames, used = decode(records, start_odd)
        results.append((plausibility(frames), start_odd, frames, used))
        print(f"start counter {'odd' if start_odd else 'even'}: {len(frames)} frames, "
              f"{used}/{len(records)} records, {results[-1][0]} implausible values, "
              f"ends with {'Insert' if frames and frames[-1]['key'] == 0x5200 else 'end of data'}")
    bad, start_odd, frames, used = min(results, key=lambda r: r[0])
    if out:
        json.dump({"start_counter_odd": start_odd, "records_used": used, "frames": frames}, open(out, "w"))
    return frames


if __name__ == "__main__":
    main(*sys.argv[1:3])
