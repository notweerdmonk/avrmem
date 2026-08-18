/*
 * avrmem - AVR ELF memory and symbol explorer
 * Copyright (C) 2026 notweerdmonk
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
 * SOFTWARE.
*/

/**
 * @file avr_elf.h
 * @author notweerdmonk, gpt-5.6-luna
 * @brief ELF32-AVR parser and symbol/address resolution interface.
 *
 * @details
 * This interface exposes the ELF abstraction used by avrmem.
 *
 * Responsibilities of this module include:
 *
 * - reading and validating an AVR ELF32 executable;
 * - parsing ELF sections, program headers, and symbol tables;
 * - resolving symbols by name or symbol-table index;
 * - resolving ELF/linker addresses through @ref AvrDevice;
 * - resolving section and symbol load-memory addresses (LMAs) from
 *   PT_LOAD segments;
 * - obtaining AVR device identity metadata embedded in the ELF.
 *
 * Device-specific memory mapping is deliberately delegated to
 * @ref AvrDevice.  This module must not contain MCU-specific memory
 * constants.
 *
 * SFR/register interpretation is likewise delegated to the device layer.
 * The ELF layer may attach an @ref AvrSfr pointer to a resolved symbol,
 * but it does not parse avr-libc SFR definitions itself.
 */

#ifndef AVR_ELF_H
#define AVR_ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "avr_device.h"


/**
 * @def AVR_ELF_MACHINE_AVR
 * @brief ELF machine identifier for AVR executables.
 *
 * @details
 * `EM_AVR` has the value 83.
 *
 * The value is defined locally so that the public ELF interface does not
 * depend on a host system `<elf.h>` implementation.
 */
#define AVR_ELF_MACHINE_AVR 83


/**
 * @def AVR_ELF_SHN_UNDEF
 * @brief ELF section index identifying an undefined symbol.
 */
#define AVR_ELF_SHN_UNDEF  0

/**
 * @def AVR_ELF_SHN_ABS
 * @brief ELF section index identifying an absolute symbol.
 */
#define AVR_ELF_SHN_ABS    0xfff1

/**
 * @def AVR_ELF_SHN_COMMON
 * @brief ELF section index identifying a common symbol.
 */
#define AVR_ELF_SHN_COMMON 0xfff2


/**
 * @struct AvrElf
 * @brief Opaque representation of a parsed AVR ELF executable.
 *
 * @details
 * The concrete representation is private to `avr_elf.c`.
 *
 * An @ref AvrElf owns:
 *
 * - the complete ELF file image;
 * - parsed section information;
 * - parsed program headers;
 * - parsed symbols;
 * - ELF string tables;
 * - ELF-derived device metadata.
 *
 * Pointers returned by this API remain valid only until
 * @ref avr_elf_close is called.
 */
typedef struct AvrElf AvrElf;


/**
 * @struct AvrElfSection
 * @brief Parsed representation of one ELF section.
 *
 * @details
 * The fields are maintained in the ELF representation.
 *
 * In particular:
 *
 * - @c addr is the ELF section VMA;
 * - @c offset is the file offset;
 * - @c size is the section size in bytes.
 *
 * ELF section headers do not directly contain an LMA.  The LMA is derived
 * from file-backed PT_LOAD program headers by @ref avr_elf_section_lma.
 */
typedef struct {

  /**
   * Section name.
   *
   * @details
   * The string is owned by @ref AvrElf and must not be modified or freed.
   */
  const char *name;

  /**
   * ELF section-table index.
   */
  uint32_t index;

  /**
   * ELF section type (`sh_type`).
   */
  uint32_t type;

  /**
   * ELF section virtual address / VMA (`sh_addr`).
   */
  uint32_t addr;

  /**
   * File offset of the section contents (`sh_offset`).
   */
  uint32_t offset;

  /**
   * Section size in bytes (`sh_size`).
   */
  uint32_t size;

  /**
   * ELF section flags (`sh_flags`).
   */
  uint32_t flags;

  /**
   * ELF section link field (`sh_link`).
   */
  uint32_t link;

  /**
   * ELF section information field (`sh_info`).
   */
  uint32_t info;

  /**
   * Required section alignment (`sh_addralign`).
   */
  uint32_t alignment;

  /**
   * ELF section entry size (`sh_entsize`).
   */
  uint32_t entry_size;

  /**
   * `true` when the section has the ELF `SHF_ALLOC` flag.
   */
  bool alloc;

  /**
   * `true` when the section has the ELF `SHF_WRITE` flag.
   */
  bool writable;

  /**
   * `true` when the section has the ELF `SHF_EXECINSTR` flag.
   */
  bool executable;

  /**
   * `true` when the section type is `SHT_NOBITS`.
   *
   * @details
   * Such sections occupy runtime memory but do not contain file-backed
   * bytes in the ELF image.
   */
  bool nobits;

} AvrElfSection;


