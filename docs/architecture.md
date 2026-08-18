---
layout: default
title: Architecture
---

# avrmem Architecture

## Status

**Accepted**

This document records the architectural decisions governing the `avrmem`
project.

---

## 1. Problem statement

`avrmem` needs to interpret AVR ELF executables while remaining independent
of any single MCU.

The original implementation contained MCU-specific constants directly in
the executable:

```text
FLASH size
SRAM range
EEPROM size
ELF DATA offset
````

That approach does not scale.

It also conflates several distinct concepts:

```text
ELF/linker address
physical AVR address
CPU instruction/word address
load-memory address
```

The current architecture separates those concerns.

---

# 2. Architectural overview

The application is divided into the following layers:

```text
                           +------------------+
                           |    avrmem.c      |
                           | CLI / presentation|
                           +--------+---------+
                                    |
                    +---------------+---------------+
                    |                               |
                    v                               v
             +-------------+                 +---------------+
             |  avr_elf.c  |                 | avr_device.c |
             | ELF semantics|                | device model |
             +------+------+                 +-------+-------+
                    |                                |
                    |                                v
                    |                       +-------------------+
                    |                       | avr_device_probe  |
                    |                       +---------+---------+
                    |                                 |
                    |                    +------------+------------+
                    |                    |                         |
                    |                    v                         v
                    |             avr-gcc -dM -E            avr-gcc linker
                    |             <avr/io.h>                generated map
                    |                    |                         |
                    |                    v                         v
                    |               avr_sfr.c              linker regions
                    |
                    v
                 ELF image
```

The responsibilities are intentionally separated.

---

# 3. `avrmem.c`

`avrmem.c` is the application frontend.

It owns:

* command-line parsing
* application control flow
* device selection policy
* presentation
* error reporting

It does **not** own:

* ELF parsing
* linker-address calculations
* SFR parsing
* MCU-specific memory constants

The frontend consumes the public APIs exposed by the lower layers.

---

# 4. `avr_elf.c`

`avr_elf.c` owns ELF semantics.

It is responsible for:

* validating ELF32 files
* validating little-endian representation
* validating `EM_AVR`
* parsing ELF section headers
* parsing ELF program headers
* parsing `.symtab`
* resolving section and symbol names
* resolving symbols
* deriving LMAs from `PT_LOAD`
* extracting ELF AVR device metadata

It deliberately does not contain:

```text
ATmega328P constants
SRAM offsets
FLASH sizes
EEPROM sizes
SFR tables
```

Those belong to the device layer.

---

# 5. `avr_device.c`

`avr_device.c` owns the runtime representation of one selected AVR device.

The device model contains two different classes of information.

## Physical memory

```text
FLASH
SRAM
EEPROM
```

These are actual target-memory regions.

## ELF/linker address spaces

```text
TEXT
DATA
EEPROM
```

These are the address ranges used by the AVR linker and therefore appear
in ELF section and symbol addresses.

The two categories are deliberately represented separately.

---

# 6. Physical SRAM versus DATA linker space

SRAM is a physical memory.

The AVR linker may represent the same physical SRAM using a different
address-space origin.

Therefore:

```text
physical SRAM address
        !=
ELF DATA-space address
```

The conversion is owned by `AvrDevice`.

Conceptually:

```text
ELF DATA address
       |
       v
DATA linker region
       |
       v
physical AVR DATA address
       |
       +----> register file
       +----> I/O
       +----> extended I/O
       +----> SRAM
