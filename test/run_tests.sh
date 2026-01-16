#!/bin/bash
# Test script for multimon-ng
# Runs decoder tests against sample files and validates output
#
# Environment variables:
#   MULTIMON     - Path to multimon-ng binary (default: ./build/multimon-ng)
#   EXAMPLE_DIR  - Path to example files (default: ./example)
#   WINE_CMD     - Wine command to use for Windows binaries (e.g., "wine" or "wine64")
#                  When set, the script runs in Windows binary test mode

set -e

# Get script directory (for temp files)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# Path to multimon-ng binary (can be overridden)
MULTIMON=${MULTIMON:-./build/multimon-ng}

# Path to example files
EXAMPLE_DIR=${EXAMPLE_DIR:-./example}

# Wine command (empty for native Linux binaries)
WINE_CMD=${WINE_CMD:-}

# Track test results
TESTS_RUN=0
TESTS_PASSED=0

# Helper function to filter Wine debug output
filter_wine_output() {
    grep -v "^it looks like wine32 is missing" | \
    grep -v "^multiarch needs to be enabled" | \
    grep -v "^execute \"dpkg --add-architecture" | \
    grep -v "^[0-9a-f]*:err:" | \
    grep -v "^wine: "
}

# Function to run a test and check output contains all expected strings
# Arguments: name decoder input_type input_file expected1 [expected2 ...]
run_test() {
    local name="$1"
    local decoder="$2"
    local input_type="$3"
    local input_file="$4"
    shift 4
    local expected_patterns=("$@")
    
    TESTS_RUN=$((TESTS_RUN + 1))
    
    echo -n "Testing $name... "
    
    # Run the command, with Wine if specified
    if [ -n "$WINE_CMD" ]; then
        if [ "$input_type" = "raw" ]; then
            # Raw input can be passed directly to Wine
            output=$($WINE_CMD "$MULTIMON" -t raw -q -a "$decoder" "$input_file" 2>&1 | filter_wine_output)
        else
            # Non-raw input: use sox on Linux side to convert to raw, then pipe to Wine
            # This matches how multimon-ng internally calls sox (see unixinput.c)
            # Create a temporary file to capture sox errors for better debugging
            local sox_err
            sox_err=$(mktemp)
            output=$(sox -V1 --ignore-length -t "$input_type" "$input_file" \
                -t raw -esigned-integer -b16 -r 22050 - remix 1 2>"$sox_err" | \
                $WINE_CMD "$MULTIMON" -t raw -q -a "$decoder" - 2>&1 | filter_wine_output)
            local sox_errors
            sox_errors=$(cat "$sox_err")
            rm -f "$sox_err"
            # If sox produced errors (not just info messages), include them in output for debugging
            if [ -n "$sox_errors" ] && echo "$sox_errors" | grep -qiE "error|fail|cannot|unable"; then
                output="sox error: $sox_errors
$output"
            fi
        fi
    else
        output=$("$MULTIMON" -t "$input_type" -q -a "$decoder" "$input_file" 2>&1)
    fi
    
    local all_found=1
    local missing=""
    for pattern in "${expected_patterns[@]}"; do
        if ! echo "$output" | grep -qF "$pattern"; then
            all_found=0
            missing="$pattern"
            break
        fi
    done
    
    if [ $all_found -eq 1 ]; then
        echo -e "${GREEN}PASSED${NC}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        echo "  Missing expected output: $missing"
        echo "  Got output:"
        echo "$output" | sed 's/^/    /'
        return 1
    fi
}

# Helper function to display binary not found error
binary_not_found_error() {
    echo "Error: multimon-ng binary not found at $MULTIMON"
    echo "Build the project first or set MULTIMON environment variable"
    exit 1
}

# Check if multimon-ng binary exists
if [ -z "$WINE_CMD" ]; then
    # Native binary - check if executable
    [ -x "$MULTIMON" ] || binary_not_found_error
else
    # Windows binary via Wine - check if file exists
    [ -f "$MULTIMON" ] || binary_not_found_error
fi

# Check if example directory exists
if [ ! -d "$EXAMPLE_DIR" ]; then
    echo "Error: example directory not found at $EXAMPLE_DIR"
    exit 1