/**
 * @struct AvrElfSymbol
 * @brief Raw representation of one ELF symbol-table entry.
 *
 * @details
 * The fields are extracted directly from the ELF32 symbol entry.
 *
 * @c value is the exact value stored in the ELF symbol table.  No
 * device-specific address translation is performed in this structure.
 */
typedef struct {

  /**
   * Symbol name.
   *
   * @details
   * The string is owned by @ref AvrElf.
   */
  const char *name;

  /**
   * Symbol-table index.
   */
  uint32_t index;

  /**
   * Raw ELF symbol value (`st_value`).
   */
  uint32_t value;

  /**
   * Symbol size in bytes (`st_size`).
   */
  uint32_t size;

  /**
   * Raw ELF symbol information byte (`st_info`).
   */
  uint8_t info;

  /**
   * Raw ELF symbol visibility/other byte (`st_other`).
   */
  uint8_t other;

  /**
   * ELF section-table index associated with the symbol (`st_shndx`).
   */
  uint16_t section_index;

  /**
   * Extracted ELF symbol type.
   *
   * @details
   * Equivalent to `ELF32_ST_TYPE(info)`.
   */
  uint8_t type;

  /**
   * Extracted ELF symbol binding.
   *
   * @details
   * Equivalent to `ELF32_ST_BIND(info)`.
   */
  uint8_t bind;

} AvrElfSymbol;


/**
 * @struct AvrResolvedSymbol
 * @brief Fully resolved ELF symbol.
 *
 * @details
 * This structure combines raw ELF symbol information with the
 * device-specific interpretation of the symbol address.
 *
 * A typical SRAM symbol may be represented as:
 *
 * @code
 * ELF value:
 *     0x00800123
 *
 * memory space:
 *     AVR_MEM_SRAM
 *
 * avr_address:
 *     0x00000123
 *
 * physical_address:
 *     0x00000123
 * @endcode
 *
 * For an SFR symbol, the resolved object may additionally point to the
 * corresponding @ref AvrSfr:
 *
 * @code
 * memory_space = AVR_MEM_IO
 * avr_address  = 0x00000025
 * sfr           -> "PORTB"
 * @endcode
 *
 * Runtime/VMA and load/LMA are intentionally represented independently.
 */
typedef struct {

  /**
   * Symbol name.
   *
   * @details
   * Owned by @ref AvrElf.
   */
  const char *name;

  /**
   * Raw ELF symbol value.
   */
  uint32_t value;

  /**
   * Symbol size in bytes.
   */
  uint32_t size;

  /**
   * ELF section-table index associated with the symbol.
   */
  uint16_t section_index;


  /**
   * @name Device-resolved address
   * @{
   */

  /**
   * Memory-space classification produced by @ref AvrDevice.
   */
  AvrMemorySpace memory_space;

  /**
   * Physical AVR address within the classified memory space.
   *
   * @details
   * This is the device-resolved AVR address rather than the original
   * ELF/linker address.
   */
  uint32_t avr_address;

  /**
   * Physical target-memory address.
   *
   * @details
   * This is retained separately from @ref avr_address because future AVR
   * architectures may distinguish the logical AVR address representation
   * from an externally addressable physical-memory address.
   */
  uint32_t physical_address;

  /**
   * Indicates whether @ref physical_address contains a valid result.
   */
  bool has_physical_address;

  /** @} */


  /**
   * @name SFR information
   * @{
   */

  /**
   * Matching SFR definition, when the resolved DATA-space address
   * corresponds to a known SFR.
   *
   * @details
   * The object is owned by @ref AvrDevice.
   *
   * The pointer becomes invalid when the associated device is destroyed.
   */
  const AvrSfr *sfr;

  /** @} */


  /**
   * @name Load-memory address
   * @{
   */

  /**
   * Load-memory address of the symbol.
   *
   * @details
   * For a `.data` symbol, runtime VMA normally refers to SRAM while
   * this LMA refers to the FLASH initialization image.
   */
  uint32_t lma;

  /**
   * Indicates whether @ref lma is available.
   *
   * @details
   * For `SHT_NOBITS` sections such as `.bss`, this is `false`.
   */
  bool has_lma;

  /** @} */


  /**
   * @name FLASH CPU word address
   * @{
   */

  /**
   * AVR CPU instruction/word address for a FLASH-resident symbol.
   */
  uint32_t flash_word_address;

  /**
   * Indicates whether @ref flash_word_address is valid.
   */
  bool has_flash_word_address;

  /** @} */


  /**
   * @name Containing ELF section
   * @{
   */

  /**
   * ELF section containing the symbol.
   *
   * @details
   * The section is owned by @ref AvrElf.
   */
  const AvrElfSection *section;

  /** @} */


  /**
   * Raw ELF symbol-table information.
   *
   * This is a value copy of the original symbol representation.
   */
  AvrElfSymbol symbol;

} AvrResolvedSymbol;


