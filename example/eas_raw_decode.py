#!/usr/bin/env python3
"""
example/eas_raw_decode.py — decode EAS raw payload bytes from multimon-ng
--json output (schema v2).

The "raw" encoding in an EAS header message contains the full SAME
(Specific Area Message Encoding) header string as ASCII bytes, hex-encoded.
This is identical to the "alpha" encoding for standard ASCII content.

The raw encoding is useful when the consumer wants to re-parse the SAME
fields directly (event code, areas, duration, originator) without relying
on the multimon-ng alpha decode.

SAME header format (ECFM/FCC):
  ZCZC-ORG-EEE-PSSCCC+TTTT-JJJHHMM-LLLLLLLL-
  ORG  = originator (EAS/CIV/WXR/PEP)
  EEE  = event code (RWT=Required Weekly Test, etc.)
  PSS  = state FIPS, CCC = county FIPS (repeats for each area)
  TTTT = duration in hours/minutes (HHMM)
  JJJ  = Julian day, HHMM = time, LLLLLLLL = originator ID

Usage (decode from live multimon-ng output):

    multimon-ng -t flac -a EAS --json capture.flac \\
        | python3 example/eas_raw_decode.py

Usage (decode a single raw value from the command line):

    python3 example/eas_raw_decode.py \\
        --hex 5a435a432d454153...
"""

import sys
import json
import argparse
import re


# ---------------------------------------------------------------------------
# SAME field parser
# ---------------------------------------------------------------------------

def parse_same(header: str) -> dict:
    """Parse a SAME header string into its component fields."""
    # Strip ZCZC prefix and trailing -
    s = header.strip().lstrip('Z').lstrip('C').lstrip('Z').lstrip('C').lstrip('-')
    # More robust: split on known structure
    m = re.match(
        r'ZCZC-([A-Z]{3})-([A-Z]{3})-([\d\+/-]+)\+(\d{4})-(\d{3})(\d{4})-(.+?)-?$',
        header.strip()
    )
    if not m:
        return {"raw_string": header.strip(), "parse_error": "unrecognized format"}
    org, event, areas_raw, duration, jday, time, originator = m.groups()
    areas = areas_raw.split('-') if areas_raw else []
    return {
        "originator":  org,
        "event":       event,
        "areas":       areas,
        "duration":    f"{duration[:2]}h{duration[2:]}m",
        "julian_day":  int(jday),
        "time_utc":    f"{time[:2]}:{time[2:]}",
        "station_id":  originator,
    }


def process_one(raw_hex: str, capcode=None, label: str = ""):
    try:
        text = bytes.fromhex(raw_hex).decode('ascii', errors='replace')
    except ValueError as e:
        print(f"{label}decode error: {e}")
        return

    parsed = parse_same(text)
    addr = f"capcode={capcode}" if capcode is not None else ""
    print(f"{label}{addr}")
    print(f"  raw  : {raw_hex}")
    print(f"  ascii: {text!r}")
    for k, v in parsed.items():
        if k not in ("raw_string", "parse_error"):
            print(f"  {k:12s}: {v}")
    if "parse_error" in parsed:
        print(f"  parse_error  : {parsed['parse_error']}")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Decode EAS raw payload bytes from multimon-ng --json output."
    )
    parser.add_argument("--hex", help="Raw hex string (standalone mode)")
    args = parser.parse_args()

    if args.hex:
        process_one(args.hex, label="[cmdline] ")
        return

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        if msg.get("protocol") != "eas":
            continue
        if msg.get("msg_type") != "eas_header":
            continue

        raw_entry = next(
            (e for e in msg.get("encodings", []) if e["encoding"] == "raw"),
            None
        )
        if not raw_entry:
            continue

        process_one(
            raw_entry["data"],
            capcode=msg.get("capcode"),
            label=f"[{msg.get('demod', 'EAS')}] ",
        )


if __name__ == "__main__":
    main()
