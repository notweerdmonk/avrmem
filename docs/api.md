---
layout: default
title: API Reference
---

# avrmem API Reference

This document describes the public interfaces exposed by the project's
device, SFR, ELF, and device-probe abstractions.

The command-line frontend in `src/avrmem.c` should consume these APIs rather
than duplicating ELF parsing or AVR address calculations.

---

## `avr_device.h`

### `AvrMemorySpace`

```c
typedef enum {
  AVR_MEM_UNKNOWN = 0,
  AVR_MEM_FLASH,
  AVR_MEM_REGISTER,
  AVR_MEM_IO,
  AVR_MEM_SRAM,
  AVR_MEM_EEPROM
} AvrMemorySpace;
````

Identifies the AVR physical/address-space category to which a resolved
ELF/linker address belongs.

`AVR_MEM_REGISTER` identifies the CPU register file.

`AVR_MEM_IO` identifies AVR I/O and extended-I/O space.

`AVR_MEM_SRAM` identifies physical SRAM.

---

### `AvrMemoryRegion`

```c
typedef struct {
  uint32_t start;
  uint32_t size;
} AvrMemoryRegion;
```

Describes one contiguous physical memory region.

`start` is the physical address of the first byte.

`size` is the number of bytes in the region.

---

### `AvrAddressSpace`

```c
typedef struct {
  uint32_t origin;
  uint32_t length;
} AvrAddressSpace;
```

Describes an ELF/linker address space.

This is deliberately different from `AvrMemoryRegion`.

For example, an AVR-GCC DATA linker address may differ from the physical
SRAM address to which it resolves.

---

### `AvrDevice`

`AvrDevice` is the main public device abstraction.

It owns:

* canonical device name
* physical FLASH region
* physical SRAM region
* physical EEPROM region
* ELF/linker TEXT space
* ELF/linker DATA space
* ELF/linker EEPROM space
* AVR DATA-space subregions
* SFR database
* FLASH byte-to-word address representation

The structure is populated by `avr_device_init()`.

---

## Device lifecycle

### `avr_device_init`

```c
bool avr_device_init(
  AvrDevice *device,
  const char *device_name
);
```

Initialize an `AvrDevice` for the named AVR target.

`device_name` must be a device name accepted by:

```text
avr-gcc -mmcu=<device>
```

Example:

```text
atmega328p
```

On success, the device owns the discovered device name and SFR database.

Returns `true` on success.

---

### `avr_device_destroy`

```c
void avr_device_destroy(
  AvrDevice *device
);
```

Release all resources owned by an `AvrDevice`.

Always pair a successful `avr_device_init()` with
`avr_device_destroy()`.

---

## Device identity

### `avr_device_name`

```c
const char *avr_device_name(
  const AvrDevice *device
);
```

Return the canonical device name.

The returned string is owned by `AvrDevice`.

It remains valid until `avr_device_destroy()`.

---

## Physical memory accessors

### `avr_device_flash_region`

```c
AvrMemoryRegion avr_device_flash_region(
  const AvrDevice *device
);
```

Return the physical FLASH region.

---

### `avr_device_sram_region`

```c
AvrMemoryRegion avr_device_sram_region(
  const AvrDevice *device
);
```

Return the physical SRAM region.

Runtime `.data`, `.bss`, stack, heap, and `.noinit` storage reside in this
physical region.

---

### `avr_device_eeprom_region`

```c
AvrMemoryRegion avr_device_eeprom_region(
  const AvrDevice *device
);
```

Return the physical EEPROM region.

---

## ELF/linker address-space accessors

### `avr_device_data_region_origin`

```c
uint32_t avr_device_data_region_origin(
  const AvrDevice *device
);
```

Return the origin of the AVR-GCC DATA linker space.

---

### `avr_device_data_region_length`

```c
uint32_t avr_device_data_region_length(
  const AvrDevice *device
);
```

Return the length of the AVR-GCC DATA linker space.

---

### `avr_device_text_region_origin`

```c
uint32_t avr_device_text_region_origin(
  const AvrDevice *device
);
```

Return the origin of the AVR-GCC TEXT linker space.

---

### `avr_device_text_region_length`

```c
uint32_t avr_device_text_region_length(
  const AvrDevice *device
);
```

Return the length of the AVR-GCC TEXT linker space.

---

### `avr_device_eeprom_region_origin`

```c
uint32_t avr_device_eeprom_region_origin(
  const AvrDevice *device
);
```

Return the origin of the AVR-GCC EEPROM linker space.

---

### `avr_device_eeprom_region_length`

```c
uint32_t avr_device_eeprom_region_length(
  const AvrDevice *device
);
```

Return the length of the AVR-GCC EEPROM linker space.

---

## Flash address representation

### `avr_device_flash_address_shift`

```c
uint32_t avr_device_flash_address_shift(
  const AvrDevice *device
);
```

Return the device-specific shift used to convert a physical FLASH byte
address into an AVR CPU instruction/word address.

For conventional AVR:

```text
word_address = byte_address >> 1
```

and therefore the shift is `1`.

---

## Address classification

### `avr_device_classify_address`

```c
AvrMemorySpace avr_device_classify_address(
  const AvrDevice *device,
  uint32_t address
);
```

Classify an address expressed in ELF/linker address space.

The classification is based on the selected device's linker regions and
physical DATA-space layout.

---

## Address translation

### `avr_device_to_physical_sram`

```c
bool avr_device_to_physical_sram(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address
);
```

Translate an ELF/linker DATA address to a physical SRAM address.

Returns `false` when the address does not resolve to physical SRAM.

---

### `avr_device_to_physical_flash`

```c
bool avr_device_to_physical_flash(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address
);
```

Translate an ELF/linker TEXT address to a physical FLASH byte address.

---

### `avr_device_to_physical_eeprom`

```c
bool avr_device_to_physical_eeprom(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address
);
```

Translate an ELF/linker EEPROM address to a physical EEPROM address.

---

### `avr_device_flash_byte_to_word`

```c
bool avr_device_flash_byte_to_word(
  const AvrDevice *device,
  uint32_t byte_address,
  uint32_t *word_address
);
```

Convert a physical FLASH byte address into the AVR CPU instruction/word
address.

The input is a physical byte address.

---

### `avr_device_resolve_address`

```c
bool avr_device_resolve_address(
  const AvrDevice *device,
  uint32_t address,
  AvrMemorySpace *memory_space,
  uint32_t *avr_address,
  uint32_t *physical_address
);
```

Resolve an ELF/linker address in one operation.

On success:

```text
address
  |
  +--> memory_space
  +--> avr_address
  +--> physical_address
