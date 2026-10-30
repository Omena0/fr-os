#!/usr/bin/env python3
"""Generate a PSF1 8x16 console font from a Uniscii .hex font file.

Usage:
    python3 tools/gen_font.py <font.hex> [out.psf]

Uniscii (.hex) format: one line per glyph, ``cccc:hhhhhhhhhhhhhhhh`` where
``cccc`` is the Unicode code point and the 32 hex digits are the 16 glyph
rows (one byte per row, MSB leftmost). Only code points 0x00-0xFF (Latin-1)
are emitted; everything else is ignored.

The output is a PSF1 font: 256-byte header followed by 256 glyphs of 16
bytes each, consumable by tools/psf2c.py into
src/kernel/drivers/font8x16.c.

Vendor the Uniscii .hex you want (e.g. docs/fonts/unscii-16.hex, public
domain) rather than regenerating from a smoothed outline font.
"""

import sys


def parse_hex(path):
    """Return {codepoint: bytes16} from a Uniscii .hex file."""
    glyphs = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" not in line:
                continue
            addr, data = line.split(":", 1)
            try:
                cp = int(addr, 16)
            except ValueError:
                continue
            data = data.strip()
            if len(data) < 32:
                continue
            try:
                rows = bytes(int(data[i:i + 2], 16) for i in range(0, 32, 2))
            except ValueError:
                continue
            glyphs[cp] = rows
    return glyphs


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    in_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "build/lat15.psf"

    cell_w, cell_h = 8, 16
    glyphs = parse_hex(in_path)

    out = bytearray()
    for cp in range(256):
        rows = glyphs.get(cp, b"\x00" * cell_h)
        out.extend(rows[:cell_h].ljust(cell_h, b"\x00"))

    # PSF1 header: magic(2) + height + length(2).
    header = bytearray(256)
    header[0] = 0xB5
    header[1] = 0x00
    header[2] = cell_h
    header[3] = 256 & 0xFF
    header[4] = (256 >> 8) & 0xFF

    data = bytes(header) + bytes(out)
    with open(out_path, "wb") as fh:
        fh.write(data)
    covered = sum(1 for cp in range(256) if cp in glyphs)
    print(f"wrote {out_path} ({len(data)} bytes, {cell_w}x{cell_h}, "
          f"{covered}/256 code points covered)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
