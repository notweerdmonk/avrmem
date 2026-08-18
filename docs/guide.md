---
layout: default
title: Developer Guide
---

# Developer Guide

## 1. Purpose

`avrmem` is designed to inspect AVR ELF executables without maintaining an
MCU-specific memory database in the source tree.

The implementation separates:

- ELF parsing
- AVR device discovery
- AVR address translation
- SFR discovery
- command-line parsing and presentation

The central design principle is:

```text
ELF semantics
     !=
device semantics
     !=
presentation
````

---

# 2. Source tree

The project is organized as:

```text
.
├── Makefile
├── README.md
├── src/
│   ├── avr_device.c
│   ├── avr_device_probe.c
│   ├── avr_elf.c
│   ├── avr_sfr.c
│   └── avrmem.c
├── include/
│   ├── avr_device.h
│   ├── avr_device_probe.h
│   ├── avr_elf.h
│   └── avr_sfr.h
├── bin/
└── docs/
    ├── _config.yml
    ├── index.md
    ├── README.md
    ├── guide.md
    ├── api.md
    ├── testing.md
    └── architecture.md
```

The source code belongs in `src/`.

Public and internal headers belong in `include/`.

Generated executables belong in `bin/`.

The `docs/` directory is also the source tree for the Jekyll documentation
site.

---

# 3. Build system

The project is built with the top-level `Makefile`.

Normal build:

```bash
make
```

The executable is:

```text
bin/avrmem
```

## Debug builds

A debug configuration can be enabled with:

```bash
make debug
```

or:

```bash
make DEBUG=1
make DEBUG=yes
make DEBUG=true
make DEBUG=on
```

The debug configuration uses:

```text
-O0
-g3
-DDEBUG
```

The optimized configuration uses:

```text
-O2
```

with the project's warning options.

## Clean build

```bash
make clean
make
```

For a full rebuild:

```bash
make rebuild
```

---

# 4. Host/toolchain requirements

The current implementation assumes a POSIX host.

The build itself is a host C application, while the AVR target information
is discovered through the installed AVR-GCC toolchain.

Required tools include:

```text
gcc or compatible C11 compiler
avr-gcc
avr-libc
AVR binutils selected by avr-gcc
make
```

Verify the AVR compiler:

```bash
which avr-gcc
avr-gcc --version
```

Verify a device is accepted:

```bash
avr-gcc -mmcu=atmega328p --version
```

---

# 5. Runtime device selection

The device can be selected explicitly:

```bash
bin/avrmem --device atmega328p firmware.elf
```

or inferred from the ELF:

```bash
bin/avrmem firmware.elf
```

When `--device` is omitted, `avrmem` obtains the device name from the
ELF AVR device metadata.

The selected device is then passed into:

```c
avr_device_init()
```

The device layer is therefore initialized before symbol/section address
resolution.

---

# 6. Overall runtime flow

A normal invocation follows this sequence:

```text
argv
 |
 v
getopt_long()
 |
 v
CommandLine
 |
 v
avr_elf_open()
 |
 +--> ELF validation
 +--> sections
 +--> symbols
 +--> program headers
 +--> AVR device metadata
 |
 v
device selection
 |
 v
avr_device_init()
 |
 +--> avr-gcc macro probe
 |      |
 |      +--> device name
 |      +--> physical memory
 |      +--> SFR definitions
 |
 +--> avr-gcc linker-map probe
        |
        +--> TEXT
        +--> DATA
        +--> EEPROM
 |
 v
AvrDevice
 |
 v
section/symbol resolution
 |
 v
presentation
```

---

# 7. `avr_device_probe.c`

The probe layer is responsible for discovering device information from the
installed toolchain.

It intentionally does not contain a database such as:

```c
if (strcmp(device_name, "atmega328p") == 0)
  ...
