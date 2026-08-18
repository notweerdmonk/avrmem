---
layout: default
title: Testing Guide
---

# Testing Guide

## 1. Purpose

The testing strategy for `avrmem` is focused on the boundaries between:

- the host/POSIX environment
- AVR-GCC and avr-libc discovery
- linker-map generation
- linker-region parsing
- SFR parsing
- ELF parsing
- AVR address translation
- command-line parsing
- user-facing presentation

The project currently relies on a combination of:

- compiler warnings
- toolchain sanity checks
- command-line smoke tests
- known-good AVR ELF fixtures
- positive and negative cases
- targeted regression tests

---

# 2. Build verification

Always start with a clean build.

```bash
make clean
make
````

The executable should be generated at:

```text
bin/avrmem
```

A debug build can be used when investigating parser or address-resolution
problems:

```bash
make debug
```

or:

```bash
make DEBUG=1
```

---

# 3. Compiler warning baseline

The normal build should complete without warnings:

```text
-std=c11
-Wall
-Wextra
-Wpedantic
-O2
```

Warnings should not be ignored.

A new warning should normally be treated as a regression unless there is a
documented reason for it.

---

# 4. AVR toolchain sanity checks

Before diagnosing `avrmem`, verify that the required AVR toolchain is
available.

```bash
which avr-gcc
avr-gcc --version
```

Verify the device:

```bash
avr-gcc -mmcu=atmega328p --version
```

Verify that avr-libc is selected correctly:

```bash
printf '#include <avr/io.h>\n' |
  avr-gcc -mmcu=atmega328p -dM -E -x c - 2>&1 |
  grep -E \
    '__AVR_DEVICE_NAME__|RAMSTART|RAMEND|FLASHSTART|FLASHEND|E2START|E2END|__SFR_OFFSET'
```

The exact macro set can vary between avr-libc releases. The important point
is that the target-specific AVR definitions are visible.

---

# 5. Device-probe smoke test

The device probe is a central dependency of the application.

A successful probe should establish:

```text
canonical device name
physical FLASH
physical SRAM
physical EEPROM
TEXT linker region
DATA linker region
EEPROM linker region
SFR database
```

The application should provide diagnostics identifying the failing stage
when probing fails.

A successful run should show diagnostics similar to:

```text
probe: probing 'atmega328p'
probe: macro command exit status = 0
probe: macro output = ...
probe: canonical device = atmega328p
probe: physical memory:
       FLASH  ...
       SRAM   ...
       EEPROM ...
probe: SFR database = ... registers
probe: linker command:
  ...
probe: linker exit status = ...
probe: linker map size = ...
probe: linker regions:
       TEXT   ...
       DATA   ...
       EEPROM ...
probe: SUCCESS
```

The exact sizes and register count depend on the installed toolchain.

---

# 6. Linker-map probe test

The linker probe should be tested independently when troubleshooting device
initialization.

A minimal source file is enough:

```c
int main(void)
{
  return 0;
}
```

The probe uses:

```text
avr-gcc -mmcu=<device>
```

with an actual linker map:

```text
-Wl,-Map=<map-file>
```

The generated map must contain the resolved memory configuration.

At minimum, the parser expects:

```text
text
data
eeprom
```

with numeric origins and lengths.

When the linker invocation fails, inspect the compiler/linker diagnostic
before modifying the map parser.

---

# 7. ELF fixture requirements

The most useful regression fixture is an AVR ELF executable containing:

* `.text`
* `.data`
* `.bss`
* `.noinit` when available
* `.symtab`
* function symbols
* SRAM object symbols
* at least one `.data` object
* AVR device metadata

For a strong fixture, include a symbol representing an SFR address when
possible so the complete resolution path can be exercised.

---

# 8. Basic command-line smoke tests

With a known-good ELF:

```bash
bin/avrmem firmware.elf
```

The default invocation should display basic information together with the
default memory/section presentation.

Test each display mode:

```bash
bin/avrmem --memory firmware.elf
bin/avrmem --sections firmware.elf
bin/avrmem --symbols firmware.elf
bin/avrmem --map firmware.elf
```

Test a specific symbol:

```bash
bin/avrmem --symbol led_app firmware.elf
```

Test explicit device selection:

```bash
bin/avrmem --device atmega328p firmware.elf
```

Test combined options:

```bash
bin/avrmem \
  --device atmega328p \
  --sections \
  --symbols \
  firmware.elf