/**
 * @struct AvrResolvedSection
 * @brief ELF section resolved against a selected AVR device.
 *
 * @details
 * @ref AvrElfSection describes the raw ELF section.
 * `AvrResolvedSection` adds:
 *
 * - device-specific address interpretation;
 * - physical address;
 * - memory-space classification;
 * - load-memory address.
 */
typedef struct {

  /**
   * Original ELF section.
   *
   * @details
   * The section is owned by @ref AvrElf.
   */
  const AvrElfSection *section;

  /**
   * ELF section VMA.
   */
  uint32_t value;

  /**
   * Section size in bytes.
   */
  uint32_t size;

  /**
   * Physical AVR address corresponding to the section VMA.
   */
  uint32_t avr_address;

  /**
   * Physical target-memory address corresponding to the section.
   */
  uint32_t physical_address;

  /**
   * Indicates whether @ref physical_address is valid.
   */
  bool has_physical_address;

  /**
   * Section load-memory address.
   *
   * @details
   * Derived from a file-backed PT_LOAD program segment.
   */
  uint32_t lma;

  /**
   * Indicates whether @ref lma is available.
   */
  bool has_lma;

  /**
   * Device-specific memory-space classification.
   */
  AvrMemorySpace memory_space;

} AvrResolvedSection;


/**
 * @brief Open and completely parse an ELF file.
 *
 * @param[in] filename
 *   Path to the ELF executable.
 *
 * @param[out] out_elf
 *   Receives the newly allocated @ref AvrElf object on success.
 *
 * @return
 *   `true` when the ELF file is successfully loaded and parsed;
 *   `false` otherwise.
 *
 * @details
 * The parser validates the ELF format and populates all information
 * required by later queries, including:
 *
 * - ELF headers;
 * - section headers;
 * - program headers;
 * - section names;
 * - symbol table;
 * - symbol names;
 * - AVR device metadata when available.
 *
 * The returned object owns its complete parsed representation.
 *
 * @note
 * The caller must release a successfully opened ELF object with
 * @ref avr_elf_close.
 */
bool
avr_elf_open(
  const char *filename,
  AvrElf **out_elf
);


/**
 * @brief Release an ELF object and all data owned by it.
 *
 * @param[in,out] elf
 *   ELF object to destroy.
 *
 * @note
 * It is safe to pass `NULL`.
 *
 * @warning
 * All pointers previously returned from the ELF API become invalid after
 * this call.
 */
void
avr_elf_close(
  AvrElf *elf
);


/**
 * @brief Return the ELF machine identifier.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @return
 *   Raw ELF `e_machine` value, or `0` when @p elf is `NULL`.
 */
uint16_t
avr_elf_machine(
  const AvrElf *elf
);


/**
 * @brief Return the ELF entry point.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @return
 *   ELF `e_entry` value, or `0` when @p elf is `NULL`.
 */
uint32_t
avr_elf_entry(
  const AvrElf *elf
);


/**
 * @brief Return the number of parsed ELF sections.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @return
 *   Number of sections, or `0` when @p elf is `NULL`.
 */
size_t
avr_elf_section_count(
  const AvrElf *elf
);


