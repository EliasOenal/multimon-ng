#!/bin/bash
# Test helper functions for multimon-ng
# Source this file from test scripts

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# Track test results (initialize if not set)
TESTS_RUN=${TESTS_RUN:-0}
TESTS_PASSED=${TESTS_PASSED:-0}

# Filter Wine debug output
filter_wine_output() {
    grep -v "^it looks like wine32 is missing" | \
    grep -v "^multiarch needs to be enabled" | \
    grep -v "^execute \"dpkg --add-architecture" | \
    grep -v "apt-get install wine32" | \
    grep -v "^[0-9a-f]*:err:" | \
    grep -v "^wine: " | \
    grep -v "^warning: noninteger"
}

# Run multimon-ng with optional Wine wrapper
run_multimon() {
    if [ -n "$WINE_CMD" ]; then
        $WINE_CMD "$MULTIMON" "$@" 2>&1 | filter_wine_output
    else
        "$MULTIMON" "$@" 2>&1
    fi
}

# Run gen-ng with optional Wine wrapper
run_gen_ng() {
    if [ -n "$WINE_CMD" ]; then
        $WINE_CMD "$GEN_NG" "$@" 2>&1 | filter_wine_output
    else
        "$GEN_NG" "$@" 2>&1
    fi
}

# Check if output contains all expected patterns
# Arguments: output pattern1 [pattern2 ...]
# Returns: 0 if all found, 1 if any missing (sets MISSING_PATTERN)
check_patterns() {
    local output="$1"
    shift
    
    for pattern in "$@"; do
        if ! echo "$output" | grep -qF "$pattern"; then
            MISSING_PATTERN="$pattern"
            return 1
        fi
    done
    return 0
}

# Report test result
# Arguments: name passed [missing_pattern] [output]
report_result() {
    local name="$1"
    local passed="$2"
    local missing="$3"
    local output="$4"
    
    if [ "$passed" -eq 1 ]; then
        echo -e "${GREEN}PASSED${NC}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo -e "${RED}FAILED${NC}"
        [ -n "$missing" ] && echo "  Missing expected output: $missing"
        if [ -n "$output" ]; then
            echo "  Got output:"
            echo "$output" | sed 's/^/    /'
        fi
        return 1
    fi
}

# Generic test runner for sample files
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
    
    local output
    if [ -n "$WINE_CMD" ] && [ "$input_type" != "raw" ]; then
        # Non-raw input with Wine: use sox on host to convert, pipe to Wine
        output=$(sox -V1 --ignore-length -t "$input_type" "$input_file" \
            -t raw -esigned-integer -b16 -r 22050 - remix 1 2>/dev/null | \
            run_multimon -t raw -q -a "$decoder" -)
    else
        output=$(run_multimon -t "$input_type" -q -a "$decoder" "$input_file")
    fi
    
    if check_patterns "$output" "${expected_patterns[@]}"; then
        report_result "$name" 1
    else
        report_result "$name" 0 "$MISSING_PATTERN" "$output"
        return 1
    fi
}

# Generate signal with gen-ng and decode with multimon-ng
# Arguments: name gen_opts decoder expected1 [expected2 ...]
run_gen_decode_test() {
    local name="$1"
    local gen_opts="$2"
    local decoder="$3"
    shift 3
    local expected_patterns=("$@")
    
    local tmpfile="${TEST_DIR}/tmp_$$.raw"
    
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "Testing $name... "
    
    # Generate signal
    if ! eval "run_gen_ng -t raw $gen_opts \"$tmpfile\"" >/dev/null 2>&1; then
        echo -e "${RED}FAILED${NC} (gen-ng failed)"
        rm -f "$tmpfile"
        return 1
    fi
    
    # Decode signal
    local output
    output=$(run_multimon -t raw -q -a "$decoder" "$tmpfile")
    rm -f "$tmpfile"
    
    if check_patterns "$output" "${expected_patterns[@]}"; then
        report_result "$name" 1
    else
        report_result "$name" 0 "$MISSING_PATTERN" "$output"
        return 1
    fi
}

# Test that decoding FAILS (for error cases beyond correction capability)
# Arguments: name gen_opts decoder
# Verifies that decoder produces NO output (uncorrectable errors are silently dropped)
run_gen_decode_expect_fail() {
    local name="$1"
    local gen_opts="$2"
    local decoder="$3"
    
    local tmpfile="${TEST_DIR}/tmp_fail_$$.raw"
    
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "Testing $name... "
    
    # Generate signal
    if ! eval "run_gen_ng -t raw $gen_opts \"$tmpfile\"" >/dev/null 2>&1; then
        echo -e "${RED}FAILED${NC} (gen-ng failed)"
        rm -f "$tmpfile"
        return 1
    fi
    
    # Decode signal
    local output
    output=$(run_multimon -t raw -q -a "$decoder" "$tmpfile")
    rm -f "$tmpfile"
    
    # Trim whitespace and check for any decoder output
    output=$(echo "$output" | tr -d '[:space:]')
    
    if [ -n "$output" ]; then
        echo -e "${RED}FAILED${NC} (decoder produced output when it should have failed)"
        echo "  Got output:"
        echo "$output" | sed 's/^/    /'
        return 1
    fi
    
    echo -e "${GREEN}PASSED${NC} (no output, as expected)"
    TESTS_PASSED=$((TESTS_PASSED + 1))
    return 0
}
