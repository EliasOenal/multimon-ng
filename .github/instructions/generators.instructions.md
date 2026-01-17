---
applyTo: "gen_*.c,gen.c,gen.h"
---

# Signal Generator Development Guidelines

## File Structure

Generator files (`gen_<name>.c`) implement signal generation for testing protocols.

## Required Steps When Adding a New Generator

1. Create `gen_<name>.c` with `gen_init_<name>()` and `gen_<name>()` functions
2. Add function declarations to `gen.h`
3. Add command-line option handling to `gen.c`
4. Add source file to `CMakeLists.txt` (GEN_SOURCES list)

## Code Style

- Use snake_case for functions and variables
- Follow GNU11 C standard
- Use platform conditionals: `#ifdef WIN32`, `#ifdef _MSC_VER`

## Key Options Reference

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

## BCH Error Injection

For testing BCH error correction, use the `-e` option to inject 0-3 bit errors per codeword.