/**
 * @brief Return an ELF section by section-table index.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] index
 *   Zero-based section-table index.
 *
 * @return
 *   Pointer to the corresponding @ref AvrElfSection, or `NULL` when the
 *   index is outside the section table or @p elf is `NULL`.
 *
 * @warning
 * The returned section is owned by @ref AvrElf and must not be modified
 * or freed by the caller.
 */
const AvrElfSection *
avr_elf_section(
  const AvrElf *elf,
  size_t index
);


/**
 * @brief Return an ELF section by section-table index.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] index
 *   ELF section-table index.
 *
 * @return
 *   Pointer to the corresponding @ref AvrElfSection, or `NULL` when
 *   unavailable.
 *
 * @details
 * This is the explicit indexed accessor intended for callers that
 * operate directly on ELF section indices.
 */
const AvrElfSection *
avr_elf_section_at(
  const AvrElf *elf,
  uint16_t index
);


/**
 * @brief Find an ELF section by name.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] name
 *   NUL-terminated section name.
 *
 * @return
 *   Pointer to the matching @ref AvrElfSection, or `NULL` when no section
 *   with that name exists.
 */
const AvrElfSection *
avr_elf_find_section(
  const AvrElf *elf,
  const char *name
);


/**
 * @brief Resolve an ELF section against an AVR device.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] device
 *   Device used to interpret the ELF/linker address.
 *
 * @param[in] section_index
 *   ELF section-table index.
 *
 * @param[out] resolved
 *   Receives the fully resolved section.
 *
 * @return
 *   `true` when the section can be resolved; `false` otherwise.
 *
 * @details
 * Resolution includes:
 *
 * - ELF VMA;
 * - device memory-space classification;
 * - physical AVR address;
 * - physical target address;
 * - section LMA when available.
 *
 * The returned object contains borrowed pointers into @ref AvrElf and
 * @ref AvrDevice-owned data.
 */
bool
avr_elf_resolve_section(
  const AvrElf *elf,
  const AvrDevice *device,
  uint16_t section_index,
  AvrResolvedSection *resolved
);


/**
 * @brief Return the number of parsed ELF symbols.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @return
 *   Number of symbols in the available symbol table, or `0` when
 *   unavailable.
 */
size_t
avr_elf_symbol_count(
  const AvrElf *elf
);


/**
 * @brief Return an ELF symbol by symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] index
 *   Zero-based symbol-table index.
 *
 * @return
 *   Pointer to the corresponding @ref AvrElfSymbol, or `NULL` when
 *   unavailable.
 */
const AvrElfSymbol *
avr_elf_symbol(
  const AvrElf *elf,
  size_t index
);


/**
 * @brief Return an ELF symbol by symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] index
 *   Zero-based symbol-table index.
 *
 * @return
 *   Pointer to the corresponding @ref AvrElfSymbol, or `NULL` when
 *   unavailable.
 *
 * @details
 * This is the explicit indexed accessor intended for frontend and
 * iterator-style consumers.
 */
const AvrElfSymbol *
avr_elf_symbol_at(
  const AvrElf *elf,
  size_t index
);


/**
 * @brief Determine whether an ELF symbol is useful for normal presentation.
 *
 * @param[in] symbol
 *   Raw ELF symbol to classify.
 *
 * @return
 *   `true` when the symbol belongs to a currently supported useful type;
 *   `false` otherwise.
 *
 * @details
 * The current presentation policy includes:
 *
 * - `STT_OBJECT`;
 * - `STT_FUNC`;
 * - `STT_NOTYPE`.
 */
bool
avr_elf_symbol_is_useful(
  const AvrElfSymbol *symbol
);


/**
 * @brief Resolve an ELF symbol by name.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] device
 *   Device used to interpret the symbol address.
 *
 * @param[in] name
 *   NUL-terminated symbol name.
 *
 * @param[out] result
 *   Receives the resolved symbol.
 *
 * @return
 *   `true` when the symbol is found and resolved; `false` otherwise.
 *
 * @details
 * Local and global symbols are both considered.
 *
 * The resulting object can contain:
 *
 * - memory-space classification;
 * - AVR/physical address;
 * - SFR association;
 * - LMA;
 * - FLASH CPU word address.
 */
bool
avr_elf_resolve_symbol(
  const AvrElf *elf,
  const AvrDevice *device,
  const char *name,
  AvrResolvedSymbol *result
);


