# multimon-ng JSON output schema v1

## Overview

When invoked with `--json`, multimon-ng emits one JSON object per decoded
protocol frame, newline-terminated (JSON Lines format), to stdout.  Each
object follows this schema.

`--json-old` retains the legacy per-demodulator JSON format for consumers
that depend on the pre-v1 output.  The two flags are mutually exclusive.

---

## Design principles

1. **Fixed envelope, open payload.** Every object has the same small set of
   top-level fields.  Protocol-specific data lives in a nested object keyed by
   protocol name, absent when not applicable.

2. **`kind` classifies the frame** so consumers can route without inspecting
   payload fields: `"message"` (subscriber-directed payload), `"event"`
   (protocol control / state, no subscriber payload), `"diagnostic"` (decoder
   health).

3. **`encodings` is the canonical payload** — an ordered array of decode
   representations.  `message` is a convenience shortcut (the best decode's
   `data`).

4. **Omit unavailable fields** rather than emitting nulls inside the nested
   protocol object.  Envelope fields are always present.

5. **Unknown `kind`/`msg_type`/encoding values must be tolerated** — future
   versions add new values additively.  Consumers must not treat an unknown
   type as an error.

---

## Envelope (always present on every output line)

```jsonc
{
  "$schema":       "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,            // int — bump only on breaking change
  "timestamp":      1760000000,   // int64 — Unix epoch seconds, always present
  "timestamp_us":   156337,       // int — microseconds; 0 when --iso8601 not active
  "demod":          "POCSAG1200", // string — dem_par->name (CLI token, preserves baud/variant)
  "protocol":       "pocsag",    // string — logical protocol family, normalized lowercase
                                  //   "flex" | "pocsag" | "gsc" | "eas" | "dtmf"
                                  //   "fms" | "selcall" | "morse"
  "kind":           "message",    // string — "message" | "event" | "diagnostic"
  "msg_type":       "pocsag",     // string — semantic type; see per-demod table
  "capcode":        1234567,      // int64 | null — subscriber address (null when no address)
  "capcode_type":   "short",      // string | null — "short" | "long" | "group"
  "message":        "Hello",      // string | null — best decode shortcut from encodings[]
  "encodings":      [ ... ],      // array — all decode representations; always present, may be []
  "<demod_key>":    { ... }       // object — protocol-specific fields; absent when not applicable
}
```

---

## `encodings` array

Ordered: `raw` first, then text attempts in descending score order.
Present for all `kind=message` frames that have a wire byte buffer.
Empty (`[]`) for events, diagnostics, and frames with no byte buffer
(DTMF, Morse, ZVEI/selcall, FMS).

### `raw` — wire bytes

```jsonc
{ "encoding": "raw", "data": "c8e5ecec6f", "bits": 35 }
```

`data`: lowercase hex of the pre-decode wire bytes.
`bits`: total bit count of the raw payload; omitted when the payload is
byte-aligned (bits = len(data) × 4).

Byte packing by protocol:
- 7-bit POCSAG / FLEX ALN: 7-bit codepoints packed MSB-first into bytes
- 4-bit BCD (POCSAG numeric, FLEX NUM): packed nibbles
- FLEX HEX/BIN: raw payload bytes

### `alpha` — 7-bit alpha decode

```jsonc
{ "encoding": "alpha", "data": "Hello world", "score": 47 }
```

The decoder's 7-bit alpha table applied to the raw bytes.  For POCSAG, the
active `--pocsag-charset` trtab is used; `pocsag.charset` records which one.
`score`: heuristic confidence (higher = more likely correct), present when
heuristic selection was used (POCSAG auto/skyper modes).  Control characters
are hex-escaped (`\x03`).

### `numeric` — BCD numeric decode

```jsonc
{ "encoding": "numeric", "data": "0612345678", "score": 12 }
```

`score` present when heuristic selection was used (POCSAG auto mode).

### `skyper` — POCSAG Skyper decode

```jsonc
{ "encoding": "skyper", "data": "Gdkkn vnqkc", "score": 1 }
```

POCSAG-only.  Alpha decode with ROT-1 caesar shift.  Only present when
`--pocsag-mode skyper` or `auto`.

### Presence by protocol and message type

