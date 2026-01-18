#!/bin/bash
# Generate multimon-ng demo audio
# Requires: gen-ng binary, sox

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
GEN_NG="${GEN_NG:-$REPO_ROOT/build/gen-ng}"
DEMO_DIR="$SCRIPT_DIR"

# Check dependencies
if [ ! -x "$GEN_NG" ]; then
    echo "Error: gen-ng not found at $GEN_NG"
    echo "Build with: mkdir build && cd build && cmake .. && make"
    exit 1
fi

if ! command -v sox &>/dev/null; then
    echo "Error: sox not found (required for wav conversion)"
    exit 1
fi

echo "Using gen-ng: $GEN_NG"
echo "Output dir: $DEMO_DIR"
echo

# Generate 300ms silence for gaps between segments
generate_silence() {
    local samples=$((22050 * 3 / 10))  # 300ms at 22050 Hz
    dd if=/dev/zero bs=2 count=$samples 2>/dev/null
}

# Generate 2s silence for end
generate_silence_2s() {
    local samples=$((22050 * 2))  # 2s at 22050 Hz
    dd if=/dev/zero bs=2 count=$samples 2>/dev/null
}

echo "=== Generating scope title ==="
"$GEN_NG" -S " MULTIMON-NG      " -t raw "$DEMO_DIR/scope_demo.raw"

echo "=== Generating message segments ==="

echo "seg1: POCSAG512 address 1996"
"$GEN_NG" -P "Born in 1996 as multimon and revived in 2012, multimon-ng decodes the radio signals most people forget exist." \
    -A 1996 -B 512 -t raw "$DEMO_DIR/seg1_pocsag512.raw"

echo "seg2: POCSAG1200 address 911411"
"$GEN_NG" -P "Pagers still beep in hospitals, FLEX networks quietly serve first responders," \
    -A 911411 -B 1200 -t raw "$DEMO_DIR/seg2_pocsag1200.raw"

echo "seg3: POCSAG512 address 2012"
"$GEN_NG" -P "and weather alerts broadcast whether anyone is listening. This tool makes them readable." \
    -A 2012 -B 512 -t raw "$DEMO_DIR/seg3_pocsag512.raw"

echo "seg4: POCSAG2400 address 144390"
"$GEN_NG" -P "Ham operators use it to monitor APRS and packet radio." \
    -A 144390 -B 2400 -t raw "$DEMO_DIR/seg4_pocsag2400.raw"

echo "seg5: FLEX capcode 8675309"
"$GEN_NG" -f "Security researchers poke at paging infrastructure." \
    -F 8675309 -t raw "$DEMO_DIR/seg5_flex.raw"

echo "seg6: FLEX capcode 162550"
"$GEN_NG" -f "Weather nerds track EAS alerts. Some folks just enjoy watching DTMF tones scroll by." \
    -F 162550 -t raw "$DEMO_DIR/seg6_flex.raw"

echo "seg7: POCSAG2400 address 73"
"$GEN_NG" -P "Hook up a radio to your sound card, pipe in an SDR, and see what you find." \
    -A 73 -B 2400 -t raw "$DEMO_DIR/seg7_pocsag2400.raw"

echo "=== Generating silence ==="
generate_silence_2s > "$DEMO_DIR/silence_2s.raw"

echo "=== Concatenating segments ==="
{
    cat "$DEMO_DIR/scope_demo.raw"
    generate_silence
    cat "$DEMO_DIR/seg1_pocsag512.raw"
    generate_silence
    cat "$DEMO_DIR/seg2_pocsag1200.raw"
    generate_silence
    cat "$DEMO_DIR/seg3_pocsag512.raw"
    generate_silence
    cat "$DEMO_DIR/seg4_pocsag2400.raw"
    generate_silence
    cat "$DEMO_DIR/seg5_flex.raw"
    generate_silence
    cat "$DEMO_DIR/seg6_flex.raw"
    generate_silence
    cat "$DEMO_DIR/seg7_pocsag2400.raw"
    cat "$DEMO_DIR/silence_2s.raw"
} > "$DEMO_DIR/full_demo.raw"

echo "=== Converting to WAV ==="
sox -R -t raw -e signed-integer -b 16 -r 22050 -c 1 \
    "$DEMO_DIR/full_demo.raw" "$DEMO_DIR/full_demo_clean.wav"

echo "=== Adding 4% noise ==="
sox "$DEMO_DIR/full_demo_clean.wav" -p synth whitenoise vol 0.04 | \
    sox -m "$DEMO_DIR/full_demo_clean.wav" - -p | \
    sox - "$DEMO_DIR/full_demo.wav" vol 2

rm -f "$DEMO_DIR/full_demo_clean.wav"

echo
echo "=== Done ==="
ls -lh "$DEMO_DIR/full_demo.wav"
echo
echo "Play with:"
echo "  ./build/multimon-ng -q -P normal -a FLEX -a POCSAG512 -a POCSAG1200 -a POCSAG2400 -a SDL_SCOPE demo/full_demo.wav"
