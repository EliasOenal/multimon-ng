#!/usr/bin/env python3
"""
example/flex_raw_decode.py — decode FLEX raw payload bits from multimon-ng
--json output (schema v2).

The "raw" encoding in a FLEX message contains the decoded payload re-packed
into a compact bit stream:

  ALN (alphanumeric): 7-bit ASCII codepoints packed MSB-first.
  NUM (numeric):      4-bit BCD nibbles packed MSB-first.
                      Nibble 0xA is the spare slot: ' ' in old FLEX,
                      '.' in FLEX_NEXT (per ARIB STD-43A). Both decoders
                      use the same raw packing -- only the character
                      differs in the 'numeric' encoding.
  HEX/BIN:            raw payload bytes as-is (no re-packing needed).

Usage (decode from live multimon-ng output):

    multimon-ng -t flac -a FLEX -a FLEX_NEXT --json capture.flac \\
        | python3 example/flex_raw_decode.py

Usage (decode a single raw value from the command line):

    python3 example/flex_raw_decode.py \\
        --hex e9979f40 --bits 28 --type aln
"""

import sys
import json
import argparse


# ---------------------------------------------------------------------------
# BCD tables (from demod_flex.c / demod_flex_next.c)
# Old FLEX:   nibble 0xA -> ' '   table: "0123456789 U -]["
# FLEX_NEXT:  nibble 0xA -> '.'   table: "0123456789.U -]["
# ---------------------------------------------------------------------------
_BCD_OLD  = "0123456789 U -]["
_BCD_NEXT = "0123456789.U -]["


# ---------------------------------------------------------------------------
# ALN decode: MSB-first 7-bit codepoints
# ---------------------------------------------------------------------------

def decode_aln(raw_hex: str, bits: int) -> str:
    """Decode FLEX ALN raw payload: 7-bit codepoints packed MSB-first."""
    buf = bytes.fromhex(raw_hex)
    n_chars = bits // 7
    result = []
    for i in range(n_chars):
        bit_pos = i * 7
        byte_pos = bit_pos // 8
        bit_off  = bit_pos % 8
        # Extract 7 bits starting at bit_off in byte_pos (MSB-first)
        if byte_pos + 1 < len(buf):
            val = (buf[byte_pos] << 8 | buf[byte_pos + 1]) >> (16 - 7 - bit_off)
        else:
            val = buf[byte_pos] >> (8 - 7 - bit_off) if bit_off <= 1 else buf[byte_pos] << (bit_off - 1)
        code = val & 0x7F
        if code == 0:
            break
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
# NUM decode: MSB-first 4-bit BCD nibbles
# ---------------------------------------------------------------------------