| Protocol / msg_type | `encodings` entries |
|---|---|
| FLEX ALN, FLEX Secure t=00 | `raw`, `alpha` |
| FLEX NUM / SNUM / NNUM | `raw`, `numeric` |
| FLEX HEX / BIN, FLEX Secure t=10 | `raw` |
| FLEX_NEXT — same rules as FLEX | same |
| POCSAG, mode=standard/alpha | `raw`, `alpha` |
| POCSAG, mode=standard/numeric | `raw`, `numeric` |
| POCSAG, mode=skyper | `raw`, `alpha`, `skyper` |
| POCSAG, mode=auto | `raw`, `alpha`, `numeric`, `skyper` (all scored) |
| GSC alpha | `raw`, `alpha` |
| GSC numeric | `raw`, `numeric` |
| EAS header | `raw`, `alpha` |
| EAS EOM | `[]` |
| DTMF, Morse, ZVEI/selcall, FMS | `[]` |
| Any `kind=event` or `kind=diagnostic` | `[]` |

---

## Protocol-specific nested objects

Present only when the frame belongs to that protocol.  Absent (key not present)
otherwise — never a null or empty object.

### `flex` — FLEX (both old decoder and FLEX_NEXT)

```jsonc
"flex": {
  // --- Common (old decoder + FLEX_NEXT) ---
  "baud":        1600,   // int — 1600 | 3200 | 6400
  "levels":      4,      // int — FSK levels: 2 | 4
  "cycle":       14,     // int — 0–127
  "frame":       125,    // int — 0–127
  "phase":       "C",    // string — "A" | "B" | "C" | "D"

  // --- FLEX_NEXT addressed messages ---
  "addr_type":    "L",   // string — "S" (short) | "L" (long) | "G" (group temp addr)
  "type_tag":     "ALN", // string — wire type tag (ALN/NUM/HEX/TON/SEC:ALN/BIW_DATE/…)
  "is_group":     false, // bool
  "is_priority":  false, // bool
  "group_slot":   3,     // int — group slot index (temp group addresses only)
  "group_capcodes": [1119003, 1119004], // array of int64 — group member capcodes
  "msg_number":   1,     // int — reassembly sequence N
  "retrieval":    0,     // int — R field (0/1)
  "maildrop":     0,     // int — M field (0/1)
  "k_ok":         true,  // bool — K checksum result
  "sig_ok":       true,  // bool — message signature result
  "fragment":     "K",   // string — fragment type (K/0/1/…)
  "frag_seq":     2,     // int — fragment sequence number
  "frag_index":   1,     // int — fragment index within sequence
  "frag_words":   8,     // int — fragment word count
  "frag_chars":   21,    // int — fragment character count
  "total_fragments": 3,  // int — total fragments in reassembly
  "total_chars":  63,    // int — total characters in reassembled message

  // --- FLEX_NEXT Secure messages ---
  "sec_subtype":  "alpha",  // string — "alpha" | "binary" | "vendor" | "reserved"
  "opr_category": "OA",     // string — operator category

  // --- FLEX_NEXT Alphanumeric fragmentation error ---
  "frag_seq_error": true,  // bool — fragment sequence mismatch detected

  // --- FLEX_NEXT Numeric special format ---
  "special_format": "id_rom",  // string — "id_rom" | "standard"

  // --- FLEX_NEXT SMSG (system message) ---
  "smsg_sub_type": "network_id", // string — "network_id"|"numeric"|"source"|"numbered"|"reserved"
  "area_id":       5,     // int — SMSG network_id area
  "multiplier":    2,     // int — SMSG network_id multiplier
  "source_code":   42,    // int — SMSG source code

  // --- FLEX_NEXT HEX/BIN messages ---
  "blocking":      8,     // int — bits per character (0 = 16)
  "display_rtl":   false, // bool — right-to-left display
  "header_msg":    false, // bool — this is a displayable header message
  "status_info":   false, // bool — first 8 data bits indicate encoding method

  // --- FLEX_NEXT FLEXTIME event ---
  "flextime": {
    "timestamp":        "2026-10-10 14:52:03",  // string — OTA decoded time
    "precision_seconds": 0.5,                   // float
    "time_mode":        "standard",             // string — "standard" | "direct"
    "date": {
      "value":       "2026-10-10",  // string
      "age_frames":  0,             // int
      "age_seconds": 0.0            // float
    },
    "time": {
      "value":       "14:52:03.5",  // string
      "sec_coarse":  3,             // int
      "age_frames":  0,
      "age_seconds": 0.0
    },
    "tz": {
      "value":        "+03:00",  // string
      "offset_min":   180,       // int
      "dst":          false,     // bool
      "zone_code":    3,         // int
      "sec_ext":      0,         // int
      "sec_combined": 3,         // int
      "age_frames":   0,
      "age_seconds":  0.0
    }
  },

  // --- FLEX_NEXT BIW system info (msg_type == "biw_*") ---
  "biw_position": 1,    // int — BIW slot position
  "lid":          12,   // int — biw_sysid local id
  "cov":          3,    // int — biw_sysid coverage zone
  "country":      81,   // int — biw_countrycode
  "tmf":          2,    // int — biw_countrycode traffic multiplier factor
  // biw_date:
  "year":         2026, // int
  "month":        10,   // int
  "day":          10,   // int
  // biw_time:
  "hour":         14,   // int
  "min":          52,   // int
  "sec":          3.5,  // float
  // biw_sysinfo:
  "a_type":       0,    // int — system info A type
  "info":         42,   // int — system info value

  // --- BCH diagnostic (msg_type == "bch_stats") ---
  "polarity":    "POS",  // string — "POS" | "NEG"
  "bch_0err":    88,     // int
  "bch_1err":    4,      // int
  "bch_2err":    1,      // int
  "bch_uncorr":  0,      // int
  "errbits":     3       // int
}
```

