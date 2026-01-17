#!/bin/bash
# Test suite for multimon-ng
#
# Usage: ./test/run_tests.sh
#
# Environment variables:
#   MULTIMON   - Path to multimon-ng binary (default: ./build/multimon-ng)
#   GEN_NG     - Path to gen-ng binary (default: ./build/gen-ng)
#   WINE_CMD   - Wine command for Windows binaries (e.g., "wine64")

set -e

# Resolve directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$SCRIPT_DIR"
SAMPLES_DIR="$SCRIPT_DIR/samples"
BCH_REF_DIR="$SAMPLES_DIR/bch_reference"

# Source helper functions
source "$SCRIPT_DIR/lib/helpers.sh"

# Binary paths (can be overridden)
MULTIMON=${MULTIMON:-./build/multimon-ng}
GEN_NG=${GEN_NG:-./build/gen-ng}
WINE_CMD=${WINE_CMD:-}

# Export for helper functions
export MULTIMON GEN_NG WINE_CMD TEST_DIR

# Check binary exists
check_binary() {
    if [ -z "$WINE_CMD" ]; then
        [ -x "$1" ] || { echo "Error: $1 not found. Build first or set path."; exit 1; }
    else
        [ -f "$1" ] || { echo "Error: $1 not found."; exit 1; }
    fi
}

check_binary "$MULTIMON"

echo "Running multimon-ng tests..."
echo "Binary: $MULTIMON"
[ -n "$WINE_CMD" ] && echo "Wine: $WINE_CMD"
echo

FAILED=0

# =============================================================================
# Basic decoder tests (sample files)
# =============================================================================
echo "Basic decoder tests:"

run_test "UFSK1200" "UFSK1200" "auto" "$SAMPLES_DIR/ufsk1200.raw" \
    "N3000000000005000102000000F7" \
    "N3000400001405000106000400D7" \
    "N3001200002000000100001200BA" \
    || FAILED=1

run_test "X10" "X10" "auto" "$SAMPLES_DIR/x10rf.wav" \
    "bstring = 00110000110011110001000011101111" \
    "housecode = P 2" \
    || FAILED=1

run_test "POCSAG512" "POCSAG512" "flac" "$SAMPLES_DIR/POCSAG_sample_-_512_bps.flac" \
    "POCSAG512: Address:  273040  Function: 3  Alpha:   512 B SIDE ZZZZZZ" \
    || FAILED=1

run_test "POCSAG1200" "POCSAG1200" "auto" "$SAMPLES_DIR/POCSAG_sample_-_1200_bps.flac" \
    "POCSAG1200: Address:  273040  Function: 3  Alpha:   +++TIME=0008300324" \
    || FAILED=1

run_test "POCSAG2400" "POCSAG2400" "flac" "$SAMPLES_DIR/POCSAG_sample_-_2400_bps.flac" \
    "POCSAG2400: Address: 1022869  Function: 1  Alpha:   +++TIME=0008300324" \
    || FAILED=1

# =============================================================================
# BCH reference tests (pre-generated files for regression testing)
# =============================================================================
if [ -d "$BCH_REF_DIR" ]; then
    echo
    echo "BCH reference tests:"
    
    # FLEX BCH (mix of auto and explicit)
    run_test "FLEX BCH clean" "FLEX" "auto" "$BCH_REF_DIR/flex_clean.raw" \
        "000123456" "FLEX_REF_CLEAN" || FAILED=1
    run_test "FLEX BCH 1-bit" "FLEX" "raw" "$BCH_REF_DIR/flex_1bit.raw" \
        "000234567" "FLEX_REF_1BIT" || FAILED=1
    run_test "FLEX BCH 2-bit" "FLEX" "auto" "$BCH_REF_DIR/flex_2bit.raw" \
        "000345678" "FLEX_REF_2BIT" || FAILED=1
    
    # POCSAG BCH (mix of auto and explicit)
    run_test "POCSAG BCH clean" "POCSAG1200" "raw" "$BCH_REF_DIR/pocsag_clean.raw" \
        "Address:  111111" "POCSAG_REF_CLEAN" || FAILED=1
    run_test "POCSAG BCH 1-bit" "POCSAG1200" "auto" "$BCH_REF_DIR/pocsag_1bit.raw" \
        "Address:  222222" "POCSAG_REF_1BIT" || FAILED=1
    run_test "POCSAG BCH 2-bit" "POCSAG1200" "raw" "$BCH_REF_DIR/pocsag_2bit.raw" \
        "Address:  333333" "POCSAG_REF_2BIT" || FAILED=1
    
    # POCSAG inverted polarity BCH (mix of auto and explicit)
    run_test "POCSAG inv BCH clean" "POCSAG1200" "auto" "$BCH_REF_DIR/pocsag_inv_clean.raw" \
        "Address:  444444" "POCSAG_INV_CLEAN" || FAILED=1
    run_test "POCSAG inv BCH 1-bit" "POCSAG1200" "raw" "$BCH_REF_DIR/pocsag_inv_1bit.raw" \
        "Address:  555555" "POCSAG_INV_1BIT" || FAILED=1
    run_test "POCSAG inv BCH 2-bit" "POCSAG1200" "auto" "$BCH_REF_DIR/pocsag_inv_2bit.raw" \
        "Address:  666666" "POCSAG_INV_2BIT" || FAILED=1