```

---

# 9. Automatic device discovery

Test the normal device-inference path:

```bash
bin/avrmem --symbol led_app firmware.elf
```

The command must:

```text
ELF
 |
 +--> AVR device metadata
 |
 v
device initialization
 |
 v
symbol resolution
```

Verify that the selected device is reported correctly.

---

# 10. Explicit device selection

Test:

```bash
bin/avrmem \
  --device atmega328p \
  --symbol led_app \
  firmware.elf
```

The explicitly supplied device must be used for device initialization.

This test is important because the tool supports both:

```text
automatic device discovery
```

and:

```text
explicit device selection
```

---

# 11. CLI option-order tests

Options must precede the ELF filename.

This should succeed:

```bash
bin/avrmem --symbols firmware.elf
```

This should fail:

```bash
bin/avrmem firmware.elf --symbols
```

This should also fail:

```bash
bin/avrmem firmware.elf --device atmega328p
```

The project intentionally uses `getopt_long()` with a leading `+` in the
option string so that option processing stops at the first non-option
argument.

---

# 12. CLI argument-validation tests

Missing ELF:

```bash
bin/avrmem
```

Expected: failure with a missing-ELF diagnostic.

Missing device value:

```bash
bin/avrmem --device
```

Expected: failure.

Missing symbol value:

```bash
bin/avrmem --symbol
```

Expected: failure.

More than one ELF filename:

```bash
bin/avrmem first.elf second.elf
```

Expected: failure.

Unknown option:

```bash
bin/avrmem --unknown firmware.elf
```

Expected: failure.

Duplicate options that require uniqueness:

```bash
bin/avrmem \
  --device atmega328p \
  --device atmega328p \
  firmware.elf
```

Expected: failure.

Similarly:

```bash
bin/avrmem \
  --symbol led_app \
  --symbol other_symbol \
  firmware.elf
```

Expected: failure.

---

# 13. Short-option tests

The `getopt_long()` interface also supports the project's short aliases.

Verify at least:

```bash
bin/avrmem -h
bin/avrmem -m firmware.elf
bin/avrmem -c firmware.elf
bin/avrmem -s firmware.elf
bin/avrmem -p firmware.elf
bin/avrmem -d atmega328p firmware.elf
bin/avrmem -y led_app firmware.elf
```

---

# 14. Section-resolution tests

Run:

```bash
bin/avrmem --sections firmware.elf
```

Verify that sections report:

```text
name
size
VMA
LMA
memory space
flags
```

Important sections to inspect are:

```text
.text
.data
.bss
.noinit
```

---

# 15. FLASH symbol test

Select a function known to reside in `.text`:

```bash
bin/avrmem --symbol some_function firmware.elf
```

Verify:

```text
Memory: FLASH
```

and that the output contains:

```text
Flash byte address
AVR word address
```

The word address must be calculated from the physical FLASH byte address
using the device abstraction.

For a conventional AVR:

```text
word address = byte address >> 1
```

The CLI should not implement this calculation itself.

---

# 16. SRAM symbol test

Select a global or static SRAM-resident object:

```bash
bin/avrmem --symbol counter firmware.elf
```

Verify:

```text
Memory: SRAM
```

and:

```text
Physical address
```

The output should distinguish the ELF VMA from the physical SRAM address.

---

# 17. `.data` test

Select a symbol known to reside in `.data`:

```bash
bin/avrmem --symbol initialized_value firmware.elf
```

Verify:

```text
VMA -> SRAM
LMA -> FLASH
```

The symbol LMA must satisfy:

```text
symbol LMA =
  section LMA +
  (symbol VMA - section VMA)