### `pocsag`

```jsonc
"pocsag": {
  "function":  1,         // int — raw function bits 0–3
  "charset":   "DE",      // string — active --pocsag-charset (US|DE|DK|SE|FR|SI);
                           // documents which trtab produced the alpha entry;
                           // omitted when default (US / no flag)
  "baud_rate": 1200,      // int — 512 | 1200 | 2400 (parsed from demod name)
  "polarity":  "normal"   // string — "normal" | "inverted" (detected at sync time)
}
```

### `gsc`

```jsonc
"gsc": {
  "function_suffix": "1",      // string — single-char suffix from Function Plan A/B
  "polarity":        "normal", // string — "normal" | "inverted"
  "msg_type":        "alpha",  // string — raw GSC msg_type before v1 normalization
  "score":           95,       // int — decode confidence
  "alt_type":        "numeric",// string — alternative decode type, if tried
  "alt_message":     "01234",  // string — alternative decode result
  "alt_score":       12,       // int — alternative decode score
  "blocks":          4,        // int — number of Golay blocks decoded
  "error_count":     0,        // int — corrected errors
  "uncorrectable":   0,        // int — uncorrectable codewords
  "voice_state":     "start"   // string — "start" | "end" (voice messages only)
}
```

### `eas`

```jsonc
"eas": {
  "header_begin": "ZCZC"   // string — HEADER_BEGIN token; present for eas_header
  // or:
  "eom":          "NNNN"   // string — EOM token; present for eas_eom
}
```

### `fms`

```jsonc
"fms": {
  "raw":         "0812345678ab", // string — hex of full 48-bit message
  "service_id":  1,              // int
  "service":     "POLIZEI",      // string — decoded label
  "state_id":    4,              // int
  "state":       "Berlin",       // string — decoded label
  "location":    42,             // int
  "vehicle":     "1A2B",         // string — 4-nibble vehicle id hex
  "direction":   1,              // int — 0=vehicle→ctrl, 1=ctrl→vehicle
  "short_info":  2,              // int — tactical 0–3
  "crc_ok":      true,           // bool
  "crc_swapped": false           // bool — CRC ok only after 1-bit swap
}
```

### `selcall` — ZVEI1, ZVEI2, ZVEI3, DZVEI, PZVEI

```jsonc
"selcall": {
  "code": "1A2B3C"   // string — full accumulated digit string
}
```

---

## `msg_type` values per demodulator

