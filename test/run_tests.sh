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

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
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
TESTS_SKIPPED=0

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
        # Suppress Wine debug output (stderr) but capture program output
        output=$($WINE_CMD "$MULTIMON" -t "$input_type" -q -a "$decoder" "$input_file" 2>/dev/null)
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

# Function to skip a test (for tests that require sox when running Windows binaries)
skip_test() {
    local name="$1"
    local reason="$2"
    
    TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
    echo -e "Testing $name... ${YELLOW}SKIPPED${NC} ($reason)"
}

# Check if multimon-ng binary exists
if [ -z "$WINE_CMD" ]; then
    # Native binary - check if executable
    if [ ! -x "$MULTIMON" ]; then
        echo "Error: multimon-ng binary not found at $MULTIMON"
        echo "Build the project first or set MULTIMON environment variable"
        exit 1
    fi
else
    # Windows binary via Wine - check if file exists
    if [ ! -f "$MULTIMON" ]; then
        echo "Error: multimon-ng binary not found at $MULTIMON"
        echo "Build the project first or set MULTIMON environment variable"
        exit 1
    fi
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

# Tests that require sox - skip when running Windows binaries via Wine
# (sox is not available inside Wine environment)
if [ -n "$WINE_CMD" ]; then
    skip_test "X10" "requires sox (not available in Wine)"
    skip_test "POCSAG512" "requires sox (not available in Wine)"
    skip_test "POCSAG1200" "requires sox (not available in Wine)"
    skip_test "POCSAG2400" "requires sox (not available in Wine)"
else
    # Test X10 (wav format, requires sox)
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
fi

echo
if [ $TESTS_SKIPPED -gt 0 ]; then
    echo "Tests: $TESTS_PASSED/$TESTS_RUN passed, $TESTS_SKIPPED skipped"
else
    echo "Tests: $TESTS_PASSED/$TESTS_RUN passed"
fi

if [ $FAILED -ne 0 ]; then
    exit 1
fi

exit 0
