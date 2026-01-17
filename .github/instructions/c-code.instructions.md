---
applyTo: "**/*.c,**/*.h"
---

# C Code Guidelines for multimon-ng

## Build System

- Primary: CMake (`mkdir build && cd build && cmake .. && make -j$(nproc)`)
- Secondary: qmake

## Code Style

- **C Standard**: GNU11 (`-std=gnu11`)
- **Compiler Flags**: `-Wall -Wextra`
- **Naming**: snake_case for functions and variables
- **File Organization**: All source files in root directory (flat structure)
- **Platform Conditionals**: `#ifdef WIN32`, `#ifdef _MSC_VER`

## Key Headers

- `multimon.h` - Demodulator struct definitions, ALL_DEMOD macro
- `gen.h` - Generator struct definitions
- `bch.h` - BCH error correction API

## Testing Changes

Always run tests after making changes:
```bash
./test/run_tests.sh
```

## Common Warnings

The following warnings are known and harmless:
- Format warnings in `demod_flex.c` (`%lld` format specifiers)
- Unused variable warnings in some demodulators