| `demod` | `kind` | `msg_type` values |
|---|---|---|
| `FLEX` | message | `alphanumeric` `numeric` `unknown` |
| `FLEX` | event | `tone_only` |
| `FLEX_NEXT` | message | `alphanumeric` `numeric` `special_numeric` `numbered_numeric` `binary` `secure` `short_msg` |
| `FLEX_NEXT` | event | `tone_only` `instruction` `flextime` `biw_sysid` `biw_date` `biw_time` `biw_sysinfo` `biw_countrycode` |
| `FLEX_NEXT` | diagnostic | `bch_stats` |
| `POCSAG512` `POCSAG1200` `POCSAG2400` | message | `pocsag` |
| `POCSAG512` `POCSAG1200` `POCSAG2400` | event | `tone_only` |
| `GSC` | message | `alphanumeric` `numeric` `voice` |
| `GSC` | event | `tone_only` |
| `EAS` | event | `eas_header` `eas_eom` |
| `DTMF` | message | `dtmf` |
| `FMS` | message | `fms` |
| `ZVEI1` `ZVEI2` `ZVEI3` `DZVEI` `PZVEI` | message | `selcall` |
| `MORSE_CW` | message | `morse` |

---

## Worked examples

### POCSAG, auto mode, German charset

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "POCSAG1200", "protocol": "pocsag", "kind": "message", "msg_type": "pocsag",
  "capcode": 1234567, "capcode_type": "short",
  "message": "Über alles",
  "encodings": [
    { "encoding": "raw",     "data": "dc62657220616c6c657300" },
    { "encoding": "alpha",   "data": "Über alles", "score": 52 },
    { "encoding": "numeric", "data": "3 7385-2 5338", "score": 9 },
    { "encoding": "skyper",  "data": "Tacd zkkdr",   "score": 1 }
  ],
  "pocsag": { "function": 1, "charset": "DE", "baud_rate": 1200, "polarity": "normal" }
}
```

### POCSAG tone-only (event)

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "POCSAG512", "protocol": "pocsag", "kind": "event", "msg_type": "tone_only",
  "capcode": 7654321, "capcode_type": "short",
  "message": null, "encodings": [],
  "pocsag": { "function": 3, "baud_rate": 512, "polarity": "normal" }
}
```

### FLEX_NEXT alphanumeric

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "message", "msg_type": "alphanumeric",
  "capcode": 1119003, "capcode_type": "long",
  "message": "P2000 TEST",
  "encodings": [
    { "encoding": "raw",   "data": "5032303030205445535400" },
    { "encoding": "alpha", "data": "P2000 TEST" }
  ],
  "flex": {
    "baud": 1600, "levels": 4, "cycle": 14, "frame": 125, "phase": "C",
    "is_group": false, "msg_number": 1, "k_ok": true
  }
}
```

### FLEX_NEXT tone-only (event)

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "event", "msg_type": "tone_only",
  "capcode": 1119003, "capcode_type": "long",
  "message": null, "encodings": [],
  "flex": { "baud": 1600, "levels": 4, "cycle": 14, "frame": 125, "phase": "C" }
}
```

### FLEX_NEXT BCH diagnostic

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "diagnostic", "msg_type": "bch_stats",
  "capcode": null, "capcode_type": null,
  "message": null, "encodings": [],
  "flex": {
    "baud": 1600, "levels": 4, "phase": "C", "cycle": 14, "frame": 125,
    "bch_0err": 88, "bch_1err": 4, "bch_2err": 1, "bch_uncorr": 0, "errbits": 3
  }
}
```

### FLEX_NEXT numeric

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "message", "msg_type": "numeric",
  "capcode": 1119003, "capcode_type": "long",
  "message": "0612345678",
  "encodings": [
    { "encoding": "raw",     "data": "0612345678" },
    { "encoding": "numeric", "data": "0612345678" }
  ],
  "flex": { "baud": 1600, "levels": 4, "cycle": 14, "frame": 125, "phase": "C" }
}
```

### EAS header

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "EAS", "protocol": "eas", "kind": "event", "msg_type": "eas_header",
  "capcode": null, "capcode_type": null,
  "message": "ZCZC-EAS-RWT-012057+0015-2780415-WKBT/FM-",
  "encodings": [
    { "encoding": "raw",   "data": "5a435a432d454153..." },
    { "encoding": "alpha", "data": "ZCZC-EAS-RWT-012057+0015-2780415-WKBT/FM-" }
  ],
  "eas": { "header_begin": "ZCZC" }
}
```

### EAS end-of-message

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "EAS", "protocol": "eas", "kind": "event", "msg_type": "eas_eom",
  "capcode": null, "capcode_type": null,
  "message": null, "encodings": [],
  "eas": { "eom": "NNNN" }
}
```

