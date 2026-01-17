# Copilot Agent Instructions for multimon-ng

## Repository Overview

**multimon-ng** is a C-based command-line tool that decodes various digital radio transmission modes including POCSAG (512/1200/2400), FLEX, DTMF, ZVEI, EAS, AFSK, X10, Morse, and more. **gen-ng** is the companion signal generator for testing.

- **Language**: C (GNU11 standard)
- **License**: GPL-2.0
- **Build**: CMake (primary), qmake (secondary)
- **Platforms**: Linux, macOS, Windows (MinGW)

## Project Structure

```
/                       # Source files (flat structure)
├── CMakeLists.txt      # Build configuration
├── multimon-ng.pro     # qmake build configuration
├── multimon.h          # Demodulator definitions and ALL_DEMOD macro
├── unixinput.c         # multimon-ng main()
├── gen.c               # gen-ng main()
├── gen.h               # Generator declarations
├── demod_*.c           # Demodulators (one per protocol)
├── gen_*.c             # Signal generators
├── bch.[ch]            # BCH error correction library (FLEX/POCSAG)
├── pocsag.c            # POCSAG decoder (uses bch.c)
├── cJSON.[ch]          # JSON output support (third-party)
├── test/
│   ├── run_tests.sh    # Test runner (42 tests)
│   ├── lib/helpers.sh  # Shared test functions
│   └── samples/        # Test audio files and BCH reference data
├── cmake/              # Cross-compilation toolchains
└── unsupported/        # Legacy Makefile and VS project files
```

## Build Instructions

### CMake Build (Recommended)

Always use an out-of-source build:

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
```

Produces `build/multimon-ng` and `build/gen-ng`. Build time is under 30 seconds.

**Notes:**
- Build warnings (format specifiers, unused variables) are normal and harmless
- X11 and PulseAudio are optional; build succeeds without them (uses DUMMY_AUDIO)
- If `bch.c` is missing, `bch_stub.c` is used (disables BCH error correction)

### Cross-Compilation for Windows

```bash
# 64-bit Windows
mkdir build-mingw64 && cd build-mingw64
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw64.cmake
make

# 32-bit Windows  
mkdir build-mingw32 && cd build-mingw32
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw32.cmake
make
```

### Clean Rebuild

```bash
rm -rf build && mkdir build && cd build && cmake .. && make -j$(nproc)
```

## Testing

Run the test suite before submitting changes:

```bash
# Native tests (42 tests)
./test/run_tests.sh

# Windows builds with Wine
WINE_CMD=wine MULTIMON=./build-mingw64/multimon-ng.exe GEN_NG=./build-mingw64/gen-ng.exe ./test/run_tests.sh
```

**Test categories:**
- Basic decoders (UFSK1200, X10, POCSAG512/1200/2400)
- BCH error correction regression tests (FLEX and POCSAG, 0-2 bit errors)
- End-to-end encode/decode (gen-ng → multimon-ng)
- POCSAG inverted polarity
- DTMF/ZVEI tone decoding (skipped under Wine - known issue)

**Manual testing:**
```bash
# Test with raw file
./build/multimon-ng -t raw -q -a POCSAG1200 ./test/samples/pocsag_ref.raw

# Test with wav/flac (requires sox)
./build/multimon-ng -t wav -q -a X10 ./test/samples/x10rf.wav
```

## gen-ng Signal Generator

Generate test signals for protocol testing:

```bash
# POCSAG
./build/gen-ng -P "Hello" -A 12345 -B 1200 -t raw out.raw      # Alpha message
./build/gen-ng -P "123" -A 12345 -B 1200 -N -t raw out.raw     # Numeric (-N)
./build/gen-ng -P "Test" -A 12345 -B 512 -I -t raw out.raw     # Inverted (-I), 512 baud

# FLEX
./build/gen-ng -f "Message" -F 1234567 -t raw out.raw

# DTMF/ZVEI
./build/gen-ng -d "123#" -t raw out.raw                        # DTMF
./build/gen-ng -z "12345" -t raw out.raw                       # ZVEI

# BCH error injection for testing error correction
./build/gen-ng -e 2 -P "Test" -A 1000 -B 1200 -t raw out.raw   # 2-bit errors per codeword

# See all options
./build/gen-ng -h
```

**Key gen-ng options:**
| Option | Description |
|--------|-------------|
| `-P <msg>` | POCSAG message |
| `-A <addr>` | POCSAG address (0-2097151) |
| `-B <baud>` | POCSAG baud rate (512/1200/2400) |
| `-N` | Numeric encoding (POCSAG) |
| `-I` | Inverted polarity (POCSAG) |
| `-f <msg>` | FLEX message |
| `-F <cap>` | FLEX capcode |
| `-d <str>` | DTMF digits |
| `-z <str>` | ZVEI digits |
| `-e <0-3>` | Inject bit errors per codeword |
| `-t <type>` | Output type (raw, wav, etc.) |

## CI/CD Workflows

Four GitHub Actions workflows run on every push:

| Workflow | File | Description |
|----------|------|-------------|
| C/C++ CI cmake | cmake.yml | Linux CMake build + full test suite |
| C/C++ CI qmake | qmake.yml | Linux qmake build |
| MinGW32 Cross-Compile | mingw32.yml | Windows 32-bit + Wine tests |
| MinGW64 Cross-Compile | mingw64.yml | Windows 64-bit + Wine tests |

**All workflows must pass.** The cmake workflow is the primary validation.

## Code Conventions

- **C Standard**: GNU11 (`-std=gnu11`)
- **Compiler Flags**: `-Wall -Wextra`
- **Naming**: snake_case for functions and variables
- **File Organization**: All source files in root directory (flat structure)
- **Platform Conditionals**: `#ifdef WIN32`, `#ifdef _MSC_VER`

