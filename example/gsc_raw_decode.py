#!/usr/bin/env python3
"""
example/gsc_raw_decode.py — decode GSC raw payload bytes from multimon-ng
--json output (schema v2).

The "raw" encoding in a GSC message contains the BCH(15,7) data bytes
from each decoded block: 7 bytes per block (d[0..6]), concatenated.
Both the alpha and numeric decoders operate on this same byte array
using different bit unpack layouts:

  Alpha:   7 bytes -> 8 six-bit codepoints, packed LSB-first across bytes
  Numeric: 7 bytes -> 12 four-bit nibbles, packed LSB-first across bytes,
           with a shift-prefix mechanism (0xF) for extended characters

Usage (decode from live multimon-ng output):

    multimon-ng -t flac -a GSC --json capture.flac \\
        | python3 example/gsc_raw_decode.py

Usage (decode a single raw value from the command line):

    python3 example/gsc_raw_decode.py \\
        --hex 552a071924552a --bits 56
"""

import sys
import json
import argparse


# ---------------------------------------------------------------------------
# GSC decode tables (from demod_gsc.c)
# ---------------------------------------------------------------------------

# alpha_table[i] = character for 6-bit codepoint i (index 62 = CR -> newline)
_ALPHA_TABLE = (
    ' ', '!', '"', '#', '$', '%', '&', "'", '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', '\n', ']', '\x00', '_',
)

# numeric_table[i] = character for 4-bit nibble i (unshifted)
_NUM_TABLE = ('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '\x00', 'U', ' ', '-', '*', '\x00')

# numeric_shift_table[i] = character for 4-bit nibble i (after 0xF shift prefix)
_NUM_SHIFT = ('A', 'B', 'C', 'D', 'E', ' ', 'F', 'G', 'H', 'J', '\x00', 'L', 'N', 'P', 'R', '?')


# ---------------------------------------------------------------------------
# Decode functions
# ---------------------------------------------------------------------------

def _block_alpha(d7: bytes) -> str:
    """Unpack 8 six-bit alpha codepoints from one 7-byte GSC data block."""
    d = d7
    ch = [
        d[0] & 0x3F,
        ((d[0] >> 6) | (d[1] << 1)) & 0x3F,
        ((d[1] >> 5) | (d[2] << 2)) & 0x3F,
        ((d[2] >> 4) | (d[3] << 3)) & 0x3F,
        ((d[3] >> 3) | (d[4] << 4)) & 0x3F,
        ((d[4] >> 2) | (d[5] << 5)) & 0x3F,
        ((d[5] >> 1) | (d[6] << 6)) & 0x3F,
        (d[6] >> 0) & 0x3F,          # top 6 bits of d[6] (cont bit is bit 6)
    ]
    result = []
    for c in ch:
        if c == 0x3E:          # fill character, skip
            continue
        ch_val = _ALPHA_TABLE[c & 0x3F]
        if ch_val == '\x00':   # null = end-of-message
            break
        result.append(ch_val)
    return ''.join(result)


def _block_numeric(d7: bytes) -> list:
    """Unpack 12 four-bit nibbles from one 7-byte GSC data block."""
    d = d7
    return [
        d[0] & 0x0F,
        ((d[0] >> 4) | (d[1] << 3)) & 0x0F,
        (d[1] >> 1) & 0x0F,
        ((d[1] >> 5) | (d[2] << 2)) & 0x0F,
        (d[2] >> 2) & 0x0F,
        ((d[2] >> 6) | (d[3] << 1)) & 0x0F,
        (d[3] >> 3) & 0x0F,
        d[4] & 0x0F,
        ((d[4] >> 4) | (d[5] << 3)) & 0x0F,
        (d[5] >> 1) & 0x0F,
        ((d[5] >> 5) | (d[6] << 2)) & 0x0F,
        (d[6] >> 2) & 0x0F,
    ]


def decode_alpha(raw_hex: str, bits: int) -> str:
    """Decode GSC alpha from raw hex payload (7 bytes per block)."""
    buf = bytes.fromhex(raw_hex)
    n_blocks = len(buf) // 7
    result = []
    for b in range(n_blocks):
        result.append(_block_alpha(buf[b * 7: b * 7 + 7]))
    return ''.join(result)


def decode_numeric(raw_hex: str, bits: int) -> str:
    """Decode GSC numeric from raw hex payload (7 bytes per block, max 2 blocks)."""
    buf = bytes.fromhex(raw_hex)
    n_blocks = min(len(buf) // 7, 2)   # protocol: only first 2 blocks are numeric
    nibbles = []
    for b in range(n_blocks):
        nibbles.extend(_block_numeric(buf[b * 7: b * 7 + 7]))
    result = []
    shifted = False
    for nib in nibbles:
        if nib == 0x0A:        # fill, skip
            continue
        if nib == 0x0F:        # shift prefix
            shifted = True
            continue
        c = _NUM_SHIFT[nib] if shifted else _NUM_TABLE[nib]
        shifted = False
        if c == '\x00':
            break
        result.append(c)
    return ''.join(result)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def process_one(raw_hex: str, bits: int, capcode=None, label: str = ""):
    alpha   = decode_alpha(raw_hex, bits)
    numeric = decode_numeric(raw_hex, bits)
    addr    = f"capcode={capcode}" if capcode is not None else ""
    print(f"{label}{addr}")
    print(f"  raw     ({bits:4d} bits): {raw_hex}")
    print(f"  alpha            : {alpha!r}")
    print(f"  numeric          : {numeric!r}")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Decode GSC raw payload bytes from multimon-ng --json output."
    )
    parser.add_argument("--hex",  help="Raw hex string (standalone mode)")
    parser.add_argument("--bits", type=int, help="Bit count (standalone mode)")
    args = parser.parse_args()

    if args.hex:
        if not args.bits:
            parser.error("--bits required with --hex")
        process_one(args.hex, args.bits, label="[cmdline] ")
        return

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        if msg.get("protocol") != "gsc":
            continue
        if msg.get("kind") != "message":
            continue

        raw_entry = next(
            (e for e in msg.get("encodings", []) if e["encoding"] == "raw"),
            None
        )
        if not raw_entry:
            continue

        process_one(
            raw_entry["data"],
            raw_entry["bits"],
            capcode=msg.get("capcode"),
            label=f"[{msg.get('demod', 'GSC')}] ",
        )


if __name__ == "__main__":
    main()
