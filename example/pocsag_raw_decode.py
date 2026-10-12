#!/usr/bin/env python3
"""
example/pocsag_raw_decode.py — decode POCSAG raw payload bits from multimon-ng
--json output (schema v2).

The "raw" encoding in a POCSAG message contains the 20-bit data fields from
each received codeword, packed consecutively MSB-first into nibbles
(5 nibbles = 20 bits per codeword).  The alpha and numeric decoders both
operate on this same bit stream using different interpretations:

  Alpha:   7-bit codepoints packed MSB-first, bit-reversed per the POCSAG spec
  Numeric: 4-bit BCD nibbles, high nibble first per byte

Usage (decode from live multimon-ng output):

    multimon-ng -t flac -a POCSAG1200 --json capture.flac \\
        | python3 example/pocsag_raw_decode.py

Usage (decode a single raw value from the command line):

    python3 example/pocsag_raw_decode.py \\
        --hex d5ab51593668de0c1830ecc183664c5b56ad4564d9a3783060c3b3060d99316000 \\
        --bits 260
"""

import sys
import json
import argparse


# ---------------------------------------------------------------------------
# POCSAG alpha decode
# Standard 7-bit table (US ASCII subset + control chars).
# Codepoints above 0x7e are not defined; rendered as '.'.
# ---------------------------------------------------------------------------

def _rev7(b: int) -> int:
    """Reverse bit order of a 7-bit value (LSB-first on wire -> MSB-first index)."""
    return (
        ((b << 6) & 0x40) | ((b >> 6) & 0x01) |
        ((b << 4) & 0x20) | ((b >> 4) & 0x02) |
        ((b << 2) & 0x10) | ((b >> 2) & 0x04) |
        (b & 0x08)
    )


def _get7(buf: bytes, n: int) -> int:
    """Extract the n-th 7-bit codepoint from the packed byte buffer."""
    hi = buf[(n * 7) // 8]
    lo = buf[(n * 7 + 6) // 8]
    return ((hi << 8 | lo) >> ((n + 1) % 8)) & 0x7F


def decode_alpha(raw_hex: str, bits: int) -> str:
    """Decode POCSAG alpha from raw hex payload."""
    buf = bytes.fromhex(raw_hex)
    n_chars = (bits * 8 // 7) // 8  # conservative: bits / 7
    # more precisely: floor(bits / 7)
    n_chars = bits // 7
    result = []
    for i in range(n_chars):
        code = _rev7(_get7(buf, i))
        if code == 0:
            break                   # null terminator
        if 0x20 <= code <= 0x7E:
            result.append(chr(code))
        elif code == 0x0A:
            result.append('\n')
        elif code == 0x0D:
            result.append('\r')
        else:
            result.append(f'<{code:02X}>')
    return ''.join(result)


# ---------------------------------------------------------------------------
# POCSAG numeric decode
# Nibble -> character table from pocsag.c: "084 2.6]195-3U7["
# Index in this string = nibble value in the packed buffer.
# ---------------------------------------------------------------------------

_NUMERIC_TABLE = "084 2.6]195-3U7["


def decode_numeric(raw_hex: str, bits: int) -> str:
    """Decode POCSAG numeric from raw hex payload."""
    buf = bytes.fromhex(raw_hex)
    n_nibbles = bits // 4
    result = []
    for i in range(n_nibbles):
        byte = buf[i // 2]
        nib = (byte >> 4) & 0xF if i % 2 == 0 else byte & 0xF
        result.append(_NUMERIC_TABLE[nib])
    return ''.join(result)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def process_one(raw_hex: str, bits: int, capcode: int = None, label: str = ""):
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
        description="Decode POCSAG raw payload bits from multimon-ng --json output."
    )
    parser.add_argument("--hex",  help="Raw hex string (standalone mode)")
    parser.add_argument("--bits", type=int, help="Bit count (standalone mode)")
    args = parser.parse_args()

    if args.hex:
        if not args.bits:
            parser.error("--bits required with --hex")
        process_one(args.hex, args.bits, label="[cmdline] ")
        return

    # stdin: consume multimon-ng --json output line by line
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        if msg.get("protocol") != "pocsag":
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
            label=f"[{msg.get('demod', 'POCSAG')}] ",
        )


if __name__ == "__main__":
    main()
