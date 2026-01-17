---
applyTo: "test/**/*.sh,test/**/*.raw"
---

# Test Development Guidelines

## Test Structure

Tests are located in `test/` directory:
- `run_tests.sh` - Main test runner
- `lib/helpers.sh` - Shared test functions
- `samples/` - Test audio files and reference data

## Running Tests

```bash
# Native tests (42 tests)
./test/run_tests.sh

# Windows builds with Wine
WINE_CMD=wine MULTIMON=./build-mingw64/multimon-ng.exe GEN_NG=./build-mingw64/gen-ng.exe ./test/run_tests.sh
```

## Test Categories

- Basic decoders (UFSK1200, X10, POCSAG512/1200/2400)
- BCH error correction regression tests (FLEX and POCSAG, 0-2 bit errors)
- End-to-end encode/decode (gen-ng → multimon-ng)
- POCSAG inverted polarity
- DTMF/ZVEI tone decoding (skipped under Wine - known issue)

## Adding New Tests

1. Use helper functions from `test/lib/helpers.sh`
2. Add test samples to `test/samples/` if needed
3. Follow existing test patterns for consistency

## Known Issues

- DTMF/ZVEI tests fail under Wine - these are automatically skipped when `WINE_CMD` is set
- Use `-t raw` for raw input files to avoid sox dependency issues