```

The output pointers are optional and may be `NULL`.

For a DATA-space SRAM address, `avr_address` is the physical AVR DATA-space
address.

---

## SFR access

### `avr_device_find_register`

```c
const AvrSfr *avr_device_find_register(
  const AvrDevice *device,
  const char *name
);
```

Find an SFR by symbolic name.

Example:

```c
const AvrSfr *portb =
  avr_device_find_register(
    &device,
    "PORTB"
  );
```

The returned object is owned by `AvrDevice`.

---

### `avr_device_find_register_address`

```c
const AvrSfr *avr_device_find_register_address(
  const AvrDevice *device,
  uint16_t data_address
);
```

Find an SFR by canonical physical AVR DATA-space address.

---

### `avr_memory_space_name`

```c
const char *avr_memory_space_name(
  AvrMemorySpace space
);
```

Return a printable name for an `AvrMemorySpace`.

---

# `avr_sfr.h`

## `AvrSfr`

An `AvrSfr` describes one discovered Special Function Register.

The database records:

* symbolic name
* normalized physical DATA-space address
* original SFR encoding
* address-space encoding type
* register width

SFR definitions are obtained from the selected avr-libc headers rather than
from a hardcoded MCU table.

---

## `AvrSfrDatabase`

`AvrSfrDatabase` owns the parsed collection of SFR definitions.

It is owned by `AvrDevice`.

---

## Database lifecycle

### `avr_sfr_init`

```c
bool avr_sfr_init(
  AvrSfrDatabase **out_database
);
```

Create an empty SFR database.

Returns `true` on success.

---

### `avr_sfr_destroy`

```c
void avr_sfr_destroy(
  AvrSfrDatabase *database
);
```

Destroy an SFR database and release all owned resources.

---

## Parsing

### `avr_sfr_parse`

```c
bool avr_sfr_parse(
  AvrSfrDatabase *database,
  const char *preprocessor_output
);
```

Parse concrete avr-gcc preprocessor output.

Typical input contains definitions such as:

```c
#define PORTB _SFR_IO8(0x05)
#define UCSR0A _SFR_MEM8(0xC0)
```

The parser normalizes them into physical AVR DATA-space addresses.

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

## Database queries

### `avr_sfr_count`

```c
size_t avr_sfr_count(
  const AvrSfrDatabase *database
);
```

Return the number of discovered SFRs.

---

### `avr_sfr_at`

```c
const AvrSfr *avr_sfr_at(
  const AvrSfrDatabase *database,
  size_t index
);
```

Return an SFR by database index.

The returned pointer remains valid until the database is destroyed.

---

### `avr_sfr_find`

```c
const AvrSfr *avr_sfr_find(
  const AvrSfrDatabase *database,
  const char *name
);
```

Find an SFR by symbolic name.

---

### `avr_sfr_find_address`

```c
const AvrSfr *avr_sfr_find_address(
  const AvrSfrDatabase *database,
  uint16_t address
);
```

Find an SFR by canonical physical DATA-space address.

---

# `avr_device_probe.h`

The probe interface is an internal layer between the installed AVR
toolchain and `AvrDevice`.

## `AvrDeviceProbeResult`

The result contains discovered:

* device name
* physical FLASH
* physical SRAM
* physical EEPROM
* DATA linker region
* TEXT linker region
* EEPROM linker region
* FLASH address shift
* optional SFR database

Ownership of `name` and `sfr_database` belongs to the probe result until
they are transferred into `AvrDevice`.

---

### `avr_device_probe`

```c
bool avr_device_probe(
  const char *device_name,
  AvrDeviceProbeResult *result
);
```

Discover the selected AVR device.

The probe uses the installed AVR toolchain rather than a hardcoded MCU
database.

---

### `avr_device_probe_destroy`

```c
void avr_device_probe_destroy(
  AvrDeviceProbeResult *result
);
```

Release resources owned by a probe result.

---

# `avr_elf.h`

## `AvrElf`

`AvrElf` is an opaque object owning:

* the ELF image
* parsed ELF sections
* parsed program headers
* parsed symbols
* ELF string tables
* ELF-derived device-name metadata

---

## ELF lifecycle

### `avr_elf_open`

```c
bool avr_elf_open(
  const char *filename,
  AvrElf **out_elf
);
```

Open and completely parse an ELF32 AVR executable.

The parser currently expects:

```text
ELF32
little-endian
EM_AVR
ET_EXEC
```

---

### `avr_elf_close`

```c
void avr_elf_close(
  AvrElf *elf
);
```

Release the `AvrElf` object.

All pointers returned by ELF accessors become invalid after this call.

---

## Basic ELF information

### `avr_elf_machine`

```c
uint16_t avr_elf_machine(
  const AvrElf *elf
);
```

Return the ELF `e_machine` value.

---

### `avr_elf_get_machine`

```c
uint16_t avr_elf_get_machine(
  const AvrElf *elf
);
```

Alias/accessor for the ELF machine identifier.

---

### `avr_elf_entry`

```c
uint32_t avr_elf_entry(
  const AvrElf *elf
);
```

Return the ELF entry point.

---

## ELF device metadata

### `avr_elf_get_device_name`

```c
const char *avr_elf_get_device_name(
  const AvrElf *elf
);
```

Return the AVR device name discovered from ELF AVR device metadata.

The string is owned by `AvrElf`.

It remains valid until `avr_elf_close()`.

Returns `NULL` when the ELF does not provide a usable device name.

---

# ELF sections

## `AvrElfSection`

An `AvrElfSection` describes an ELF section in its original ELF
representation.

Important fields include:

* `name`
* `index`
* `type`
* `addr`
* `offset`
* `size`
* `flags`
* `alloc`
* `writable`
* `executable`
* `nobits`

`addr` is the ELF section VMA.

`offset` is the file offset.

LMA is not stored here; it is derived through `avr_elf_section_lma()`.

---

### `avr_elf_section_count`

```c
size_t avr_elf_section_count(
  const AvrElf *elf
);
```

Return the number of ELF sections.

---

### `avr_elf_section`

```c
const AvrElfSection *avr_elf_section(
  const AvrElf *elf,
  size_t index
);
```

Return a section by index.

---

### `avr_elf_section_at`

```c
const AvrElfSection *avr_elf_section_at(
  const AvrElf *elf,
  uint16_t index
);
```

Indexed section accessor used by the frontend.

---

### `avr_elf_find_section`

```c
const AvrElfSection *avr_elf_find_section(
  const AvrElf *elf,
  const char *name
);
```

Find a section by name.

---

# Resolved sections

## `AvrResolvedSection`

A resolved section combines ELF information with device-specific address
translation.

Important fields include:

```text
section
value
size
avr_address
physical_address
has_physical_address
lma
has_lma
memory_space
```

---

### `avr_elf_resolve_section`

```c
bool avr_elf_resolve_section(
  const AvrElf *elf,
  const AvrDevice *device,
  uint16_t section_index,
  AvrResolvedSection *resolved
);
```

Resolve one ELF section against a selected `AvrDevice`.

The returned `physical_address` is derived through the device abstraction.

The returned LMA is derived from the ELF program headers.

---

# ELF symbols

## `AvrElfSymbol`

`AvrElfSymbol` describes a raw ELF symbol.

Important fields include:

* `name`
* `value`
* `size`
* `section_index`
* `type`
* `bind`
* `info`
* `other`

`value` is the original ELF symbol value.

---

### `avr_elf_symbol_count`

```c
size_t avr_elf_symbol_count(
  const AvrElf *elf
);
```

Return the number of parsed symbols.

---

### `avr_elf_symbol`

```c
const AvrElfSymbol *avr_elf_symbol(
  const AvrElf *elf,
  size_t index
);
```

Return a raw symbol by index.

---

### `avr_elf_symbol_at`

```c
const AvrElfSymbol *avr_elf_symbol_at(
  const AvrElf *elf,
  size_t index
);
```

Explicit indexed symbol accessor.

---

### `avr_elf_symbol_is_useful`

```c
bool avr_elf_symbol_is_useful(
  const AvrElfSymbol *symbol
);
```

Return whether the symbol is appropriate for normal user-facing symbol
presentation.

The current policy includes:

```text
STT_OBJECT
STT_FUNC
STT_NOTYPE
```

---

# Resolved symbols

## `AvrResolvedSymbol`

A resolved symbol combines raw ELF information with device translation.

Important fields include:

```text
name
value
size
section_index
memory_space
avr_address
physical_address
has_physical_address
sfr
lma
has_lma
flash_word_address
has_flash_word_address
section
symbol
```

For a known SFR:

```text
memory_space
      |
      v