### Adding New Demodulators

1. Create `demod_<name>.c` with the demodulator implementation
2. Define `const struct demod_param demod_<name>` in the file
3. Add `extern const struct demod_param demod_<name>;` to `multimon.h`
4. Add `&demod_<name>` to the `ALL_DEMOD` macro in `multimon.h`
5. Add source file to `CMakeLists.txt` (SOURCES list) and `multimon-ng.pro`

### Adding New Generators

1. Create `gen_<name>.c` with `gen_init_<name>()` and `gen_<name>()` functions
2. Add function declarations to `gen.h`
3. Add command-line option handling to `gen.c`
4. Add source file to `CMakeLists.txt` (GEN_SOURCES list)

## Key Files Reference

| File | Purpose |
|------|---------|
| `unixinput.c` | multimon-ng entry point, CLI parsing, audio input handling |
| `gen.c` | gen-ng entry point, CLI parsing |
| `multimon.h` | Demodulator struct definitions, ALL_DEMOD macro |
| `gen.h` | Generator struct definitions |
| `bch.c` | Unified BCH(31,21,2) error correction for FLEX and POCSAG |
| `bch.h` | BCH API: `bch_init()`, `bch_flex_*()`, `bch_pocsag_*()` |
| `pocsag.c` | POCSAG decoder implementation |
| `demod_flex.c` | FLEX decoder implementation |
| `gen_pocsag.c` | POCSAG signal generator |
| `gen_flex.c` | FLEX signal generator |
| `CMakeLists.txt` | Build config, source lists, platform detection |
| `test/run_tests.sh` | Test suite entry point |
| `test/lib/helpers.sh` | Shared test functions (run_test, run_gen_decode_test, etc.) |

## BCH Error Correction

The `bch.c` library provides BCH(31,21,2) error correction for both FLEX and POCSAG:

- **FLEX**: Uses GF(2^5) field arithmetic with primitive polynomial x^5+x^2+1
- **POCSAG**: Uses polynomial division with generator 0x769

**API:**
```c
void bch_init(void);                              // Call once at startup
uint32_t bch_flex_encode(uint32_t data);          // Encode 21-bit data
int bch_flex_correct(uint32_t *codeword);         // Correct up to 2 bits, returns error count or -1
uint32_t bch_pocsag_encode(uint32_t data);        // Encode 21-bit data  
int bch_pocsag_correct(uint32_t *codeword);       // Correct up to 2 bits, returns error count or -1
```

Both protocols must call `bch_init()` before using encode/correct functions (called automatically in demod init).

## Common Issues

1. **"execlp: No such file or directory"**: Occurs when using `-t wav` without sox installed. Use `-t raw` for raw input files or install sox.

2. **Missing PulseAudio/X11**: Optional dependencies. Build succeeds without them.

3. **DTMF/ZVEI tests fail under Wine**: Known issue with tone detection under Wine emulation. Tests are skipped automatically when `WINE_CMD` is set.

4. **Format warnings in demod_flex.c**: Known issues with `%lld` format specifiers, harmless.

## Pre-installed Environment

The CI environment has these tools via `.github/workflows/copilot-setup-steps.yml`:

| Category | Tools |
|----------|-------|
| Build | `cmake`, `build-essential` (gcc, g++, make) |
| Audio | `sox` (wav/flac to raw conversion) |
| Libraries | `libpulse-dev`, `libx11-dev` |
| Cross-compile | `gcc-mingw-w64-*`, `g++-mingw-w64-*` |
| Windows testing | `wine`, `wine64` |
| Alternative build | `qt5-qmake` |

### Testing with SoX

```bash
# Convert formats
sox -R -t wav input.wav -esigned-integer -b16 -r 22050 -t raw output.raw

# Test wav file directly (sox must be installed)
./build/multimon-ng -t wav -q -a X10 ./test/samples/x10rf.wav
```

### Testing Windows Builds with Wine

```bash
wine ./build-mingw64/multimon-ng.exe -h
wine ./build-mingw64/multimon-ng.exe -t raw -q -a UFSK1200 ./test/samples/ufsk1200.raw
```

## Trust These Instructions

These instructions have been validated by running actual build commands. Only search for additional information if these instructions appear incomplete or produce unexpected errors.