fi

# =============================================================================
# End-to-end tests (gen-ng -> multimon-ng)
# =============================================================================
if [ -z "$WINE_CMD" ]; then
    [ -x "$GEN_NG" ] && GEN_NG_AVAILABLE=1 || GEN_NG_AVAILABLE=0
else
    [ -f "$GEN_NG" ] && GEN_NG_AVAILABLE=1 || GEN_NG_AVAILABLE=0
fi

if [ $GEN_NG_AVAILABLE -eq 1 ]; then
    echo
    echo "DTMF/ZVEI end-to-end tests:"
    
    run_gen_decode_test "DTMF digits" \
        '-d "123456"' "DTMF" "DTMF: 1" "DTMF: 2" "DTMF: 3" "DTMF: 4" "DTMF: 5" "DTMF: 6" || FAILED=1
    
    run_gen_decode_test "DTMF with letters" \
        '-d "0ABCD"' "DTMF" "DTMF: 0" "DTMF: A" "DTMF: B" "DTMF: C" "DTMF: D" || FAILED=1
    
    run_gen_decode_test "DTMF star pound" \
        '-d "*#"' "DTMF" "DTMF: *" "DTMF: #" || FAILED=1
    
    run_gen_decode_test "ZVEI1 sequence" \
        '-z "12345"' "ZVEI1" "ZVEI1: 12345" || FAILED=1
    
    run_gen_decode_test "ZVEI1 with E" \
        '-z "1E234"' "ZVEI1" "ZVEI1: 1E234" || FAILED=1
    
    echo
    echo "FLEX end-to-end tests:"
    
    run_gen_decode_test "FLEX short message" \
        '-f "Hi" -F 12345' "FLEX" "000012345" "ALN" "Hi" || FAILED=1
    
    run_gen_decode_test "FLEX medium message" \
        '-f "Test 123 ABC" -F 99999' "FLEX" "000099999" "Test 123 ABC" || FAILED=1
    
    run_gen_decode_test "FLEX special chars" \
        '-f "Hello World! @2024" -F 500000' "FLEX" "000500000" "Hello World! @2024" || FAILED=1
    
    run_gen_decode_test "FLEX min capcode" \
        '-f "MinCap" -F 1' "FLEX" "000000001" "MinCap" || FAILED=1
    
    run_gen_decode_test "FLEX 1-bit error" \
        '-f "Error1" -F 54321 -e 1' "FLEX" "000054321" "Error1" || FAILED=1
    
    run_gen_decode_test "FLEX 2-bit error" \
        '-f "Error2" -F 11111 -e 2' "FLEX" "000011111" "Error2" || FAILED=1
    
    run_gen_decode_expect_fail "FLEX 3-bit error (uncorrectable)" \
        '-f "Error3" -F 22222 -e 3' "FLEX" || FAILED=1
    
    run_gen_decode_test "FLEX_NEXT decoder" \
        '-f "FLEX_NEXT test" -F 777777' "FLEX_NEXT" "0000777777" "FLEX_NEXT test" || FAILED=1
    
    echo
    echo "POCSAG end-to-end tests:"
    
    run_gen_decode_test "POCSAG1200 alpha" \
        '-P "Hello" -A 12345 -B 1200' "POCSAG1200" "Address:   12345" "Hello" || FAILED=1
    
    run_gen_decode_test "POCSAG1200 alpha long" \
        '-P "Hello World! This is a test." -A 54321 -B 1200' "POCSAG1200" "Address:   54321" "Function: 3" "Alpha:" "Hello World!" || FAILED=1
    
    run_gen_decode_test "POCSAG1200 numeric" \
        '-P "1234567890" -A 67890 -B 1200 -N' "POCSAG1200" "Address:   67890" "Function: 0" "Numeric:" "1234567890" || FAILED=1
    
    run_gen_decode_test "POCSAG1200 numeric special" \
        '-P "123-456.789" -A 22222 -B 1200 -N' "POCSAG1200" "Address:   22222" "123-456.789" || FAILED=1
    
    run_gen_decode_test "POCSAG512 alpha" \
        '-P "Test 512" -A 11111 -B 512' "POCSAG512" "Address:   11111" "Test 512" || FAILED=1
    
    run_gen_decode_test "POCSAG2400 alpha" \
        '-P "Test 2400" -A 99999 -B 2400' "POCSAG2400" "Address:   99999" "Test 2400" || FAILED=1
    
    run_gen_decode_test "POCSAG1200 large address" \
        '-P "BigAddr" -A 2097151 -B 1200' "POCSAG1200" "Address: 2097151" "BigAddr" || FAILED=1
    
    run_gen_decode_test "POCSAG1200 min address" \
        '-P "MinAddr" -A 8 -B 1200' "POCSAG1200" "Address:       8" "MinAddr" || FAILED=1
    
    echo
    echo "POCSAG BCH error correction tests:"
    
    run_gen_decode_test "POCSAG 1-bit error" \
        '-P "ErrTest1" -A 11111 -e 1' "POCSAG1200" "Address:   11111" "ErrTest1" || FAILED=1
    
    run_gen_decode_test "POCSAG 2-bit error" \
        '-P "ErrTest2" -A 22222 -e 2' "POCSAG1200" "Address:   22222" "ErrTest2" || FAILED=1
    
    run_gen_decode_expect_fail "POCSAG 3-bit error (uncorrectable)" \
        '-P "ErrTest3" -A 33333 -e 3' "POCSAG1200" || FAILED=1
    
    echo
    echo "POCSAG inverted polarity tests:"
    
    run_gen_decode_test "POCSAG inverted alpha" \
        '-P "InvertTest" -A 44444 -I' "POCSAG1200" "Address:   44444" "InvertTest" || FAILED=1
    
    run_gen_decode_test "POCSAG inverted numeric" \
        '-P "9876543210" -A 55555 -I -N' "POCSAG1200" "Address:   55555" "Numeric:" "9876543210" || FAILED=1
    
    run_gen_decode_test "POCSAG inverted 1-bit error" \
        '-P "InvErr1" -A 66666 -I -e 1' "POCSAG1200" "Address:   66666" "InvErr1" || FAILED=1
    
    run_gen_decode_test "POCSAG inverted 2-bit error" \
        '-P "InvErr2" -A 77777 -I -e 2' "POCSAG1200" "Address:   77777" "InvErr2" || FAILED=1
    
    echo
    echo "WAV roundtrip tests (sox integration):"
    
    run_gen_decode_wav_test "POCSAG wav roundtrip" \
        '-P "WavTest" -A 88888' "POCSAG1200" "Address:   88888" "WavTest" || FAILED=1
    
    run_gen_decode_wav_test "FLEX wav roundtrip" \
        '-f "FlexWav" -F 1234567' "FLEX" "1234567" "FlexWav" || FAILED=1
else
    echo
    echo "Skipping end-to-end tests (gen-ng not available)"
fi

# =============================================================================
# Summary
# =============================================================================
echo
echo "Tests: $TESTS_PASSED/$TESTS_RUN passed"

[ $FAILED -ne 0 ] && exit 1
exit 0