AVR_MEM_REGISTER / AVR_MEM_IO
      |
      v
sfr -> AvrSfr
```

---

### `avr_elf_resolve_symbol`

```c
bool avr_elf_resolve_symbol(
  const AvrElf *elf,
  const AvrDevice *device,
  const char *name,
  AvrResolvedSymbol *result
);
```

Resolve a symbol by name.

Local symbols are included in the lookup.

---

### `avr_elf_resolve_symbol_at`

```c
bool avr_elf_resolve_symbol_at(
  const AvrElf *elf,
  const AvrDevice *device,
  size_t symbol_index,
  AvrResolvedSymbol *resolved
);
```

Resolve a symbol by symbol-table index.

This is the central indexed symbol-resolution implementation.

---

### `avr_elf_resolve_symbol_index`

```c
bool avr_elf_resolve_symbol_index(
  const AvrElf *elf,
  const AvrDevice *device,
  size_t symbol_index,
  AvrResolvedSymbol *result
);
```

Compatibility wrapper around the indexed symbol resolver.

---

# LMA API

### `avr_elf_section_lma`

```c
bool avr_elf_section_lma(
  const AvrElf *elf,
  const AvrElfSection *section,
  uint32_t section_offset,
  uint32_t *lma
);
```

Resolve the load-memory address corresponding to an offset within a
file-backed section.

Conceptually:

```text
LMA =
  PT_LOAD.p_paddr
  + (section.sh_offset - PT_LOAD.p_offset)
  + section_offset