def decode_num(raw_hex: str, bits: int, flex_next: bool = True) -> str:
    """Decode FLEX NUM raw payload: 4-bit BCD nibbles packed MSB-first."""
    bcd = _BCD_NEXT if flex_next else _BCD_OLD
    buf = bytes.fromhex(raw_hex)
    n_nibbles = bits // 4
    result = []
    for i in range(n_nibbles):
        byte = buf[i // 2]
        nib = (byte >> 4) & 0xF if i % 2 == 0 else byte & 0xF
        result.append(bcd[nib])
    return ''.join(result)


def decode_hex_bin(raw_hex: str, bits: int, blocking: int, display_rtl: bool) -> str:
    """
    Decode FLEX HEX/BIN raw payload respecting blocking (bits/char) and direction.

    blocking: B field from flex{} (0=16 bits/char, 1-15=that many bits/char).
    display_rtl: D field from flex{} (True=reverse character order for display).

    Returns a hex string of character-boundary-aligned values, space-separated,
    reversed if RTL.
    """
    bits_per_char = blocking if blocking != 0 else 16
    buf = bytes.fromhex(raw_hex)

    # Reconstruct bit stream from packed bytes (MSB-first)
    bitstream = 0
    for b in buf:
        bitstream = (bitstream << 8) | b
    total_bits = bits  # authoritative bit count

    # Extract characters from MSB end of the bit stream
    n_chars = total_bits // bits_per_char
    chars = []
    shift = total_bits - bits_per_char
    for _ in range(n_chars):
        val = (bitstream >> shift) & ((1 << bits_per_char) - 1)
        chars.append(val)
        shift -= bits_per_char

    if display_rtl:
        chars = list(reversed(chars))

    fmt = f"{{:0{(bits_per_char + 3) // 4}x}}"
    return ' '.join(fmt.format(c) for c in chars)



    """Decode FLEX NUM raw payload: 4-bit BCD nibbles packed MSB-first."""
    bcd = _BCD_NEXT if flex_next else _BCD_OLD
    buf = bytes.fromhex(raw_hex)
    n_nibbles = bits // 4
    result = []
    for i in range(n_nibbles):
        byte = buf[i // 2]
        nib = (byte >> 4) & 0xF if i % 2 == 0 else byte & 0xF
        result.append(bcd[nib])
    return ''.join(result)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def process_one(raw_hex: str, bits: int, msg_type: str = None,
                demod: str = "", capcode=None, label: str = "",
                flex_obj: dict = None):
    flex_next = "NEXT" in demod.upper()
    flex_obj  = flex_obj or {}

    addr = f"capcode={capcode}" if capcode is not None else ""
    print(f"{label}{addr}  [{msg_type or 'unknown'}]")
    print(f"  raw     ({bits:4d} bits): {raw_hex}")

    if msg_type in ("alphanumeric", "secure", "short_msg") or msg_type is None:
        decoded_aln = decode_aln(raw_hex, bits)
        decoded_num = decode_num(raw_hex, bits, flex_next=flex_next)
        print(f"  alpha            : {decoded_aln!r}")
        print(f"  numeric          : {decoded_num!r}")

    elif msg_type in ("numeric", "special_numeric", "numbered_numeric"):
        decoded_num = decode_num(raw_hex, bits, flex_next=flex_next)
        print(f"  numeric          : {decoded_num!r}")

    elif msg_type == "binary" or (msg_type and "hex" in msg_type.lower()):
        blocking    = flex_obj.get("blocking", 0)
        display_rtl = bool(flex_obj.get("display_rtl", False))
        bits_per_char = blocking if blocking != 0 else 16
        decoded_hex = decode_hex_bin(raw_hex, bits, blocking, display_rtl)
        print(f"  blocking         : {bits_per_char} bits/char"
              f"{'  (RTL — chars reversed)' if display_rtl else ''}")
        print(f"  chars            : {decoded_hex}")
        if bits_per_char == 8:
            try:
                text = bytes(int(raw_hex[i:i+2], 16) for i in range(0, (bits//4)&~1, 2))
                print(f"  ascii            : {text.decode('ascii', errors='replace')!r}")
            except Exception:
                pass

    print()


def main():
    parser = argparse.ArgumentParser(
        description="Decode FLEX raw payload bits from multimon-ng --json output."
    )
    parser.add_argument("--hex",  help="Raw hex string (standalone mode)")
    parser.add_argument("--bits", type=int, help="Bit count (standalone mode)")
    parser.add_argument("--type", choices=["aln", "num", "hex"], default="aln",
                        help="Decode type: aln (default), num, or hex")
    parser.add_argument("--blocking", type=int, default=0,
                        help="HEX blocking field (bits/char; 0=16)")
    parser.add_argument("--rtl", action="store_true",
                        help="HEX display direction: right-to-left")
    parser.add_argument("--flex-next", action="store_true",
                        help="Use FLEX_NEXT BCD table (nibble 0xA = '.')")
    args = parser.parse_args()

    if args.hex:
        if not args.bits:
            parser.error("--bits required with --hex")
        type_map = {"aln": "alphanumeric", "num": "numeric", "hex": "binary"}
        mt = type_map[args.type]
        process_one(args.hex, args.bits, msg_type=mt,
                    demod="FLEX_NEXT" if args.flex_next else "FLEX",
                    label="[cmdline] ",
                    flex_obj={"blocking": args.blocking, "display_rtl": int(args.rtl)})
        return

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        if msg.get("protocol") != "flex":
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
            msg_type=msg.get("msg_type"),
            demod=msg.get("demod", ""),
            capcode=msg.get("capcode"),
            label=f"[{msg.get('demod', 'FLEX')}] ",
            flex_obj=msg.get("flex", {}),
        )


if __name__ == "__main__":
    main()


if __name__ == "__main__":
    main()