fi

echo "Running multimon-ng tests..."
echo "Binary: $MULTIMON"
echo "Example dir: $EXAMPLE_DIR"
if [ -n "$WINE_CMD" ]; then
    echo "Wine: $WINE_CMD (Windows binary test mode)"
fi
echo

# Track if any test failed
FAILED=0

# Test UFSK1200 (raw format, no sox needed)
# Verify multiple decoded packets from the sample
run_test "UFSK1200" "UFSK1200" "raw" "$EXAMPLE_DIR/ufsk1200.raw" \
    "N3000000000005000102000000F7" \
    "N3000400001405000106000400D7" \
    "N3001200002000000100001200BA" \
    "N3002A00002418000104002A006A" \
    "N3003D00002800000405003D0054" \
    || FAILED=1

# Test X10 (wav format, requires sox)
# For Wine tests: sox runs on Linux side, converts to raw, pipes to wine multimon-ng
# Verify bit string, decoded bytes, and housecode
run_test "X10" "X10" "wav" "$EXAMPLE_DIR/x10rf.wav" \
    "bstring = 00110000110011110001000011101111" \
    "bytes = 00001100 11110011 00001000 11110111" \
    "0C F3 08 F7" \
    "housecode = P 2" \
    || FAILED=1

# Test POCSAG512 (flac format, requires sox)
# Verify address, function, and full alpha message
run_test "POCSAG512" "POCSAG512" "flac" "$EXAMPLE_DIR/POCSAG_sample_-_512_bps.flac" \
    "POCSAG512: Address:  273040  Function: 3  Alpha:   512 B SIDE ZZZZZZ" \
    || FAILED=1

# Test POCSAG1200 (flac format, requires sox)
# Verify both messages decoded from the sample
run_test "POCSAG1200" "POCSAG1200" "flac" "$EXAMPLE_DIR/POCSAG_sample_-_1200_bps.flac" \
    "POCSAG1200: Address:  273040  Function: 3  Alpha:   +++TIME=0008300324+++TIME=0008300324" \
    "POCSAG1200: Address:  671968  Function: 1" \
    || FAILED=1

# Test POCSAG2400 (flac format, requires sox)
# Verify address, function, and full alpha message with timestamp
run_test "POCSAG2400" "POCSAG2400" "flac" "$EXAMPLE_DIR/POCSAG_sample_-_2400_bps.flac" \
    "POCSAG2400: Address: 1022869  Function: 1  Alpha:   +++TIME=0008300324+++TIME=0008300324" \
    || FAILED=1

# FLEX End-to-End Tests (using gen-ng to generate test signals)
# These tests verify the full encode/decode chain for FLEX paging

# Path to gen-ng binary
GEN_NG=${GEN_NG:-./build/gen-ng}

# Check if gen-ng is available for FLEX tests
if [ -z "$WINE_CMD" ]; then
    GEN_NG_AVAILABLE=0
    [ -x "$GEN_NG" ] && GEN_NG_AVAILABLE=1
else
    GEN_NG_AVAILABLE=0
    GEN_NG=${GEN_NG:-./build-mingw32/gen-ng.exe}
    [ -f "$GEN_NG" ] && GEN_NG_AVAILABLE=1
    # For Wine, also check mingw64 path
    if [ $GEN_NG_AVAILABLE -eq 0 ]; then
        GEN_NG=${GEN_NG:-./build-mingw64/gen-ng.exe}
        [ -f "$GEN_NG" ] && GEN_NG_AVAILABLE=1
    fi
fi

