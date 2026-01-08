# Copilot Agent Instructions for multimon-ng

## Repository Overview

**multimon-ng** is a C-based command-line tool that decodes various digital radio transmission modes. It is the successor to the original multimon project. Supported protocols include POCSAG (512/1200/2400), FLEX, EAS, AFSK (1200/2400), FSK9600, DTMF, ZVEI, Morse/CW, X10, and more. CMake is the primary build system (qmake is also supported but secondary).

- **Language**: C (GNU11 standard)
- **License**: GPL-2.0
- **Build Systems**: CMake (recommended), qmake
- **Target Platforms**: Linux, macOS, Windows (MinGW/MSVC)

## Project Structure

```
/                       # Root directory with all source files (flat structure)
├── CMakeLists.txt      # Primary build configuration
├── multimon-ng.pro     # qmake build configuration (secondary)
├── multimon.h          # Main header with demodulator definitions
├── unixinput.c         # Main entry point (contains main())
├── demod_*.c           # Demodulator implementations
├── cJSON.[ch]          # JSON output support
├── BCHCode.c           # BCH error correction (optional, BCHCode_stub.c used if absent)
├── cmake/              # CMake toolchain files for cross-compilation
├── example/            # Example input files for testing
├── unsupported/        # Legacy Makefile and Visual Studio project files
└── .github/workflows/  # CI workflows
```

## Build Instructions

### CMake Build (Recommended)

Always use an out-of-source build in a `build` directory:

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The binary is created at `build/multimon-ng`. Build time is typically under 30 seconds.

**Important Notes:**
- The build produces warnings (format specifiers, VLA at file scope) that are normal and do not affect functionality
- X11 and PulseAudio are optional dependencies; the build succeeds without them
- If BCHCode.c is missing, BCHCode_stub.c is used automatically

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

## Testing Changes

Validate changes using the automated test suite:

```bash
# Run all tests (requires sox for non-raw formats)
./test/run_tests.sh

# Or test manually with example files
./build/multimon-ng -t raw -q -a UFSK1200 ./example/ufsk1200.raw
./build/multimon-ng -h
```

**Expected behavior:** The binary should display decoded data without crashes. Use `-q` for quiet mode.

## CI/CD Workflows

Four GitHub Actions workflows run on every push:

| Workflow Name | Workflow File | Description |
|----------|------|-------------|
| C/C++ CI cmake | cmake.yml | Linux CMake build (ubuntu-latest) |
| C/C++ CI qmake | qmake.yml | Linux qmake build (requires Qt) |
| MinGW32 Cross-Compile | mingw32.yml | Windows 32-bit cross-compile |
| MinGW64 Cross-Compile | mingw64.yml | Windows 64-bit cross-compile |

**All workflows must pass.** The cmake workflow is the primary validation - ensure your changes build with CMake first.

## Code Conventions

- **C Standard**: GNU11 (`-std=gnu11`)
- **Compiler Flags**: `-Wall -Wextra` (warnings are enabled)
- **Naming**: snake_case for functions and variables
- **File Organization**: All source files in root directory (flat structure)
- **Demodulator Pattern**: Each demodulator has a `demod_<name>.c` file with a `demod_<name>` struct
- **Preprocessor Conditionals**: Platform-specific code uses `#ifdef WIN32`, `#ifdef _MSC_VER`, etc.

## Adding New Demodulators

1. Create `demod_<name>.c` with the demodulator implementation
2. Add `extern const struct demod_param demod_<name>;` to `multimon.h`
3. Add `&demod_<name>` to the `ALL_DEMOD` macro in `multimon.h`
4. Add the source file to `CMakeLists.txt` (SOURCES list) and `multimon-ng.pro`

## Key Files Reference

- **unixinput.c**: Program entry point, command-line parsing, main loop
- **multimon.h**: Core data structures, demodulator declarations, ALL_DEMOD macro
- **CMakeLists.txt**: Build configuration, source file list, platform detection
- **pocsag.c**: POCSAG decoder implementation (commonly modified)
- **cJSON.c/h**: JSON output formatting (third-party library)

## Common Issues

1. **"execlp: No such file or directory"**: Occurs when using `-t wav` without sox installed. Use `-t raw` for raw input files.
2. **Missing PulseAudio/X11**: These are optional; the build uses DUMMY_AUDIO if not found.
3. **Format warnings in demod_flex.c**: These are known issues with `%lld` format specifiers and are harmless.

## Pre-installed Environment
The Copilot coding agent environment has the following tools pre-installed via `.github/workflows/copilot-setup-steps.yml`:
| Category | Tools |
|----------|-------|
| **Build Tools** | `cmake`, `build-essential` (gcc, g++, make) |
| **Audio Processing** | `sox` (for wav/flac to raw conversion) |
| **Development Libraries** | `libpulse-dev` (PulseAudio headers), `libx11-dev` (X11 headers) |
| **Cross-Compilation** | `gcc-mingw-w64-i686`, `g++-mingw-w64-i686` (32-bit Windows), `gcc-mingw-w64-x86-64`, `g++-mingw-w64-x86-64` (64-bit Windows) |
| **Windows Testing** | `wine`, `wine64` (for running Windows executables on Linux) |
| **Alternative Build** | `qt5-qmake` (for qmake builds) |
### Testing with SoX
With SoX installed, you can test with various audio formats:
```bash
# Test with wav file (requires sox)
./build/multimon-ng -t wav -q -a X10 ./example/x10rf.wav
# Test with flac file (requires sox)
./build/multimon-ng -t flac -q -a POCSAG1200 ./example/POCSAG_sample_-_1200_bps.flac
# Convert wav to raw for direct input
sox -t wav input.wav -esigned-integer -b16 -r 22050 -t raw output.raw
```
### Testing Windows Builds with Wine
After cross-compiling, you can test Windows executables using Wine:
```bash
# Build 64-bit Windows executable
mkdir build-mingw64 && cd build-mingw64
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw64.cmake
make
# Test with Wine
wine ./multimon-ng.exe -h
wine ./multimon-ng.exe -t raw -q -a UFSK1200 ../example/ufsk1200.raw
```

## Trust These Instructions

These instructions have been validated by running the actual build commands. Only search for additional information if these instructions appear incomplete or produce unexpected errors.
