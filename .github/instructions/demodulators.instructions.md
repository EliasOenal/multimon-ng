---
applyTo: "demod_*.c"
---

# Demodulator Development Guidelines

## File Structure

Each demodulator file (`demod_<name>.c`) must:
1. Define a `const struct demod_param demod_<name>` structure
2. Implement the required callback functions referenced in the struct

## Required Steps When Adding a New Demodulator

1. Create `demod_<name>.c` with the demodulator implementation
2. Add `extern const struct demod_param demod_<name>;` to `multimon.h`
3. Add `&demod_<name>` to the `ALL_DEMOD` macro in `multimon.h`
4. Add source file to `CMakeLists.txt` (SOURCES list)
5. Add source file to `multimon-ng.pro`

## Code Style

- Use snake_case for functions and variables
- Follow GNU11 C standard
- Use platform conditionals: `#ifdef WIN32`, `#ifdef _MSC_VER`
- Include necessary headers from `multimon.h`

## BCH Error Correction

For demodulators using BCH error correction (FLEX, POCSAG):
- Include `bch.h`
- Call `bch_init()` in the demodulator initialization
- Use `bch_flex_correct()` or `bch_pocsag_correct()` as appropriate