```

`.data`, `.bss`, stack, heap, and `.noinit` are all runtime allocations
within physical SRAM.

They are not separate physical memories.

Their differences are ELF/linker section semantics.

---

# 7. Device discovery

Device-specific information is not maintained in a source-level MCU table.

The selected device is passed to:

```text
avr-gcc -mmcu=<device>
```

The toolchain is then used to discover the device description.

Two complementary mechanisms are used.

## 7.1 Preprocessor discovery

The probe asks AVR-GCC to preprocess `<avr/io.h>`:

```text
avr-gcc -mmcu=<device> -dM -E ...
```

This provides the concrete macro environment selected for the device.

The probe extracts:

* canonical device name
* physical FLASH range
* physical SRAM range
* physical EEPROM range
* SFR definitions

This means the project does not maintain its own database of those values.

---

## 7.2 Linker-memory discovery

The probe performs an actual AVR link and generates a linker map:

```text
avr-gcc -mmcu=<device> ... -Wl,-Map=<map>
```

The generated map provides resolved linker memory-region information.

The probe extracts:

```text
TEXT
DATA
EEPROM
```

origins and lengths from the map.

### Why the linker map is used

The default AVR linker script can contain symbolic expressions and linker
symbols instead of concrete numeric values.

The generated map represents the values after linker resolution.

Therefore the map is the authoritative source for the linker's selected
memory regions.

---

# 8. `avr_sfr.c`

`avr_sfr.c` owns SFR parsing.

It consumes the concrete preprocessor output generated by the selected
avr-libc environment.

Typical definitions include:

```c
#define PORTB  _SFR_IO8(0x05)
#define UCSR0A _SFR_MEM8(0xC0)
```

The SFR parser normalizes both forms into physical AVR DATA-space addresses.

For `_SFR_IO8()`:

```text
encoded I/O address
        +
__SFR_OFFSET
        |
        v
physical DATA-space address
```

For `_SFR_MEM8()`:

```text
encoded memory address
        |
        v
physical DATA-space address
```

This allows callers to perform a uniform address lookup regardless of
whether avr-libc defined the register using `_SFR_IO8()` or `_SFR_MEM8()`.

---

# 9. SFRs are an auxiliary capability

SFR discovery must not invalidate an otherwise valid device description.

The device can still be useful for:

```text
FLASH
SRAM
EEPROM
TEXT/DATA/EEPROM linker regions
ELF symbol resolution
```

even when a particular avr-libc release changes its SFR macro representation.

Therefore SFR discovery is intentionally treated as an auxiliary capability.

---

# 10. Address-resolution model

Address interpretation follows this model:

```text
                    ELF symbol value
                           |
                           v
                +----------------------+
                |    AvrDevice         |
                | classify/translate   |
                +----------+-----------+
                           |
             +-------------+-------------+
             |             |             |
             v             v             v
           FLASH         DATA         EEPROM
                           |
             +-------------+-------------+
             |             |             |
             v             v             v
          REGISTER       I/O           SRAM
```

The key property is that `avr_elf.c` does not perform the MCU-specific
translation itself.

It asks `AvrDevice` to interpret the address.

---

# 11. Resolved symbol model

`AvrResolvedSymbol` combines information from both layers.

A normal symbol can therefore have:

```text
ELF VMA
    |
    +--> memory space
    +--> AVR/physical address
    +--> physical address
    +--> section
    +--> LMA
    +--> FLASH CPU word address
    +--> SFR
```

The distinction between runtime and load address is critical.

For `.data`:

```text
symbol VMA
    |
    +--> SRAM runtime location

symbol LMA
    |
    +--> FLASH initialization image
```

---

# 12. LMA resolution

ELF section headers do not directly contain a load-memory address.

The implementation derives LMA from file-backed `PT_LOAD` program headers.

For a section-relative offset:

```text
LMA =
    PT_LOAD.p_paddr
    + (section.sh_offset - PT_LOAD.p_offset)
    + section_offset
```

This works for `.data` without requiring a special-case lookup of
`__data_load_start`.

For `SHT_NOBITS` sections such as `.bss`, there is no file-backed
initialization image and therefore no LMA.

---

# 13. Device selection policy

The command-line frontend supports:

```text
--device <name>
```

and automatic device selection.

## Explicit device

When `--device` is supplied:

```text
--device <name>
        |
        v
avr_device_init()
```

The explicit device controls device-specific address interpretation.

## Automatic device

When `--device` is omitted:

```text
ELF
 |
 +--> avr_elf_get_device_name()
 |
 v
