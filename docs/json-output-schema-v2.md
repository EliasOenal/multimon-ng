# multimon-ng JSON output schema v2

## Overview

When invoked with `--json`, multimon-ng emits one JSON object per decoded
protocol frame, newline-terminated (JSON Lines format), to stdout.  Each
object follows this schema.

`--json-old` retains the legacy per-demodulator JSON format for consumers
that depend on the pre-v2 output.  The two flags are mutually exclusive.

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
  "$schema":       "multimon-ng/json/v2",
  "schema_version": 2,            // int -- bump only on breaking change
  "timestamp":      1760000000,   // int64 -- Unix epoch seconds, always present
                                  // NOTE: emitted as JSON number (IEEE-754 double).
                                  //   All protocol-constrained values (timestamps,
                                  //   capcodes, group_capcodes) are <= 2^31 and
                                  //   are represented exactly. Values that could
                                  //   exceed 2^53 would require string encoding.
  "timestamp_us":   156337,       // int -- microseconds; 0 when --iso8601 not active
  "demod":          "POCSAG1200", // string -- dem_par->name (CLI token, preserves baud/variant)
  "protocol":       "pocsag",    // string -- logical protocol family, normalized lowercase
                                  //   "flex" | "pocsag" | "gsc" | "eas" | "dtmf"
                                  //   "fms" | "selcall" | "morse"
  "kind":           "message",    // string -- "message" | "event" | "diagnostic"
  "msg_type":       "alphanumeric", // string -- semantic type; see per-demod table
  "capcode":        1234567,      // int64 | null -- subscriber address (null when no address)
  "message":        "Hello",      // string | null -- best decode shortcut from encodings[]
  "encodings":      [ ... ],      // array -- all decode representations; always present, may be []
  "<demod_key>":    { ... }       // object -- protocol-specific fields; absent when not applicable
}
```

---

## `encodings` array

Order: `raw` first (when present), then text encodings in descending score order.
In POCSAG auto mode `score` is present on each text encoding.
Present for all `kind=message` frames that carry a byte payload.
Empty (`[]`) for events, diagnostics, and frames with no byte payload
(DTMF, Morse, ZVEI/selcall).

### `raw` -- protocol payload hex

```jsonc
{ "encoding": "raw", "data": "c8e5ecec6f", "bits": 35 }
```

`data`: lowercase hex of the protocol payload.  Meaning is per-protocol:
- POCSAG: the 20-bit data fields from each POCSAG codeword, packed
  consecutively into nibbles (5 nibbles = 20 bits per codeword, MSB-first).
  These are the raw data bits as received; the alpha/numeric decoders
  interpret them separately.  Not the decoded character bytes.
- FLEX ALN: 7-bit codepoints re-packed MSB-first from the decoded string
- FLEX NUM: BCD nibbles re-packed from the decoded digit string
- FLEX HEX/BIN: the raw payload bytes as-is
- FMS: `%08x%04x` of the decoded word fields (not wire bytes)
- EAS: the full SAME header string as ASCII bytes
- GSC: BCH data bytes (7 bytes per block, post-error-correction)

`bits`: total bit count of the payload; always present on `raw` entries.
Tells the consumer how many bits in the hex blob are valid content — the
last hex nibble may be zero-padded when the bit count is not a multiple of 4.

The re-pack formats (FLEX ALN/NUM, FMS) allow a consumer to verify the
decode but are not the original wire codewords.

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
| GSC alpha (single) | `raw`, `alpha` |
| GSC numeric (single) | `raw`, `numeric` |
| GSC dual-decode | `raw`, then `alpha` + `numeric` both with `score`, sorted descending |
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
  "capcode_type": "long",  // string — "short" | "long" | "temp_group" |
                           //          "network" | "info" | "operator" | "reserved"
                           //          (old FLEX: "short" | "long" | "temp_group" only)
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
    "precision_seconds\": 0.9375,                // float — worst-case clock resolution in seconds:
                                                //   7.5 without tz (sec_coarse only, 8 steps/minute),
                                                //   0.9375 with tz (sec_ext adds 8 sub-steps per coarse tick)
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

> **Note — FLEX group addressing fields** (`capcode_type`, `is_group`,
> `group_slot`, `group_capcodes`, `maildrop`): these fields reflect FLEX's
> two distinct group-delivery mechanisms, both unique to the protocol.
> See [FLEX Group Addressing](#flex-group-addressing) below for the full
> explanation.

---

### FLEX Group Addressing

#### Temporary group delivery (`capcode_type: "temp_group"`)

FLEX defines 16 *Temporary Group Address* slots — capcodes
`2,029,568–2,029,583` (address words `0x1F7800–0x1F780F`,
`addr_type 'T'` per ARIB STD-43A §3.8.1).  These addresses are not
permanently assigned to any subscriber.  They are short-lived delivery
channels, borrowed for the duration of one group transmission and
immediately freed.

The two-phase protocol (§3.8.2.3 / §5.2):

1. **SETUP** — the transmitter sends a Short Instruction message to each
   *individual* member capcode, telling that pager: "in frame F, wake on
   temporary slot X".  A group of N members receives N individual SETUP
   instructions across successive frames.
2. **DELIVERY** — in the announced frame, the actual message is
   transmitted exactly once, addressed to the temporary slot capcode.
   All enrolled pagers decode a single over-the-air transmission.

This is why the name is *temporary*: the slot assignment is per-message,
negotiated ahead of time via the SETUP phase, and the slot index is
available for reuse as soon as DELIVERY completes.  It is an
over-the-air multiplexing mechanism — not a provisioned address — which
is why the receiver observes both a burst of SETUP instructions on
individual capcodes and then a final DELIVERY on the slot address.

In the JSON output:

- `capcode_type: "temp_group"` — the decoded capcode is the temporary
  slot address (a DELIVERY frame).
- `group_slot` — 0-based slot index (0–15); `capcode − 2,029,568`.
- `group_capcodes` — array of individual member capcodes accumulated
  from the SETUP instructions seen before this DELIVERY.  Present only
  when at least one SETUP was captured in the same decoder session; may
  be absent or incomplete if the SETUP phase was not received.

#### Regular group addresses (all protocols)

There is no "group" address type in any paging protocol outside of the
temporary-group mechanism above.  A group in the ordinary sense — one
capcode shared among multiple pagers — is a pure *operator provisioning*
decision: the network operator programs the same capcode into every
pager in the group, then transmits a single message to that capcode.
From the protocol perspective the transmission is identical to a
one-to-one message; the decoder cannot distinguish a shared capcode from
an individual one.  This applies equally to POCSAG, FLEX (old decoder),
GSC, and every other paging protocol.

This means the absence of `capcode_type: "temp_group"` does **not**
imply the message is addressed to exactly one person.

#### Maildrop and info services (`maildrop`, `capcode_type: "info"`)

FLEX carries a single-bit **M (mail-drop) flag** in the vector word of
alpha and hex messages.  When `maildrop: 1`, the pager is expected to
handle the message separately from ordinary alerts — typically by
displaying it with a special "info" icon rather than triggering a
normal alert.  The flag is a pager-side display/handling hint; it does
not change the address or the over-the-air encoding.

Historically, paging operators used the maildrop flag to deliver
machine-originated content — news headlines, stock quotes, weather
bulletins — that the subscriber opted into as a feed rather than a
person-to-person message.  This is the paging equivalent of a maildrop
box: the pager checks it periodically without an alert.

FLEX also reserves a dedicated **Info Service address range**:
capcodes `2,009,088–2,025,471` (address words `0x1F2800–0x1F67FF`,
`addr_type 'I'`, ARIB STD-43A §3.8.1).  ARIB marks the use of this
range as "under study" with no standardised protocol behaviour.  In
practice, operators provision each pager with exactly one info service
capcode, chosen so that its assigned frame aligns with the pager's
collapse slot (ensuring the pager wakes for it at zero extra battery
cost).  Messages to these capcodes are sent with `maildrop=1` so the
pager renders them with the info icon.

`capcode_type: "info"` in the JSON output means only that the decoded
capcode falls in this reserved address range.  Info service behaviour
also requires `maildrop: 1`; without it the range carries no special
semantics.

For old FLEX and POCSAG, neither the maildrop flag nor any equivalent
info-service concept is present in the wire format.  A "maildrop"
subscription on those networks is indistinguishable from any other
message at the protocol level.

---

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
  "msg_type":        "alpha",  // string -- winning decode type ("alpha"|"numeric"|"tone"|"voice")
  "score":           95,       // int -- winner score (also in encodings[] for dual-decode)
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
| `POCSAG512` `POCSAG1200` `POCSAG2400` | message | `alphanumeric` `numeric` |
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

### POCSAG alphanumeric

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1791787531, "timestamp_us": 0,
  "demod": "POCSAG1200", "protocol": "pocsag", "kind": "message", "msg_type": "alphanumeric",
  "capcode": 273040,
  "message": "+++TIME=0008300324+++TIME=0008300324<NUL>",
  "encodings": [
    { "encoding": "raw",   "data": "d5ab51593668de0c1830ecc183664c5b56ad4564d9a3783060c3b3060d99316000", "bits": 260 },
    { "encoding": "alpha", "data": "+++TIME=0008300324+++TIME=0008300324<NUL>" }
  ],
  "pocsag": { "function": 3, "baud_rate": 1200, "polarity": "normal" }
}
```