```

This verifies both:

* device DATA-space translation
* ELF PT_LOAD LMA calculation

---

# 18. `.bss` test

Select a symbol in `.bss`.

Verify that:

```text
Memory = SRAM
```

and:

```text
LMA = unavailable
```

This is expected because `.bss` is normally `SHT_NOBITS`.

The presentation should identify its initialization behavior as zeroing
rather than a FLASH load.

---

# 19. `.noinit` test

When the ELF contains `.noinit`, select a symbol from it and verify:

```text
Memory = SRAM
LMA = unavailable
Initialization = preserved
```

The tool must not invent a FLASH LMA for `.noinit`.

---

# 20. SFR tests

The SFR parser consumes concrete avr-gcc preprocessor definitions.

Typical examples:

```c
#define PORTB  _SFR_IO8(0x05)
#define UCSR0A _SFR_MEM8(0xC0)
```

The parser must normalize both forms.

For `_SFR_IO8()`:

```text
physical address =
  encoded I/O address + __SFR_OFFSET
```

For `_SFR_MEM8()`:

```text
physical address =
  encoded memory address
```

---

# 21. SFR database tests

After device initialization, verify that the database is non-empty for a
normal AVR device.

The command-line diagnostics should expose the number of discovered SFRs.

For an ATmega328P toolchain, a result on the order of dozens of registers is
expected. The exact count should not be treated as a universal constant
because avr-libc versions can expose different aliases and definitions.

---

# 22. SFR name lookup

Test known registers such as:

```text
PORTB
SREG
SP
UCSR0A
```

through the SFR API.

Verify that:

* name lookup succeeds
* address lookup succeeds
* both identify the same normalized physical DATA-space address

---

# 23. SFR address lookup

For a known register, obtain its canonical DATA-space address and query the
database by address.

The following relationship should hold:

```text
avr_sfr_find(
  database,
  register->name
)
==
avr_sfr_find_address(
  database,
  register->address
)
```

where both results refer to the same SFR definition.

---

# 24. SFR-associated symbol test

When an ELF symbol resolves to a known SFR address, the resolved symbol
should contain:

```text
memory_space
sfr
physical address
```

and the CLI should display the SFR name.

Conceptually:

```text
ELF symbol
    |
    v
AvrDevice address resolution
    |
    v
physical DATA address
    |
    v
SFR lookup
    |
    v
PORTB / UCSR0A / ...
```

---

# 25. ELF parser negative tests

The ELF parser should reject:

* non-ELF input
* ELF64 input
* big-endian input
* non-AVR ELF
* truncated ELF files
* malformed section tables
* malformed program-header tables
* invalid string-table references
* malformed symbol tables

Example:

```bash
printf 'not an ELF file\n' > invalid.bin
bin/avrmem --sections invalid.bin
```

Expected: failure.

---

# 26. Stripped ELF test

Use an ELF without `.symtab`.

Verify that:

```bash
bin/avrmem --sections stripped.elf
```

still works.

The symbol-oriented commands should report that no usable symbol table is
available rather than crashing.

---

# 27. Missing ELF device metadata

Test an ELF that does not expose usable AVR device metadata.

Without `--device`:

```bash
bin/avrmem --symbol led_app firmware_without_device_metadata.elf
```

Expected: a clear failure explaining that the device cannot be inferred and
that `--device` should be supplied.

With explicit device:

```bash
bin/avrmem \
  --device atmega328p \
  --symbol led_app \
  firmware_without_device_metadata.elf
```

Expected: successful device initialization, assuming the ELF itself is
otherwise valid.

---

# 28. Invalid device test

Test:

```bash
bin/avrmem \
  --device this_is_not_an_avr \
  firmware.elf
```

Expected: device initialization failure.

The diagnostic should identify the selected device.

---

# 29. Linker-probe failure tests

Temporarily make `avr-gcc` unavailable from `PATH`:

```bash
PATH=/nonexistent bin/avrmem firmware.elf
```

The application should fail cleanly during device initialization.

A broken linker invocation should identify the linker-probe stage rather than
being reported only as an unexplained symbol-resolution failure.

---

# 30. Runtime ownership tests

The following lifecycle should be safe:

```c
AvrDevice device;

memset(&device, 0, sizeof(device));

