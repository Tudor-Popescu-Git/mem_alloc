#!/usr/bin/env python3
"""Convert memory dumps to CSV.

Accepts raw binary files (.bin/.dmp) or text hex dumps: Visual Studio's
Memory window, Debug.ListMemory output, xxd, hexdump -C, or a bare hex
string. Address and ASCII columns are stripped automatically.

  python memdump_csv.py grid  before.txt -o grid.csv
  python memdump_csv.py long  before.txt -o long.csv --base 0x7FF6A1B2C000
  python memdump_csv.py diff  before.txt after.txt -o diff.csv
"""

import argparse
import csv
import re
import sys
from pathlib import Path

HEX_BYTE = re.compile(r"[0-9a-fA-F]{2}\Z")
HEX_RUN = re.compile(r"[0-9a-fA-F]+\Z")
ADDRESS = re.compile(r"(?:0x[0-9a-fA-F]+|[0-9a-fA-F]{5,}):?\Z")
BINARY_SUFFIXES = {".bin", ".dmp", ".raw", ".dat"}


def parse_text(text):
    """Pull bytes out of a text hex dump, ignoring address and ASCII columns."""
    out = bytearray()
    for line in text.splitlines():
        tokens = [t for t in re.split(r"[\s,]+", line.strip()) if t]
        if len(tokens) > 2 and ADDRESS.match(tokens[0]):
            tokens = tokens[1:]
        for token in tokens:
            token = token.rstrip(",;")
            if token[:2].lower() == "0x":
                token = token[2:]
            if HEX_BYTE.match(token):
                out.append(int(token, 16))
            elif HEX_RUN.match(token) and len(token) % 2 == 0 and len(token) > 2:
                out.extend(bytes.fromhex(token))
            else:
                break  # ASCII column or trailing junk: rest of line is not data
    return bytes(out)


def load(path, force_binary=False):
    data = Path(path).read_bytes()
    if force_binary or Path(path).suffix.lower() in BINARY_SUFFIXES:
        return data
    sample = data[:512]
    printable = sum(1 for b in sample if 9 <= b <= 13 or 32 <= b < 127)
    if sample and printable / len(sample) < 0.9:
        return data  # looks like binary despite the extension
    return parse_text(data.decode("utf-8", "replace"))


def printable_char(value):
    return chr(value) if 32 <= value < 127 else "."


def write_grid(data, out, width, base):
    """One row per line of the dump; columns are byte positions."""
    header = ["offset"] + [f"{c:02X}" for c in range(width)] + ["ascii"]
    out.writerow(header)
    for offset in range(0, len(data), width):
        chunk = data[offset : offset + width]
        cells = [f"{b:02X}" for b in chunk] + [""] * (width - len(chunk))
        text = "".join(printable_char(b) for b in chunk)
        out.writerow([f"{base + offset:08X}"] + cells + [text])


def write_long(data, out, base):
    """One row per byte. Best for pivot tables and charting."""
    out.writerow(["index", "address", "dec", "hex", "char"])
    for i, b in enumerate(data):
        out.writerow([i, f"{base + i:08X}", b, f"{b:02X}", printable_char(b)])


def write_diff(before, after, out, base):
    """One row per byte position across both dumps."""
    out.writerow(
        ["index", "address", "before_dec", "before_hex",
         "after_dec", "after_hex", "changed", "delta"]
    )
    for i in range(max(len(before), len(after))):
        b = before[i] if i < len(before) else None
        a = after[i] if i < len(after) else None
        if b is None or a is None:
            changed, delta = "missing", ""
        else:
            changed = "yes" if a != b else "no"
            delta = a - b
        out.writerow([
            i,
            f"{base + i:08X}",
            "" if b is None else b,
            "" if b is None else f"{b:02X}",
            "" if a is None else a,
            "" if a is None else f"{a:02X}",
            changed,
            delta,
        ])


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["grid", "long", "diff"])
    p.add_argument("files", nargs="+", help="one dump, or two for diff mode")
    p.add_argument("-o", "--output", help="CSV path (default: stdout)")
    p.add_argument("-w", "--width", type=int, default=16,
                   help="bytes per row in grid mode (default 16)")
    p.add_argument("-b", "--base", default="0",
                   help="base address added to offsets, e.g. 0x7FF6A1B2C000")
    p.add_argument("--binary", action="store_true",
                   help="force raw binary parsing")
    args = p.parse_args()

    if args.mode == "diff" and len(args.files) != 2:
        p.error("diff mode needs exactly two files")
    if args.mode != "diff" and len(args.files) != 1:
        p.error(f"{args.mode} mode needs exactly one file")

    base = int(args.base, 0)
    dumps = [load(f, args.binary) for f in args.files]
    for path, data in zip(args.files, dumps):
        if not data:
            sys.exit(f"No bytes found in {path}")
        print(f"{path}: {len(data)} bytes", file=sys.stderr)

    handle = open(args.output, "w", newline="") if args.output else sys.stdout
    try:
        writer = csv.writer(handle)
        if args.mode == "grid":
            write_grid(dumps[0], writer, args.width, base)
        elif args.mode == "long":
            write_long(dumps[0], writer, base)
        else:
            write_diff(dumps[0], dumps[1], writer, base)
    finally:
        if args.output:
            handle.close()

    if args.output:
        print(f"wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