# Helper function to generate and test FLEX signal
# Arguments: test_name message capcode errors expected_patterns...
run_flex_test() {
    local name="$1"
    local message="$2"
    local capcode="$3"
    local errors="$4"
    shift 4
    local expected_patterns=("$@")
    
    # Use test directory for temp files to stay within project
    local tmpfile="${SCRIPT_DIR}/flex_test_$$.raw"
    
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "Testing $name... "
    
    # Generate the FLEX signal
    local gen_cmd
    if [ -n "$WINE_CMD" ]; then
        gen_cmd="$WINE_CMD $GEN_NG"
    else
        gen_cmd="$GEN_NG"
    fi
    
    local gen_opts="-t raw"
    [ -n "$message" ] && gen_opts="$gen_opts -f \"$message\""
    [ -n "$capcode" ] && gen_opts="$gen_opts -F $capcode"
    [ -n "$errors" ] && [ "$errors" != "0" ] && gen_opts="$gen_opts -e $errors"
    
    # Run generator
    if ! eval "$gen_cmd $gen_opts \"$tmpfile\"" >/dev/null 2>&1; then
        echo -e "${RED}FAILED${NC} (gen-ng failed)"
        rm -f "$tmpfile"
        return 1
    fi
    
    # Run decoder
    local output
    if [ -n "$WINE_CMD" ]; then
        output=$($WINE_CMD "$MULTIMON" -t raw -q -a FLEX "$tmpfile" 2>&1 | filter_wine_output)
    else
        output=$("$MULTIMON" -t raw -q -a FLEX "$tmpfile" 2>&1)
    fi
    
    rm -f "$tmpfile"
    
    local all_found=1
    local missing=""
    for pattern in "${expected_patterns[@]}"; do
        if ! echo "$output" | grep -qF "$pattern"; then
            all_found=0
            missing="$pattern"
            break
        fi
    done
    
    if [ $all_found -eq 1 ]; then
        echo -e "${GREEN}PASSED${NC}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        echo "  Missing expected output: $missing"
        echo "  Got output:"
        echo "$output" | sed 's/^/    /'
        return 1
    fi
}

# Helper function to test FLEX signal with errors that should NOT decode correctly
# Arguments: test_name message capcode errors
run_flex_test_expect_fail() {
    local name="$1"
    local message="$2"
    local capcode="$3"
    local errors="$4"
    
    # Use test directory for temp files to stay within project
    local tmpfile="${SCRIPT_DIR}/flex_test_fail_$$.raw"
    
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "Testing $name... "
    
    # Generate the FLEX signal with errors
    local gen_cmd
    if [ -n "$WINE_CMD" ]; then
        gen_cmd="$WINE_CMD $GEN_NG"
    else
        gen_cmd="$GEN_NG"
    fi
    
    local gen_opts="-t raw"
    [ -n "$message" ] && gen_opts="$gen_opts -f \"$message\""
    [ -n "$capcode" ] && gen_opts="$gen_opts -F $capcode"
    [ -n "$errors" ] && [ "$errors" != "0" ] && gen_opts="$gen_opts -e $errors"
    
    # Run generator
    if ! eval "$gen_cmd $gen_opts \"$tmpfile\"" >/dev/null 2>&1; then
        echo -e "${RED}FAILED${NC} (gen-ng failed)"
        rm -f "$tmpfile"
        return 1
    fi
    
    # Run decoder
    local output
    if [ -n "$WINE_CMD" ]; then
        output=$($WINE_CMD "$MULTIMON" -t raw -q -a FLEX "$tmpfile" 2>&1 | filter_wine_output)
    else
        output=$("$MULTIMON" -t raw -q -a FLEX "$tmpfile" 2>&1)
    fi
    
    rm -f "$tmpfile"
    
    # Check that the original message does NOT appear (errors should corrupt it)
    if echo "$output" | grep -qF "$message"; then
        echo -e "${RED}FAILED${NC} (message decoded despite $errors-bit errors)"
        echo "  Expected: decode failure or corrupted message"
        echo "  Got output:"
        echo "$output" | sed 's/^/    /'
        return 1
    fi
    
    echo -e "${GREEN}PASSED${NC} (message not decoded as expected)"
    TESTS_PASSED=$((TESTS_PASSED + 1))
    return 0
}

