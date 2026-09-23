---
layout: default
title: avrmem
---

# avrmem

AVR ELF memory and symbol explorer.

`avrmem` analyzes AVR ELF32 executables and presents:

- ELF sections and their VMA/LMA mappings
- ELF symbols and their AVR memory mappings
- physical FLASH, SRAM, and EEPROM regions
- AVR-GCC linker address-space regions
- AVR SFR/register information
- `.data` initialization relationships
- complete AVR memory maps

## Documentation

- [Developer Guide](guide.md)
- [API Reference](api.md)
- [Testing Guide](testing.md)
- [Architecture](architecture.md)
- [Project README](../README.md)

## Project overview

The project separates the following responsibilities:

```text
                    +----------------+
                    |    avrmem.c    |
                    | CLI/presentation|
                    +--------+-------+
                             |
             +---------------+---------------+
             |                               |
             v                               v
      +-------------+                +---------------+
      |  avr_elf.c  |                | avr_device.c |
      | ELF parser  |                | Device model |
      +------+------+                +-------+-------+
             |                               |
             |                               v
             |                       +---------------+
             |                       | Probe layer   |
             |                       +-------+-------+
             |                               |
             |                +--------------+--------------+
             |                |                             |
             v                v                             v
        ELF image       avr-gcc -dM -E              avr-gcc linker map
                             |                             |
                             v                             v
                         avr_sfr.c                 linker regions
````

## Source tree

```text
src/
include/
bin/
docs/
```

The executable produced by the build is:

```text
bin/avrmem
```

## Command-line examples

```bash
bin/avrmem firmware.elf
bin/avrmem --memory firmware.elf
bin/avrmem --sections firmware.elf
bin/avrmem --symbols firmware.elf
bin/avrmem --symbol led_app firmware.elf
bin/avrmem --map firmware.elf
bin/avrmem --device atmega328p firmware.elf
```

Options must precede the ELF filename.

```

### `docs/README.md`

This is the documentation-site copy of the top-level README. Use the same content as the root `README.md`, with links adjusted for the `docs/` location.