```

Instead, the toolchain itself is the source of device-specific information.

---

# 8. AVR-LibC macro probing

The first discovery path uses AVR-GCC preprocessing.

Conceptually:

```text
avr-gcc -mmcu=<device> -dM -E ...
```

with:

```c
#include <avr/io.h>
```

The resulting macro environment contains target-specific information.

The probe extracts:

```text
device name
FLASH range
SRAM range
EEPROM range
SFR definitions
```

The canonical device name is taken from:

```text
__AVR_DEVICE_NAME__
```

The physical memory information is obtained from avr-libc macros such as:

```text
RAMSTART
RAMEND
FLASHSTART
FLASHEND
E2START
E2END
```

Not every avr-libc revision defines every optional macro, so the probe
contains fallbacks where appropriate.

---

# 9. SFR discovery

SFR discovery uses the same preprocessor output.

Typical definitions include:

```c
#define PORTB  _SFR_IO8(0x05)
#define UCSR0A _SFR_MEM8(0xC0)
```

The SFR parser converts both forms into canonical physical DATA-space
addresses.

For:

```c
_SFR_IO8(x)
```

the physical address is:

```text
x + __SFR_OFFSET
```

For:

```c
_SFR_MEM8(x)
```

the physical address is:

```text
x
```

The resulting `AvrSfrDatabase` provides both symbolic-name and
address-based lookup.

---

# 10. SFR probing is non-fatal

SFR parsing is intentionally an auxiliary capability.

The device must still be usable when:

```text
physical memory discovery succeeds
linker memory discovery succeeds
```

but:

```text
SFR parsing fails
```

This is important because avr-libc macro representations can vary between
toolchain versions.

A future avr-libc change should not prevent basic ELF/device analysis from
working simply because a new SFR representation is not yet recognized.

---

# 11. Linker-memory probing

The second discovery path is the AVR linker itself.

The probe performs a real device-specific link using:

```text
-mmcu=<device>
```

and requests a linker map:

```text
-Wl,-Map=<temporary-map>
```

The generated map is then parsed.

The important information is the linker's resolved memory configuration:

```text
TEXT
DATA
EEPROM
```

with their:

```text
origin
length
```

---

# 12. Why the linker map is used

The default linker script may contain expressions and linker symbols instead
of concrete numeric values.

For example, a script may ultimately resolve a region from another symbol
or expression.

The generated map records the values after the linker has evaluated those
expressions.

Therefore the project treats the generated linker map as the authoritative
source for the selected linker address-space layout.

Do not replace this with a hardcoded MCU lookup table.

---

# 13. `AvrDevice`

`avr_device.c` converts the probe result into the public `AvrDevice`
representation.

The device contains:

```text
physical FLASH
physical SRAM
physical EEPROM

ELF TEXT linker space
ELF DATA linker space
ELF EEPROM linker space

physical register-file region
physical I/O region
physical extended-I/O region

SFR database

FLASH byte -> CPU word address shift
```

---

# 14. Physical memory versus linker memory

This distinction is fundamental.

For example:

```text
physical SRAM
    |
    +--> physical AVR DATA-space address

ELF DATA linker space
    |
    +--> ELF/VMA address
```

The numerical values may differ.

Therefore this is wrong:

```c
physical = symbol_value;
```

when `symbol_value` is an AVR-GCC DATA-space VMA.

Instead, the address must be interpreted by `AvrDevice`.

---

# 15. SRAM model

SRAM is one physical memory region.

The following runtime objects can all reside in the same physical SRAM:

```text
.data
.bss
.noinit
heap
stack
```

They differ in ELF section semantics, not physical memory type.

This is why `AvrDevice` contains one:

```c
AvrMemoryRegion sram;
```

rather than separate physical regions for `.data` and `.bss`.

---

# 16. DATA-space classification

The physical AVR DATA space is conceptually divided into:

```text
register file
I/O
extended I/O
SRAM
```

The exact ranges are discovered or derived from the selected device rather
than hardcoded in `avrmem.c`.

The classification flow is:

```text
ELF DATA VMA
     |
     v
DATA linker-space translation
     |
     v
physical DATA address
     |
     +--> register file
     +--> I/O
     +--> extended I/O
     +--> SRAM
```

---

# 17. Unified address resolution

The preferred device API is:

```c
avr_device_resolve_address()
```

It accepts an ELF/linker address and returns:

```text
memory space
AVR/physical address
physical target address
```

Conceptually:

```text
ELF address
    |
    v
AvrDevice
    |
    +--> FLASH
    +--> REGISTER
    +--> I/O
    +--> SRAM
    +--> EEPROM
```

The caller should not replicate this logic.

---

# 18. `avr_elf.c`

The ELF parser is deliberately independent of MCU-specific address
constants.

It handles:

```text
ELF identification
ELF32 parsing
little-endian decoding
section headers
program headers
symbol table
string tables
ELF AVR device metadata
LMA calculation
symbol resolution
section resolution
```

The parser stores raw ELF values exactly as represented in the ELF file.

Device translation is delegated to `AvrDevice`.

---

# 19. ELF sections

An `AvrElfSection` contains the raw section-header representation.

Important fields include:

```text
name
addr
offset
size
flags
type
```

where:

```text
addr
    =
ELF section VMA

offset
    =