### DTMF

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "DTMF", "protocol": "dtmf", "kind": "message", "msg_type": "dtmf",
  "capcode": null, "capcode_type": null,
  "message": "5", "encodings": []
}
```

### FMS

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FMS", "protocol": "fms", "kind": "message", "msg_type": "fms",
  "capcode": null, "capcode_type": null,
  "message": null, "encodings": [],
  "fms": {
    "raw": "0812345678ab",
    "service_id": 1, "service": "POLIZEI",
    "state_id": 4,   "state": "Berlin",
    "location": 42,  "vehicle": "1A2B",
    "direction": 1,  "short_info": 2,
    "crc_ok": true,  "crc_swapped": false
  }
}
```

### ZVEI1

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "ZVEI1", "protocol": "selcall", "kind": "message", "msg_type": "selcall",
  "capcode": null, "capcode_type": null,
  "message": null, "encodings": [],
  "selcall": { "code": "1A2B3C" }
}
```

### Morse

```json
{
  "$schema": "https://github.com/EliasOenal/multimon-ng/blob/master/docs/json-schema-v1.json",
  "schema_version": 1,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "MORSE_CW", "protocol": "morse", "kind": "message", "msg_type": "morse",
  "capcode": null, "capcode_type": null,
  "message": "HELLO WORLD", "encodings": []
}
```

---

## Consumer patterns

```python
# Network monitor — keep everything
for line in sys.stdin:
    obj = json.loads(line)
    process(obj)

# Message display only
for line in sys.stdin:
    obj = json.loads(line)
    if obj["kind"] == "message" and obj["message"]:
        print(f"{obj['demod']}  {obj['capcode']}  {obj['message']}")

# Re-decode raw bytes with own charset table
for line in sys.stdin:
    obj = json.loads(line)
    raw = next((e for e in obj["encodings"] if e["encoding"] == "raw"), None)
    if raw:
        text = my_table.decode(bytes.fromhex(raw["data"]))

# All POCSAG heuristic attempts
for line in sys.stdin:
    obj = json.loads(line)
    for e in obj["encodings"]:
        if "score" in e:
            print(f"  {e['encoding']:10s}  score={e['score']:3d}  {e['data']}")

# FLEX diagnostics
for line in sys.stdin:
    obj = json.loads(line)
    if obj["kind"] == "diagnostic" and obj.get("flex"):
        record_bch(obj["flex"])

# Tolerate unknown kinds/types (required for future compat)
for line in sys.stdin:
    obj = json.loads(line)
    if obj.get("schema_version", 0) != 1:
        continue  # unknown schema version — skip
    if obj["kind"] not in ("message", "event", "diagnostic"):
        continue  # unknown kind — tolerate, skip
```

---

## Migration from `--json-old`

| Field (old) | Field (v1) | Notes |
|---|---|---|
| `demod_name` | `demod` | Renamed; same value |
| `protocol` (GSC) | `protocol` | Now on every frame, not just GSC |
| `capcode` (FLEX) | `capcode` | Unchanged |
| `address` (POCSAG/GSC) | `capcode` | Renamed |
| `alpha` / `numeric` / `skyper` | `message` + `encodings[]` | Unified |
| `message` (FLEX) | `encodings[alpha].data` | In array |
| `tone` / `tone_long` | `kind=event` `msg_type=tone_only` | No payload |
| `sync_baud` / `sync_level` | `flex.baud` / `flex.levels` | Nested |
| `phase_number` / `cycle_number` / `frame_number` | `flex.phase` / `flex.cycle` / `flex.frame` | Nested |
| `baud` / `level` / `phase` / `cycle` / `frame` (FLEX_NEXT) | `flex.*` | Nested under same key |
| `timestamp` (conditional string) | `timestamp` (always int64) | Always Unix epoch |
| `msg_type` e.g. `"flex_alphanumeric"` | `"alphanumeric"` | Protocol prefix dropped |
| *(absent)* | `$schema` `schema_version` `kind` `encodings` `capcode_type` `timestamp_us` | New |