```

`SHT_NOBITS` sections do not have file-backed initialization bytes and
therefore have no LMA.

---

### `avr_elf_symbol_lma`

```c
bool avr_elf_symbol_lma(
  const AvrElf *elf,
  const AvrElfSymbol *symbol,
  uint32_t *lma
);
```

Resolve the LMA of a symbol by calculating its section-relative offset and
delegating to `avr_elf_section_lma()`.

---

# API ownership and lifetime

The public API uses borrowed pointers extensively.

## AvrDevice-owned data

These remain valid until:

```c
avr_device_destroy()
```

including:

* `avr_device_name()` result
* `AvrSfr *` returned by SFR lookup

## AvrElf-owned data

These remain valid until:

```c
avr_elf_close()
```

including:

* section pointers
* symbol pointers
* section names
* symbol names
* ELF-derived device name

`avrmem.c` must not free any object returned through these APIs.

---

# Recommended usage pattern

Typical application initialization is:

```c
AvrElf *elf = NULL;
AvrDevice device;

memset(&device, 0, sizeof(device));

if (!avr_elf_open(
      filename,
      &elf
    ))
{
  /* error */
}

const char *device_name =
  avr_elf_get_device_name(elf);

if (!avr_device_init(
      &device,
      device_name
    ))
{
  avr_elf_close(elf);
  /* error */
}

/* Resolve/query/display data. */

avr_elf_close(elf);
avr_device_destroy(&device);
```

For command-line applications, an explicitly supplied `--device` should be
used in preference to automatic ELF device discovery.