file offset
```

The section header itself does not contain a direct LMA.

---

# 20. ELF program headers and LMA

LMA is calculated from `PT_LOAD`.

Given:

```text
section offset
PT_LOAD.p_offset
PT_LOAD.p_paddr
```

the section's load address is:

```text
section LMA =
  PT_LOAD.p_paddr +
  (section.sh_offset - PT_LOAD.p_offset)
```

For a symbol at a section-relative offset:

```text
symbol LMA =
  section LMA +
  (symbol.value - section.addr)
```

This is particularly important for `.data`.

---

# 21. `.data` handling

A `.data` section typically has:

```text
VMA -> SRAM
LMA -> FLASH
```

Example conceptually:

```text
          ELF
           |
      +----+----+
      |         |
      v         v
     VMA       LMA
      |         |
      v         v
     SRAM      FLASH
```

The runtime object lives in SRAM.

Its initialization image is stored in FLASH.

The symbol resolver keeps these addresses separate.

---

# 22. `.bss` handling

`.bss` is normally represented by:

```text
SHT_NOBITS
```

There are no bytes for the `.bss` contents in the ELF image.

The runtime address still resolves to SRAM, but:

```text
has_lma == false
```

because there is no file-backed initialization image.

The startup code normally zeroes the region.

---

# 23. `.noinit` handling

`.noinit` is a runtime SRAM section whose contents are intended to survive
normal startup initialization.

The section has a VMA, but normally no FLASH initialization image.

Therefore the presentation layer should report the section as preserved rather
than attempting to invent an LMA.

---

# 24. Symbol resolution

Symbol resolution starts with the raw ELF symbol value.

The indexed resolver:

```c
avr_elf_resolve_symbol_at()
```

performs these steps:

```text
raw ELF symbol
      |
      +--> section lookup
      |
      +--> AvrDevice address resolution
      |
      +--> SFR association
      |
      +--> LMA calculation
      |
      +--> FLASH word-address conversion
      |
      v
AvrResolvedSymbol
```

The name-based resolver delegates to the indexed resolver so there is only
one implementation of symbol-resolution semantics.

---

# 25. SFR association

Once a symbol is resolved into AVR DATA space, the device layer can look up
an SFR using its canonical physical address.

Conceptually:

```text
ELF symbol value
       |
       v
AvrDevice address resolution
       |
       v
physical DATA address
       |
       v
avr_device_find_register_address()
       |
       v
AvrSfr
```

This allows a symbol representing or referring to an SFR address to be
displayed with its symbolic register name.

---

# 26. FLASH byte and CPU word addresses

ELF program-space addresses are treated as FLASH byte addresses.

The AVR CPU instruction address may use words instead.

For a traditional AVR:

```text
word address = byte address >> 1
```

The shift is a device property:

```c
avr_device_flash_address_shift()
```

The frontend should never hardcode:

```c
address / 2
```

or:

```c
address >> 1
```

for device-independent output.

---

# 27. Command-line parsing

`avrmem` uses `getopt_long()`.

Canonical syntax:

```text
avrmem [options] <elf>
```

Examples:

```bash
bin/avrmem --sections firmware.elf
bin/avrmem --symbols firmware.elf
bin/avrmem --symbol led_app firmware.elf
bin/avrmem --device atmega328p firmware.elf
bin/avrmem --device=atmega328p --symbol=led_app firmware.elf
```

The option string begins with `+`.

This prevents GNU `getopt_long()` from permuting arguments and means option
processing stops when the first non-option argument is encountered.

Therefore:

```bash
bin/avrmem firmware.elf --sections
```

is intentionally rejected.

---

# 28. Adding a new CLI option

New options should be added in three places in `src/avrmem.c`:

1. `struct option long_options[]`
2. the short-option string
3. the `switch` handling `getopt_long()` results

Then add:

* usage documentation
* README example when appropriate
* testing documentation
* regression tests

The option parser should remain independent of ELF/device implementation
details.

---

# 29. Adding support for a new AVR device

Do not add:

```c
if (strcmp(device_name, "..."))
```

to the core implementation.

Instead:

1. Verify that the installed `avr-gcc` accepts the device.
2. Verify `<avr/io.h>` is selected.
3. Verify the physical-memory macros are available.
4. Verify the generated linker map contains the required regions.
5. Verify SFR macros can be parsed.
6. Build a representative ELF fixture.
7. Add a regression test.

The expected architecture is:

```text
new MCU
   |
   v
existing toolchain probe
   |
   +--> device description
```

not:

```text
new MCU
   |
   v
