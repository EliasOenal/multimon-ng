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

There is no automated test suite. Validate changes manually:

```bash
# Test with example raw file (UFSK1200 protocol)
./build/multimon-ng -t raw -q -a UFSK1200 ./example/ufsk1200.raw

# Show help and available demodulators
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

## Trust These Instructions

These instructions have been validated by running the actual build commands. Only search for additional information if these instructions appear incomplete or produce unexpected errors.