### POCSAG numeric (forced mode)

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1791787531, "timestamp_us": 0,
  "demod": "POCSAG512", "protocol": "pocsag", "kind": "message", "msg_type": "numeric",
  "capcode": 273040,
  "message": "5U89 04240-4394 2284U.5-.65U.5",
  "encodings": [
    { "encoding": "raw",     "data": "ad19302420b2c9234412d5ab56ad5a", "bits": 120 },
    { "encoding": "numeric", "data": "5U89 04240-4394 2284U.5-.65U.5" }
  ],
  "pocsag": { "function": 3, "baud_rate": 512, "polarity": "normal" }
}
```

### POCSAG tone-only (event)

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1791787454, "timestamp_us": 0,
  "demod": "POCSAG1200", "protocol": "pocsag", "kind": "event", "msg_type": "tone_only",
  "capcode": 671968,
  "message": null, "encodings": [],
  "pocsag": { "function": 1, "baud_rate": 1200, "polarity": "normal" }
}
```

### FLEX_NEXT alphanumeric

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "message", "msg_type": "alphanumeric",
  "capcode": 1119003,
  "message": "P2000 TEST",
  "encodings": [
    { "encoding": "raw",   "data": "5032303030205445535400" },
    { "encoding": "alpha", "data": "P2000 TEST" }
  ],
  "flex": {
    "baud": 1600, "levels": 4, "cycle": 14, "frame": 125, "phase": "C",
    "capcode_type": "long", "is_group": false, "msg_number": 1, "k_ok": true
  }
}
```

### FLEX_NEXT tone-only (event)

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "event", "msg_type": "tone_only",
  "capcode": 1119003,
  "message": null, "encodings": [],
  "flex": { "baud": 1600, "levels": 4, "cycle": 14, "frame": 125, "phase": "C", "capcode_type": "long" }
}
```