avr_device_init()
```

The device name comes from AVR ELF metadata.

This keeps the normal invocation simple:

```bash
avrmem firmware.elf
```

while still allowing:

```bash
avrmem --device atmega328p firmware.elf
```

for explicit control.

---

# 14. Command-line parsing

`avrmem` uses `getopt_long()`.

The canonical command-line structure is:

```text
avrmem [options] <elf>
```

The option-string begins with `+` so GNU `getopt_long()` stops option
processing at the first non-option argument.

Therefore:

```bash
avrmem --symbols firmware.elf
```

is valid, while:

```bash
avrmem firmware.elf --symbols
```

is rejected.

This keeps the CLI deterministic and matches the project's requirement that
all options precede the ELF filename.

---

# 15. Ownership and lifetime

Ownership is explicit throughout the design.

## `AvrDeviceProbeResult`

Owns:

```text
name
sfr_database
```

until ownership is transferred.

## `AvrDevice`

After successful initialization, owns:

```text
name
sfr_database
```

These are released by:

```c
avr_device_destroy()
```

## `AvrElf`

Owns:

```text
ELF image
section arrays
symbol arrays
program headers
string tables
device-name metadata
```

Pointers returned from the ELF API are borrowed pointers.

They remain valid until:

```c
avr_elf_close()
```

---

# 16. Error handling

Device probing is intentionally staged:

```text
1. AVR-GCC macro probe
2. device-name extraction
3. physical-memory extraction
4. SFR extraction
5. linker-map generation
6. linker-region extraction
7. AvrDevice construction
```

Each stage should identify its own failure.

This is important because the toolchain is an external dependency and can
fail due to:

* missing `avr-gcc`
* unsupported `-mmcu`
* missing avr-libc headers
* host temporary-file problems
* linker invocation failures
* unexpected linker-map formats
* changes in avr-libc macro definitions

The diagnostics should identify the actual stage rather than collapsing all
failures into:

```text
unable to initialize AVR device
```

---

# 17. POSIX host assumption

The current implementation deliberately assumes a POSIX host.

It uses interfaces such as:

```text
popen()
pclose()
mkstemp()
fdopen()
unlink()
getopt_long()
```

Feature-test macros are provided where required so the intended POSIX APIs
are visible under C11 compilation.

The design should allow future host abstraction without changing the public
ELF or device APIs.

Potential future platforms include:

```text
Darwin
Windows
```

but they are outside the current scope.

---

# 18. Rejected alternatives

## Hardcoded MCU tables

Rejected.

A table of:

```text
atmega328p
atmega2560
attiny...
```

would duplicate information already present in the AVR toolchain and would
require source changes whenever support for another device is required.

---

## MCU-specific initialization functions

Rejected.

Functions such as:

```c
init_atmega328p();
init_atmega2560();
```

would violate the device abstraction.

The public initialization path must remain:

```c
avr_device_init(
  &device,
  device_name
);
```

---

## Parsing raw linker scripts as the primary source

Rejected.

The linker script may contain unresolved expressions, symbols, and
toolchain-specific constructs.

The generated linker map reflects the resolved configuration selected by
the actual linker invocation.

---

## Hardcoded SFR tables

Rejected.

The SFR definitions already exist in avr-libc.

Duplicating them would create another source of truth.

---

## Putting all address calculations in `avr_elf.c`

Rejected.

ELF semantics and MCU address semantics are different responsibilities.

`avr_elf.c` determines:

```text
what the ELF says
```

`avr_device.c` determines:

```text
what that address means for this AVR device
```

---

# 19. Architectural invariants

The following rules should remain true as the project evolves.

### Invariant 1

No MCU-specific memory constants belong in `avrmem.c`.

### Invariant 2

No MCU-specific memory constants belong in `avr_elf.c`.

### Invariant 3

Physical memory and ELF/linker address spaces remain distinct.

### Invariant 4

SRAM is one physical memory regardless of whether storage is `.data`,
`.bss`, `.noinit`, stack, or heap.

### Invariant 5

SFRs are resolved from the selected toolchain and normalized to physical
DATA-space addresses.

### Invariant 6

LMA is derived from ELF load mappings rather than guessed from VMA.

### Invariant 7

`AvrDevice` owns device-specific interpretation.

### Invariant 8

`AvrElf` owns ELF-specific interpretation.

### Invariant 9

The CLI only orchestrates and presents.

### Invariant 10

Adding support for another AVR device should not require a per-device
initialization function or hardcoded memory table.

---

# 20. Future extensions

The current architecture leaves room for:

* ELF/device discrepancy warnings
* richer SFR metadata
* register bit-field parsing
* additional AVR memory spaces
* non-POSIX host support
* structured diagnostics
* unit tests for individual parser components
* additional ELF note handling
* machine-readable output formats such as JSON

Such extensions should preserve the separation between:

```text
ELF semantics
device semantics
toolchain discovery
CLI presentation
```