if (avr_device_init(
      &device,
      "atmega328p"
    ))
{
  /* use device */

  avr_device_destroy(&device);
}
```

Likewise:

```c
AvrElf *elf = NULL;

if (avr_elf_open(
      filename,
      &elf
    ))
{
  /* use elf */

  avr_elf_close(elf);
}
```

Destroy functions should tolerate `NULL`.

---

# 31. Address-resolution regression tests

For every supported device fixture, verify at least:

```text
FLASH address
SRAM address
EEPROM address
register address
I/O address
extended-I/O address when applicable
unknown address
```

An unknown address must not silently become another memory type.

---

# 32. LMA regression tests

For `.data`, verify:

```text
section VMA
section LMA
symbol VMA
section offset
symbol LMA
```

The following must hold:

```text
symbol LMA =
  section LMA +
  symbol section-relative offset
```

For `.bss` and other `SHT_NOBITS` sections:

```text
has_lma == false
```

---

# 33. Memory-map regression tests

Run:

```bash
bin/avrmem --map firmware.elf
```

Verify:

```text
FLASH region
SRAM region
EEPROM region
flash-resident sections
DATA/SRAM sections
.data initialization mapping
.bss initialization mapping
.noinit preservation
EEPROM sections
```

The displayed physical ranges must be derived from the selected device, not
hardcoded MCU constants.

---

# 34. Automatic versus explicit device regression

For an ELF whose embedded device matches the supplied device, compare:

```bash
bin/avrmem --symbol led_app firmware.elf
```

with:

```bash
bin/avrmem \
  --device atmega328p \
  --symbol led_app \
  firmware.elf
```

The resolved addresses should agree.

This is an important test because it exercises two different device-selection
paths while using the same device-resolution implementation.

---

# 35. Test-driven regression rule

Whenever a bug is discovered:

1. Reproduce it with the smallest possible ELF or toolchain command.
2. Identify the first failing abstraction layer.
3. Fix that layer instead of adding compensating logic elsewhere.
4. Preserve the working discovery mechanism.
5. Add a regression test that reproduces the original failure.

For example, a device-probe failure should not be hidden by changing
`avrmem.c`.

A linker-map parsing problem should not be fixed by hardcoding the device's
memory lengths.

An SFR parsing problem should not prevent physical-memory discovery.

---

# 36. Manual end-to-end smoke test

For a known-good fixture, run:

```bash
make clean
make

bin/avrmem firmware.elf
bin/avrmem --memory firmware.elf
bin/avrmem --sections firmware.elf
bin/avrmem --symbols firmware.elf
bin/avrmem --symbol led_app firmware.elf
bin/avrmem --map firmware.elf
bin/avrmem --device atmega328p firmware.elf
bin/avrmem --device atmega328p --symbol led_app firmware.elf
```

Then test invalid usage:

```bash
bin/avrmem
bin/avrmem firmware.elf --symbols
bin/avrmem --device
bin/avrmem --symbol
bin/avrmem --unknown firmware.elf
```

The valid commands should complete successfully.

The invalid commands should fail with a useful diagnostic and non-zero exit
status.

---

# 37. Documentation regression

When CLI behavior changes, update:

```text
README.md
docs/index.md
docs/guide.md
docs/testing.md
docs/api.md
```

when the change affects their respective content.

When architecture changes, update:

```text
docs/architecture.md
```

at the same time.

---

# 38. Final release checklist

Before considering a change complete:

```text
[ ] make clean
[ ] make
[ ] no compiler warnings
[ ] debug build succeeds
[ ] automatic device discovery works
[ ] explicit --device works
[ ] --memory works
[ ] --sections works
[ ] --symbols works
[ ] --symbol works
[ ] --map works
[ ] FLASH symbol resolves correctly
[ ] SRAM symbol resolves correctly
[ ] .data VMA/LMA resolve correctly
[ ] .bss reports no LMA
[ ] .noinit reports preserved initialization
[ ] SFR discovery works
[ ] SFR lookup by name works
[ ] SFR lookup by address works
[ ] invalid ELF is rejected
[ ] invalid device is rejected
[ ] invalid command-line arguments are rejected
[ ] documentation is updated
```