/**
 * @brief Resolve an ELF symbol by symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] device
 *   Device used to interpret the symbol address.
 *
 * @param[in] symbol_index
 *   Symbol-table index.
 *
 * @param[out] result
 *   Receives the resolved symbol.
 *
 * @return
 *   `true` when the indexed symbol can be resolved; `false` otherwise.
 */
bool
avr_elf_resolve_symbol_index(
  const AvrElf *elf,
  const AvrDevice *device,
  size_t symbol_index,
  AvrResolvedSymbol *result
);


/**
 * @brief Resolve an ELF symbol by symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] device
 *   Device used to interpret the symbol address.
 *
 * @param[in] symbol_index
 *   Symbol-table index.
 *
 * @param[out] resolved
 *   Receives the resolved symbol.
 *
 * @return
 *   `true` when the indexed symbol can be resolved; `false` otherwise.
 *
 * @details
 * This is the newer explicit indexed API used by the frontend.
 *
 * @warning
 * There must be one canonical implementation of indexed symbol
 * resolution in `avr_elf.c`; alternate public entry points should
 * delegate rather than duplicate the resolution algorithm.
 */
bool
avr_elf_resolve_symbol_at(
  const AvrElf *elf,
  const AvrDevice *device,
  size_t symbol_index,
  AvrResolvedSymbol *resolved
);


/**
 * @brief Resolve the load-memory address of a section-relative position.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] section
 *   ELF section whose file-backed load mapping should be resolved.
 *
 * @param[in] section_offset
 *   Byte offset from the beginning of the section.
 *
 * @param[out] lma
 *   Receives the corresponding load-memory address.
 *
 * @return
 *   `true` when the requested location has a file-backed load mapping;
 *   `false` otherwise.
 *
 * @retval false
 *   The section is `SHT_NOBITS`.
 * @retval false
 *   @p section_offset lies beyond the section.
 * @retval false
 *   The section is not represented by a suitable file-backed PT_LOAD
 *   segment.
 *
 * @details
 * The LMA is derived from ELF program headers rather than guessed from
 * the section VMA.
 *
 * Conceptually:
 *
 * @code
 * LMA =
 *   PT_LOAD.p_paddr
 *   + (section.sh_offset - PT_LOAD.p_offset)
 *   + section_offset;
 * @endcode
 */
bool
avr_elf_section_lma(
  const AvrElf *elf,
  const AvrElfSection *section,
  uint32_t section_offset,
  uint32_t *lma
);


/**
 * @brief Resolve the load-memory address of an ELF symbol.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @param[in] symbol
 *   Raw ELF symbol whose LMA should be resolved.
 *
 * @param[out] lma
 *   Receives the symbol's load-memory address.
 *
 * @return
 *   `true` when the symbol has a file-backed load mapping;
 *   `false` otherwise.
 *
 * @details
 * The implementation derives the symbol's section-relative offset and
 * delegates the actual load mapping to @ref avr_elf_section_lma.
 *
 * Absolute, undefined, and common symbols do not have an ordinary
 * section-relative LMA.
 */
bool
avr_elf_symbol_lma(
  const AvrElf *elf,
  const AvrElfSymbol *symbol,
  uint32_t *lma
);


/**
 * @brief Return the ELF machine identifier.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @return
 *   Raw ELF `e_machine` value, or `0` when @p elf is `NULL`.
 *
 * @details
 * This accessor is intentionally separate from address resolution so that
 * callers can inspect ELF identity without depending on internal ELF
 * header structures.
 *
 * This function returns the same value as @ref avr_elf_machine.
 */
uint16_t
avr_elf_get_machine(
  const AvrElf *elf
);


/**
 * @brief Return the AVR device name embedded or otherwise discoverable
 * in the ELF.
 *
 * @param[in] elf
 *   Parsed ELF object.
 *
 * @return
 *   Pointer to the discovered AVR device name, or `NULL` when the ELF
 *   does not contain usable device metadata.
 *
 * @warning
 *   The returned string is owned by @ref AvrElf and must not be freed
 *   or modified by the caller.
 *
 * @note
 *   The returned pointer remains valid until @ref avr_elf_close is
 *   called.
 *
 * @details
 * This API is primarily used by the frontend when `--device` was not
 * explicitly supplied.
 */
const char *
avr_elf_get_device_name(
  const AvrElf *elf
);


#endif /* AVR_ELF_H */