### FLEX_NEXT BCH diagnostic

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "diagnostic", "msg_type": "bch_stats",
  "capcode": null,
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
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FLEX_NEXT", "protocol": "flex", "kind": "message", "msg_type": "numeric",
  "capcode": 1119003,
  "message": "0612345678",
  "encodings": [
    { "encoding": "raw",     "data": "0612345678" },
    { "encoding": "numeric", "data": "0612345678" }
  ],
  "flex": { "baud": 1600, "levels": 4, "cycle": 14, "frame": 125, "phase": "C", "capcode_type": "long" }
}
```

### EAS header

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "EAS", "protocol": "eas", "kind": "event", "msg_type": "eas_header",
  "capcode": null,
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
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "EAS", "protocol": "eas", "kind": "event", "msg_type": "eas_eom",
  "capcode": null,
  "message": null, "encodings": [],
  "eas": { "eom": "NNNN" }
}
```

### DTMF

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "DTMF", "protocol": "dtmf", "kind": "message", "msg_type": "dtmf",
  "capcode": null,
  "message": "5", "encodings": []
}
```

### FMS

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "FMS", "protocol": "fms", "kind": "message", "msg_type": "fms",
  "capcode": null,
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
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "ZVEI1", "protocol": "selcall", "kind": "message", "msg_type": "selcall",
  "capcode": null,
  "message": null, "encodings": [],
  "selcall": { "code": "1A2B3C" }
}
```

### Morse

```json
{
  "$schema": "multimon-ng/json/v2",
  "schema_version": 2,
  "timestamp": 1760000000, "timestamp_us": 0,
  "demod": "MORSE_CW", "protocol": "morse", "kind": "message", "msg_type": "morse",
  "capcode": null,
  "message": "HELLO", "encodings": []
}
```

One object per word (word gap = timeout). "HELLO WORLD" produces two objects.
Failed character decodes (shown as `<...>` in text mode) are dropped from
`message` in v1; the object is still emitted if any characters decoded.

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
    if obj.get("schema_version"] == 2:
        continue  # unknown schema version — skip
    if obj["kind"] not in ("message", "event", "diagnostic"):
        continue  # unknown kind — tolerate, skip
```

---

## Migration from `--json-old`

| Field (old) | Field (v2) | Notes |
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
| *(absent)* | `$schema` `schema_version` `kind` `encodings` `timestamp_us` | New |