# Helper function to test FLEX_NEXT decoder
run_flex_next_test() {
    local name="$1"
    local message="$2"
    local capcode="$3"
    shift 3
    local expected_patterns=("$@")
    
    # Use test directory for temp files to stay within project
    local tmpfile="${SCRIPT_DIR}/flex_next_test_$$.raw"
    
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "Testing $name... "
    
    # Generate the FLEX signal
    local gen_cmd
    if [ -n "$WINE_CMD" ]; then
        gen_cmd="$WINE_CMD $GEN_NG"
    else
        gen_cmd="$GEN_NG"
    fi
    
    local gen_opts="-t raw"
    [ -n "$message" ] && gen_opts="$gen_opts -f \"$message\""
    [ -n "$capcode" ] && gen_opts="$gen_opts -F $capcode"
    
    # Run generator
    if ! eval "$gen_cmd $gen_opts \"$tmpfile\"" >/dev/null 2>&1; then
        echo -e "${RED}FAILED${NC} (gen-ng failed)"
        rm -f "$tmpfile"
        return 1
    fi
    
    # Run FLEX_NEXT decoder
    local output
    if [ -n "$WINE_CMD" ]; then
        output=$($WINE_CMD "$MULTIMON" -t raw -q -a FLEX_NEXT "$tmpfile" 2>&1 | filter_wine_output)
    else
        output=$("$MULTIMON" -t raw -q -a FLEX_NEXT "$tmpfile" 2>&1)
    fi
    
    rm -f "$tmpfile"
    
    local all_found=1
    local missing=""
    for pattern in "${expected_patterns[@]}"; do
        if ! echo "$output" | grep -qF "$pattern"; then
            all_found=0
            missing="$pattern"
            break
        fi
    done
    
    if [ $all_found -eq 1 ]; then
        echo -e "${GREEN}PASSED${NC}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        echo "  Missing expected output: $missing"
        echo "  Got output:"
        echo "$output" | sed 's/^/    /'
        return 1
    fi
}

if [ $GEN_NG_AVAILABLE -eq 1 ]; then
    echo
    echo "FLEX end-to-end tests (gen-ng -> multimon-ng):"
    
    # Test 1: Short message
    run_flex_test "FLEX short message" "Hi" "12345" "0" \
        "000012345" "ALN" "Hi" \
        || FAILED=1
    
    # Test 2: Medium message with varied characters
    run_flex_test "FLEX medium message" "Test 123 !@# ABC xyz" "99999" "0" \
        "000099999" "ALN" "Test 123 !@# ABC xyz" \
        || FAILED=1
    
    # Test 3: Maximum length message (251 chars)
    MAX_MSG="MAX_LENGTH_TEST:$(printf 'X%.0s' {1..235})"
    run_flex_test "FLEX max length (251)" "$MAX_MSG" "1000000" "0" \
        "001000000" "ALN" "MAX_LENGTH_TEST:" \
        || FAILED=1
    
    # Test 4: BCH 1-bit error correction
    run_flex_test "FLEX 1-bit error correction" "Error1" "54321" "1" \
        "000054321" "ALN" "Error1" \
        || FAILED=1
    
    # Test 5: BCH 2-bit error correction
    run_flex_test "FLEX 2-bit error correction" "Error2" "11111" "2" \
        "000011111" "ALN" "Error2" \
        || FAILED=1
    
    # Test 6: BCH 3-bit errors should NOT be correctable (exceeds t=2 capability)
    # This test verifies that 3-bit errors cause decode failure
    run_flex_test_expect_fail "FLEX 3-bit error (uncorrectable)" "Error3" "22222" "3" \
        || FAILED=1
    
    # Test 7: FLEX_NEXT decoder compatibility
    run_flex_next_test "FLEX_NEXT decoder" "FLEX_NEXT test" "777777" \
        "0000777777" "ALN" "FLEX_NEXT test" \
        || FAILED=1
    
    # Test 8: Special characters in message
    run_flex_test "FLEX special chars" "Hello World! @2024" "500000" "0" \
        "000500000" "ALN" "Hello World! @2024" \
        || FAILED=1
    
    # Test 9: Minimum capcode
    run_flex_test "FLEX min capcode" "MinCap" "1" "0" \
        "000000001" "ALN" "MinCap" \
        || FAILED=1
    
else
    echo
    echo "Skipping FLEX end-to-end tests (gen-ng not available at $GEN_NG)"
fi

echo
echo "Tests: $TESTS_PASSED/$TESTS_RUN passed"

if [ $FAILED -ne 0 ]; then
    exit 1
fi

exit 0