new source-code table entry
```

---

# 30. Debugging device-probe failures

The probe is intentionally staged.

Diagnostics should identify the stage:

```text
macro probe
physical-memory extraction
SFR discovery
linker-map generation
linker-region extraction
```

When debugging an initialization failure, first determine the first failed
stage.

For example:

```text
probe: probing 'atmega328p'
probe: macro output = ...
probe: physical memory: ...
probe: SFR database = ...
probe: linker command:
  ...
```

If the failure occurs after the linker command, inspect the linker diagnostic
before changing the parser.

The same rule applies to the macro stage.

---

# 31. Temporary files and POSIX behavior

The current probe implementation uses POSIX facilities such as:

```text
mkstemp()
fdopen()
popen()
pclose()
unlink()
```

Temporary paths should use the host's `TMPDIR` convention where appropriate,
with a sensible fallback.

The feature-test macro:

```c
#define _POSIX_C_SOURCE 200809L
```

must appear before system headers in source files that require the
corresponding POSIX declarations.

---

# 32. Memory ownership

Ownership transfers should be explicit.

The probe result initially owns:

```text
result->name
result->sfr_database
```

`avr_device_init()` takes ownership of those values.

The probe result must then be cleared so its destruction routine does not
free transferred objects.

The device destructor releases them.

For ELF:

```text
avr_elf_open()
    |
    v
AvrElf owns parsed data
    |
    v
avr_elf_close()
    |
    v
all ELF-owned data released
```

Do not retain borrowed pointers beyond the lifetime of their owning object.

---

# 33. Error-handling conventions

Public boolean APIs should return:

```text
true
```

only when the requested operation succeeded.

A function should not silently fabricate a result when the mapping is
unknown.

For example, an unknown ELF address should not be guessed as SRAM simply
because its numeric value looks similar to another known address.

Where appropriate:

```text
has_physical_address = false
has_lma = false
has_flash_word_address = false
```

should explicitly communicate unavailable information.

---

# 34. Coding conventions

Use two spaces for indentation.

Prefer clear, small helper functions.

Keep comments focused on:

* address-space semantics
* ownership
* parsing details
* safety checks
* non-obvious toolchain behavior

Avoid comments that merely restate obvious C syntax.

Do not introduce MCU-specific constants into the frontend or ELF parser.

---

# 35. Modifying the architecture safely

Before modifying a central abstraction, check whether the behavior is already
working in another translation unit.

In particular, preserve established mechanisms for:

```text
toolchain probing
linker-map generation
ELF parsing
LMA calculation
SFR normalization
```

when adding new features.

A new feature should normally be layered onto a working path instead of
replacing a proven mechanism with a different discovery strategy.

For example, SFR support should extend the existing AVR-GCC macro probe:

```text
existing macro probe
       |
       +--> physical memory
       |
       +--> device identity
       |
       +--> SFR database
```

rather than creating a second independent compiler invocation.

---

# 36. Documentation workflow

When behavior changes, update the relevant documentation at the same time.

Typical mapping:

```text
source/API change
    |
    +--> docs/api.md

architecture change
    |
    +--> docs/architecture.md

developer workflow change
    |
    +--> docs/guide.md

testing behavior change
    |
    +--> docs/testing.md

user-visible CLI change
    |
    +--> README.md
```

The top-level README should remain concise.

Detailed implementation information belongs in `docs/`.

---

# 37. Jekyll documentation

The `docs/` directory is intended to be a Jekyll site.

The site configuration is in:

```text
docs/_config.yml
```

Documentation pages contain front matter such as:

```yaml
---
layout: default
title: Developer Guide
---
```

The main entry point is:

```text
docs/index.md
```

The project README can be copied to:

```text
docs/README.md
```

for direct browsing from the generated site.

---

# 38. Recommended development cycle

A typical change should follow:

```text
1. Identify the owning abstraction.
2. Modify the smallest appropriate translation unit.
3. Compile with full warnings.
4. Run the focused smoke test.
5. Run the broader test suite.
6. Update documentation.
```

For device/discovery changes:

```text
make clean
make
```

then run the target ELF:

```bash
bin/avrmem --symbol <name> <firmware.elf>
```

and verify:

```text
device
memory classification
physical address
LMA
SFR association
```

---

# 39. Completion criteria for a feature

A feature is considered complete when:

* the implementation belongs to the correct layer
* no unnecessary MCU-specific constants were introduced
* existing behavior remains intact
* errors are reported at the correct abstraction boundary
* the public API is documented
* the developer workflow is documented
* testing instructions cover the new behavior
* the architecture document remains consistent with the implementation
