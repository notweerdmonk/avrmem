# avrmem

`avrmem` is an AVR ELF memory and symbol explorer written in C.

The project separates ELF semantics, AVR device discovery, AVR address
translation, SFR/register metadata, and command-line presentation into
distinct translation units.

## Features

- Parses ELF32 little-endian AVR executables without `libelf`.
- Resolves ELF sections and symbols.
- Resolves symbol VMA, physical address, LMA, and AVR flash word address.
- Automatically discovers the AVR device from ELF device metadata.
- Accepts an explicit `--device` override.
- Discovers physical FLASH, SRAM, and EEPROM sizes from the selected
  AVR-GCC/avr-libc toolchain.
- Discovers AVR-GCC linker address-space regions from the generated linker
  map rather than hardcoding MCU-specific linker constants.
- Parses `_SFR_IO8()` and `_SFR_MEM8()` definitions emitted by avr-libc.
- Associates resolved DATA-space addresses with discovered SFRs.
- Provides section, symbol, memory-model, and memory-map views.

## Project layout

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
    ├── guide.md
    ├── api.md
    ├── architecture.md
    ├── testing.md
    └── README.md
````

`bin/` contains generated binaries and is not intended to contain source
files.

## Requirements

The current host implementation assumes a POSIX environment and requires:

* a C11 compiler
* `avr-gcc`
* `avr-libc`
* AVR binutils selected by `avr-gcc`
* POSIX process and filesystem APIs

The selected AVR device is supplied at runtime with `--device`, or inferred
from the ELF when the option is omitted.

## Build

Normal build:

```bash
make
```

The executable is generated as:

```text
bin/avrmem
```

### Debug build

Any of the following enables the debug configuration:

```bash
make debug
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

## Usage

```bash
bin/avrmem firmware.elf
bin/avrmem --memory firmware.elf
bin/avrmem --sections firmware.elf
bin/avrmem --symbols firmware.elf
bin/avrmem --symbol counter firmware.elf
bin/avrmem --map firmware.elf
bin/avrmem --device atmega328p firmware.elf
bin/avrmem --device atmega328p --symbol counter firmware.elf
```

Options must precede the ELF filename.

Examples:

```bash
bin/avrmem --sections firmware.elf
bin/avrmem --device atmega328p --sections firmware.elf
bin/avrmem --device atmega328p --symbol led_app firmware.elf
```

## Automatic device discovery

When `--device` is omitted, `avrmem` opens the ELF and attempts to obtain
the AVR device name from the ELF's AVR device metadata.

The discovered device name is then passed to the device-probe layer.

When `--device` is supplied, the explicit device name is used for device
initialization.

## Address model

The project distinguishes between the ELF/linker address representation and
the physical AVR address.

Conceptually:

```text
ELF/linker address
        |
        v
AvrDevice address classification
        |
        v
physical AVR address
```

This distinction is important for AVR DATA-space addresses.

For example, an ELF DATA-space address can represent a physical SRAM
address after removing the device's linker-space origin.

### FLASH

Program-memory ELF addresses are represented as byte addresses.

The device model additionally provides the conversion from a physical FLASH
byte address to the AVR CPU instruction/word address.

### SRAM

`.data`, `.bss`, stack, heap, and `.noinit` are runtime SRAM allocations.

They are not separate physical memory types.

Their differences are determined by ELF sections and initialization
semantics.

### `.data`

A `.data` object normally has:

```text
runtime VMA -> SRAM
load LMA    -> FLASH
```

The symbol LMA is derived from the section's load mapping.

### `.bss`

`.bss` is `SHT_NOBITS`, so it has runtime storage but no file-backed
initialization image.

### `.noinit`

`.noinit` is runtime SRAM whose contents are preserved across startup rather
than initialized from the FLASH image.

## Device discovery architecture

Device information is deliberately not maintained as a hardcoded MCU table.

The probe layer uses the selected AVR-GCC toolchain:

```text
avr-gcc -mmcu=<device>
```

The preprocessor environment provides:

* physical memory information
* AVR device identity
* SFR definitions

The linker map provides:

* TEXT linker region
* DATA linker region
* EEPROM linker region

This keeps the device abstraction synchronized with the installed AVR
toolchain.

## SFR discovery

The SFR database is derived from concrete avr-libc macro definitions such as:

```c
#define PORTB  _SFR_IO8(0x05)
#define UCSR0A _SFR_MEM8(0xC0)
```

The SFR layer normalizes these into physical AVR DATA-space addresses.

For `_SFR_IO8()`:

```text
physical DATA address =
    encoded I/O address + __SFR_OFFSET
```

For `_SFR_MEM8()`:

```text
physical DATA address =
    encoded memory address
```

The device abstraction then provides name and address lookup.

## Documentation

The `docs/` directory is also the source tree for a Jekyll documentation
site.

Documentation includes:

* [Developer guide](docs/guide.md)
* [API reference](docs/api.md)
* [Testing guide](docs/testing.md)
* [Architecture / ADR](docs/architecture.md)

A copy of this README is maintained under `docs/README.md` for use by the
documentation site.

## Development principles

The main architectural constraints are:

1. Do not add MCU-specific constants to `avrmem.c`.
2. Do not parse ELF structures in the command-line frontend.
3. Do not duplicate AVR address calculations outside `avr_device.c`.
4. Do not create per-MCU initialization functions.
5. Prefer discovery from the installed AVR-GCC/avr-libc toolchain.
6. Keep physical memory and ELF/linker address spaces distinct.
7. Treat SFR discovery as an auxiliary device capability.
8. Keep ownership of allocated resources explicit.

## Testing

A basic smoke-test sequence is:

```bash
make clean
make

bin/avrmem firmware.elf
bin/avrmem --sections firmware.elf
bin/avrmem --symbols firmware.elf
bin/avrmem --map firmware.elf
bin/avrmem --symbol led_app firmware.elf
bin/avrmem --device atmega328p --symbol led_app firmware.elf
```

See [docs/testing.md](docs/testing.md) for the complete testing procedure.

## License

MIT
