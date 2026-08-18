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
 * @file avr_elf.c
 * @author notweerdmonk, gpt-5.6-luna
 * @brief Standalone ELF32-AVR parser and resolver.
 *
 * @details
 * This implementation does not use the host system's <elf.h>. AVR ELF
 * files are parsed explicitly as ELF32 little-endian objects, allowing
 * avrmem to be built on non-AVR hosts without an ELF library dependency.
 *
 * Responsibilities include:
 *
 * - parsing ELF32 AVR executables;
 * - exposing sections and symbols;
 * - deriving section and symbol LMAs from PT_LOAD segments;
 * - resolving ELF addresses through @ref AvrDevice;
 * - associating DATA-space symbols with discovered AVR SFRs;
 * - obtaining the AVR device name from `.note.gnu.avr.deviceinfo`.
 *
 * Device-specific address semantics remain in `avr_device.c`.
 */

#include "avr_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


/*
 * ----------------------------------------------------------------------
 * ELF constants
 * ----------------------------------------------------------------------
 */

#define ELF32_EHDR_SIZE       52
#define ELF32_SHDR_SIZE       40
#define ELF32_PHDR_SIZE       32
#define ELF32_SYM_SIZE        16

#define EI_CLASS              4
#define EI_DATA               5

#define ELFCLASS32            1
#define ELFDATA2LSB           1

#define ET_EXEC               2

#define PT_LOAD               1

#define SHT_NULL              0
#define SHT_PROGBITS          1
#define SHT_SYMTAB            2
#define SHT_STRTAB            3
#define SHT_NOTE              7
#define SHT_NOBITS            8

#define SHF_WRITE             0x1
#define SHF_ALLOC             0x2
#define SHF_EXECINSTR         0x4

#define STT_NOTYPE             0
#define STT_OBJECT             1
#define STT_FUNC               2
#define STT_SECTION            3
#define STT_FILE               4

#define AVR_NOTE_TYPE_DEVICEINFO 1


/*
 * ----------------------------------------------------------------------
 * Internal ELF structures
 * ----------------------------------------------------------------------
 */

typedef struct {
  uint32_t type;
  uint32_t offset;
  uint32_t vaddr;
  uint32_t paddr;
  uint32_t filesz;
  uint32_t memsz;
  uint32_t flags;
  uint32_t align;
} AvrElfProgramHeader;


typedef struct {
  uint32_t name;
  uint32_t type;
  uint32_t flags;
  uint32_t addr;
  uint32_t offset;
  uint32_t size;
  uint32_t link;
  uint32_t info;
  uint32_t addralign;
  uint32_t entsize;
} AvrElfRawSection;


typedef struct {
  uint32_t name;
  uint32_t value;
  uint32_t size;
  uint8_t info;
  uint8_t other;
  uint16_t shndx;
} AvrElfRawSymbol;


/*
 * ----------------------------------------------------------------------
 * Private AvrElf representation
 * ----------------------------------------------------------------------
 */

struct AvrElf {
  /*
   * Complete ELF file image.
   */
  uint8_t *image;
  size_t image_size;


  /**
   * Basic ELF header information.
   */
  uint16_t type;
  uint16_t machine;
  uint32_t entry;


  /**
   * Section table.
   */
  uint32_t section_offset;
  uint16_t section_count;
  uint16_t section_entry_size;
  uint16_t section_name_index;

  AvrElfRawSection *raw_sections;
  AvrElfSection *sections;


  /*
   * Section-name string table.
   */
  const uint8_t *section_strings;
  uint32_t section_strings_size;


  /**
   * Program-header table.
   */
  uint32_t program_offset;
  uint16_t program_count;
  uint16_t program_entry_size;

  AvrElfProgramHeader *program_headers;


  /*
   * Symbol table.
   */
  AvrElfRawSymbol *raw_symbols;
  AvrElfSymbol *symbols;
  size_t symbol_count;


  /*
   * Symbol-name string table.
   */
  const uint8_t *symbol_strings;
  uint32_t symbol_strings_size;


  /*
   * AVR device name obtained from .note.gnu.avr.deviceinfo.
   *
   * Owned by AvrElf.
   */
  char *device_name;
};


/*
 * ----------------------------------------------------------------------
 * Integer/range helpers
 * ----------------------------------------------------------------------
 */

/**
 * @brief Validate a 32-bit file range against an ELF image size.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
range_valid(
  size_t file_size,
  uint32_t offset,
  uint32_t size)
{
  if ((uint64_t)offset +
      (uint64_t)size >
      file_size)
  {
    return false;
  }

  return true;
}


/**
 * @brief Validate a size_t file range against an ELF image size.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
range_valid_size(
  size_t file_size,
  size_t offset,
  size_t size)
{
  if (offset > file_size)
    return false;

  if (size > file_size - offset)
    return false;

  return true;
}


/*
 * Align a note field to ELF's 4-byte alignment boundary.
 */
/**
 * @brief Align a value to the ELF note 4-byte alignment boundary.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
align4_u32(
  uint32_t value,
  uint32_t *aligned)
{
  if (aligned == NULL)
    return false;

  if (value > UINT32_MAX - 3U)
    return false;

  *aligned =
    (value + 3U) & ~3U;

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Little-endian readers
 * ----------------------------------------------------------------------
 */

/**
 * @brief Read a little-endian 16-bit integer.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static uint16_t
read_u16(
  const uint8_t *p)
{
  return (uint16_t)p[0] |
         ((uint16_t)p[1] << 8);
}


/**
 * @brief Read a little-endian 32-bit integer.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static uint32_t
read_u32(
  const uint8_t *p)
{
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}


/*
 * ----------------------------------------------------------------------
 * String-table helper
 * ----------------------------------------------------------------------
 */

/**
 * @brief Return a validated NUL-terminated string from an ELF string table.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static const char *
string_from_table(
  const uint8_t *table,
  uint32_t table_size,
  uint32_t offset)
{
  size_t remaining;

  if (table == NULL)
    return NULL;

  if (offset >= table_size)
    return NULL;

  remaining =
    table_size -
    offset;

  if (memchr(
        table + offset,
        '\0',
        remaining
      ) == NULL)
  {
    return NULL;
  }

  return (const char *)(table + offset);
}


/*
 * ----------------------------------------------------------------------
 * File loading
 * ----------------------------------------------------------------------
 */

/**
 * @brief Read an entire file into a newly allocated memory buffer.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
load_file(
  const char *filename,
  uint8_t **data,
  size_t *size)
{
  FILE *fp;
  long file_size;
  uint8_t *buffer;
  size_t bytes_read;

  if (filename == NULL ||
      data == NULL ||
      size == NULL)
  {
    return false;
  }

  *data = NULL;
  *size = 0;

  fp = fopen(filename, "rb");

  if (fp == NULL)
    return false;

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }

  file_size =
    ftell(fp);

  if (file_size < 0) {
    fclose(fp);
    return false;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return false;
  }

  /*
   * malloc(0) is implementation-defined, and there is no useful
   * zero-length ELF file anyway.
   */
  if (file_size == 0) {
    fclose(fp);
    return false;
  }

  buffer =
    malloc((size_t)file_size);

  if (buffer == NULL) {
    fclose(fp);
    return false;
  }

  bytes_read =
    fread(
      buffer,
      1,
      (size_t)file_size,
      fp
    );

  fclose(fp);

  if (bytes_read != (size_t)file_size) {
    free(buffer);
    return false;
  }

  *data = buffer;
  *size = (size_t)file_size;

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Program-header parsing
 * ----------------------------------------------------------------------
 */

/**
 * @brief Parse and validate the ELF program-header table.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
parse_program_headers(
  AvrElf *elf)
{
  size_t i;

  if (elf->program_count == 0)
    return true;

  if (elf->program_entry_size <
      ELF32_PHDR_SIZE)
  {
    return false;
  }

  /*
   * Validate multiplication before performing it.
   */
  if ((size_t)elf->program_count >
      SIZE_MAX / elf->program_entry_size)
  {
    return false;
  }

  if (!range_valid_size(
        elf->image_size,
        elf->program_offset,
        (size_t)elf->program_count *
        elf->program_entry_size))
  {
    return false;
  }

  elf->program_headers =
    calloc(
      elf->program_count,
      sizeof(*elf->program_headers)
    );

  if (elf->program_headers == NULL)
    return false;

  for (i = 0;
       i < elf->program_count;
       ++i)
  {
    size_t offset =
      (size_t)elf->program_offset +
      i * elf->program_entry_size;

    const uint8_t *p =
      elf->image + offset;

    elf->program_headers[i].type =
      read_u32(p + 0);

    elf->program_headers[i].offset =
      read_u32(p + 4);

    elf->program_headers[i].vaddr =
      read_u32(p + 8);

    elf->program_headers[i].paddr =
      read_u32(p + 12);

    elf->program_headers[i].filesz =
      read_u32(p + 16);

    elf->program_headers[i].memsz =
      read_u32(p + 20);

    elf->program_headers[i].flags =
      read_u32(p + 24);

    elf->program_headers[i].align =
      read_u32(p + 28);

    if (!range_valid(
          elf->image_size,
          elf->program_headers[i].offset,
          elf->program_headers[i].filesz
        ))
    {
      return false;
    }
  }

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Section-header parsing
 * ----------------------------------------------------------------------
 */

/**
 * @brief Parse and validate the ELF section-header table and section names.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
parse_sections(
  AvrElf *elf)
{
  size_t i;

  if (elf->section_count == 0)
    return false;

  if (elf->section_entry_size <
      ELF32_SHDR_SIZE)
  {
    return false;
  }

  if ((size_t)elf->section_count >
      SIZE_MAX / elf->section_entry_size)
  {
    return false;
  }

  if (!range_valid_size(
        elf->image_size,
        elf->section_offset,
        (size_t)elf->section_count *
        elf->section_entry_size))
  {
    return false;
  }

  elf->raw_sections =
    calloc(
      elf->section_count,
      sizeof(*elf->raw_sections)
    );

  elf->sections =
    calloc(
      elf->section_count,
      sizeof(*elf->sections)
    );

  if (elf->raw_sections == NULL ||
      elf->sections == NULL)
  {
    return false;
  }

  for (i = 0;
       i < elf->section_count;
       ++i)
  {
    size_t offset =
      (size_t)elf->section_offset +
      i * elf->section_entry_size;

    const uint8_t *p =
      elf->image + offset;

    elf->raw_sections[i].name =
      read_u32(p + 0);

    elf->raw_sections[i].type =
      read_u32(p + 4);

    elf->raw_sections[i].flags =
      read_u32(p + 8);

    elf->raw_sections[i].addr =
      read_u32(p + 12);

    elf->raw_sections[i].offset =
      read_u32(p + 16);

    elf->raw_sections[i].size =
      read_u32(p + 20);

    elf->raw_sections[i].link =
      read_u32(p + 24);

    elf->raw_sections[i].info =
      read_u32(p + 28);

    elf->raw_sections[i].addralign =
      read_u32(p + 32);

    elf->raw_sections[i].entsize =
      read_u32(p + 36);

    /*
     * SHT_NOBITS occupies no bytes in the file.
     */
    if (elf->raw_sections[i].type !=
        SHT_NOBITS)
    {
      if (!range_valid(
            elf->image_size,
            elf->raw_sections[i].offset,
            elf->raw_sections[i].size
          ))
      {
        return false;
      }
    }
  }

  /*
   * Validate and load the section-name string table.
   */
  if (elf->section_name_index >=
      elf->section_count)
  {
    return false;
  }

  {
    AvrElfRawSection *sh =
      &elf->raw_sections[
        elf->section_name_index
      ];

    if (sh->type != SHT_STRTAB)
      return false;

    if (!range_valid(
          elf->image_size,
          sh->offset,
          sh->size
        ))
    {
      return false;
    }

    elf->section_strings =
      elf->image + sh->offset;

    elf->section_strings_size =
      sh->size;
  }

  /*
   * Populate public section representations.
   */
  for (i = 0;
       i < elf->section_count;
       ++i)
  {
    AvrElfRawSection *raw =
      &elf->raw_sections[i];

    AvrElfSection *section =
      &elf->sections[i];

    section->index =
      (uint32_t)i;

    section->name =
      string_from_table(
        elf->section_strings,
        elf->section_strings_size,
        raw->name
      );

    if (section->name == NULL)
      return false;

    section->type =
      raw->type;

    section->addr =
      raw->addr;

    section->offset =
      raw->offset;

    section->size =
      raw->size;

    section->flags =
      raw->flags;

    section->link =
      raw->link;

    section->info =
      raw->info;

    section->alignment =
      raw->addralign;

    section->entry_size =
      raw->entsize;

    section->alloc =
      (raw->flags & SHF_ALLOC) != 0;

    section->writable =
      (raw->flags & SHF_WRITE) != 0;

    section->executable =
      (raw->flags & SHF_EXECINSTR) != 0;

    section->nobits =
      (raw->type == SHT_NOBITS);
  }

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Symbol-table parsing
 * ----------------------------------------------------------------------
 */

/**
 * @brief Parse the ordinary ELF symbol table when present.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
parse_symbols(
  AvrElf *elf)
{
  size_t i;
  const AvrElfRawSection *symtab = NULL;
  const AvrElfRawSection *strtab = NULL;

  /*
   * Locate the ordinary .symtab.
   */
  for (i = 0;
       i < elf->section_count;
       ++i)
  {
    if (elf->raw_sections[i].type ==
        SHT_SYMTAB)
    {
      symtab =
        &elf->raw_sections[i];

      break;
    }
  }

  /*
   * Stripped ELF files may legitimately have no symbol table.
   */
  if (symtab == NULL)
    return true;

  if (symtab->entsize <
      ELF32_SYM_SIZE)
  {
    return false;
  }

  if (symtab->size %
      symtab->entsize != 0)
  {
    return false;
  }

  if (symtab->link >=
      elf->section_count)
  {
    return false;
  }

  strtab =
    &elf->raw_sections[
      symtab->link
    ];

  if (strtab->type != SHT_STRTAB)
    return false;

  if (!range_valid(
        elf->image_size,
        strtab->offset,
        strtab->size
      ))
  {
    return false;
  }

  elf->symbol_strings =
    elf->image + strtab->offset;

  elf->symbol_strings_size =
    strtab->size;

  elf->symbol_count =
    symtab->size /
    symtab->entsize;

  elf->raw_symbols =
    calloc(
      elf->symbol_count,
      sizeof(*elf->raw_symbols)
    );

  elf->symbols =
    calloc(
      elf->symbol_count,
      sizeof(*elf->symbols)
    );

  if (elf->symbol_count != 0 &&
      (elf->raw_symbols == NULL ||
       elf->symbols == NULL))
  {
    return false;
  }

  for (i = 0;
       i < elf->symbol_count;
       ++i)
  {
    size_t offset =
      (size_t)symtab->offset +
      i * symtab->entsize;

    const uint8_t *p =
      elf->image + offset;

    AvrElfRawSymbol *raw =
      &elf->raw_symbols[i];

    AvrElfSymbol *symbol =
      &elf->symbols[i];

    raw->name =
      read_u32(p + 0);

    raw->value =
      read_u32(p + 4);

    raw->size =
      read_u32(p + 8);

    raw->info =
      p[12];

    raw->other =
      p[13];

    raw->shndx =
      read_u16(p + 14);

    symbol->index =
      (uint32_t)i;

    symbol->name =
      string_from_table(
        elf->symbol_strings,
        elf->symbol_strings_size,
        raw->name
      );

    if (symbol->name == NULL)
      return false;

    symbol->value =
      raw->value;

    symbol->size =
      raw->size;

    symbol->info =
      raw->info;

    symbol->other =
      raw->other;

    symbol->section_index =
      raw->shndx;

    /*
     * ELF symbol st_info:
     *
     *     low nibble  = type
     *     high nibble = binding
     */
    symbol->type =
      raw->info & 0x0f;

    symbol->bind =
      raw->info >> 4;
  }

  return true;
}


/*
 * ----------------------------------------------------------------------
 * AVR device-info note parsing
 * ----------------------------------------------------------------------
 *
 * avr-libc emits .note.gnu.avr.deviceinfo.
 *
 * The ELF note uses:
 *
 *     n_namesz
 *     n_descsz
 *     n_type
 *
 * followed by:
 *
 *     name = "AVR"
 *
 * and a device-info descriptor.
 *
 * Modern avr-libc describes the descriptor as:
 *
 *     flash_start
 *     flash_size
 *     sram_start
 *     sram_size
 *     eeprom_start
 *     eeprom_size
 *     offset_table_size
 *     offset_table[]
 *     string table
 *
 * offset_table[0] gives the device-name offset into the following
 * string table.
 *
 * Older avr-libc revisions used a different descriptor ordering, so
 * parse_deviceinfo_descriptor() also recognizes the older form.
 *
 * The parser only extracts the device name because the actual physical
 * memory description is already obtained through avr_device_probe().
 */


/*
 * Verify and duplicate a NUL-terminated device name from a note
 * descriptor.
 */
/**
 * @brief Duplicate and validate an ASCII string from an ELF note descriptor.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static char *
duplicate_note_string(
  const uint8_t *data,
  size_t size,
  uint32_t offset)
{
  size_t length;
  const uint8_t *p;

  if (data == NULL ||
      offset >= size)
  {
    return NULL;
  }

  p =
    data + offset;

  length = 0;

  while (offset + length < size) {
    unsigned char c =
      p[length];

    if (c == '\0')
      break;

    /*
     * Device names are ordinary printable ASCII strings.
     */
    if (!isprint(c)) {
      return NULL;
    }

    ++length;
  }

  if (offset + length >= size ||
      length == 0)
  {
    return NULL;
  }

  {
    char *name =
      malloc(length + 1);

    if (name == NULL)
      return NULL;

    memcpy(
      name,
      p,
      length
    );

    name[length] = '\0';

    return name;
  }
}


/*
 * Recognize the modern avr-libc device-info descriptor.
 */
/*
 * ----------------------------------------------------------------------
 * AVR device-info descriptor parsing
 * ----------------------------------------------------------------------
 *
 * The descriptor layout used by the AVR ELF device-info note is:
 *
 *     +0x00  flash_start
 *     +0x04  flash_size
 *     +0x08  sram_start
 *     +0x0c  sram_size
 *     +0x10  eeprom_start
 *     +0x14  eeprom_size
 *     +0x18  offset_table_size
 *     +0x1c  device_name_offset
 *     +0x20  offset table / string table area
 *
 * The device-name offset is relative to the string table.
 *
 * IMPORTANT:
 *
 * offset_table_size includes the offset-table size field and the
 * following offset entries. Therefore the string table begins at:
 *
 *     desc_offset + 24 + offset_table_size
 *
 * where desc_offset is the beginning of the descriptor.
 */
/**
 * @brief Parse the modern AVR device-info descriptor layout.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static char *
parse_deviceinfo_modern(
  const uint8_t *desc,
  uint32_t desc_size)
{
  uint32_t offset_table_size;
  uint32_t name_offset;

  uint32_t string_table_offset;
  uint32_t string_table_size;

  if (desc == NULL ||
      desc_size < 32U)
  {
    return NULL;
  }

  /*
   * +0x18:
   *
   * Size of the offset table.
   */
  offset_table_size =
    read_u32(desc + 24U);

  /*
   * The table must contain at least:
   *
   *   4 bytes: offset-table size
   *   4 bytes: device-name offset
   */
  if (offset_table_size < 8U)
    return NULL;

  /*
   * The offset table follows the six 32-bit device-information
   * values and the offset_table_size field.
   *
   * The first device-name offset is at +0x1c.
   */
  name_offset =
    read_u32(desc + 28U);

  /*
   * The string table follows the entire offset table.
   *
   * This is the important calculation:
   *
   *     24 bytes of fixed fields
   *     +
   *     offset_table_size
   */
  if (offset_table_size >
      desc_size - 24U)
  {
    return NULL;
  }

  string_table_offset =
    24U + offset_table_size;

  if (string_table_offset >= desc_size)
    return NULL;

  string_table_size =
    desc_size - string_table_offset;

  if (name_offset >= string_table_size)
    return NULL;

  return duplicate_note_string(
    desc + string_table_offset,
    string_table_size,
    name_offset
  );
}


/*
 * Recognize the older avr-libc device-info descriptor.
 *
 * The old format begins with:
 *
 *     uint32_t device_name_length;
 *     char device_name[];
 *
 * followed by physical flash/SRAM/EEPROM sizes.
 */
/**
 * @brief Parse the legacy AVR device-info descriptor layout.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static char *
parse_deviceinfo_legacy(
  const uint8_t *desc,
  uint32_t desc_size)
{
  uint32_t name_length;
  uint32_t name_offset;

  if (desc == NULL ||
      desc_size < 16U)
  {
    return NULL;
  }

  name_length =
    read_u32(desc);

  name_offset = 4U;

  if (name_length == 0 ||
      name_length >
        desc_size - name_offset)
  {
    return NULL;
  }

  /*
   * The device name includes its terminating NUL in the original
   * assembler-generated descriptor.
   */
  if (desc[name_offset +
           name_length - 1U] != '\0')
  {
    return NULL;
  }

  /*
   * Make sure all preceding characters are printable.
   */
  for (uint32_t i = 0;
       i + 1U < name_length;
       ++i)
  {
    if (!isprint(desc[
          name_offset + i
        ]))
    {
      return NULL;
    }
  }

  {
    char *name =
      malloc(name_length);

    if (name == NULL)
      return NULL;

    memcpy(
      name,
      desc + name_offset,
      name_length
    );

    name[name_length - 1U] =
      '\0';

    return name;
  }
}


/*
 * Parse one AVR device-info note descriptor.
 */
/**
 * @brief Parse one AVR device-info descriptor, accepting modern and legacy layouts.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static char *
parse_deviceinfo_descriptor(
  const uint8_t *desc,
  uint32_t desc_size)
{
  char *name;

  /*
   * Prefer the modern format.
   */
  name =
    parse_deviceinfo_modern(
      desc,
      desc_size
    );

  if (name != NULL)
    return name;

  /*
   * Fall back to the older avr-libc layout.
   */
  return parse_deviceinfo_legacy(
    desc,
    desc_size
  );
}


/*
 * Find and extract the AVR device name from a note section.
 */
/**
 * @brief Locate and parse an AVR device-info note in an ELF note section.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static char *
parse_deviceinfo_note_section(
  const AvrElf *elf,
  const AvrElfSection *section)
{
  uint32_t cursor = 0;

  if (elf == NULL ||
      section == NULL ||
      section->type != SHT_NOTE ||
      section->size == 0)
  {
    return NULL;
  }

  /*
   * Note data are file-backed.
   */
  if (!range_valid(
        elf->image_size,
        section->offset,
        section->size
      ))
  {
    return NULL;
  }

  while (cursor <
         section->size)
  {
    uint32_t namesz;
    uint32_t descsz;
    uint32_t type;

    uint32_t name_offset;
    uint32_t desc_offset;

    uint32_t name_padding;
    uint32_t desc_padding;

    const uint8_t *note;

    /*
     * A note header is:
     *
     *     namesz
     *     descsz
     *     type
     *
     * = 12 bytes.
     */
    if (section->size - cursor < 12U)
      return NULL;

    note =
      elf->image +
      section->offset +
      cursor;

    namesz =
      read_u32(note + 0);

    descsz =
      read_u32(note + 4);

    type =
      read_u32(note + 8);

    name_offset =
      cursor + 12U;

    if (name_offset >
        section->size)
    {
      return NULL;
    }

    if (namesz >
        section->size - name_offset)
    {
      return NULL;
    }

    if (!align4_u32(
          namesz,
          &name_padding
        ))
    {
      return NULL;
    }

    desc_offset =
      name_offset +
      name_padding;

    if (desc_offset >
        section->size)
    {
      return NULL;
    }

    if (descsz >
        section->size - desc_offset)
    {
      return NULL;
    }

    if (!align4_u32(
          descsz,
          &desc_padding
        ))
    {
      return NULL;
    }

    if (desc_offset +
        desc_padding >
        section->size)
    {
      return NULL;
    }

    /*
     * We only care about the AVR device-info note:
     *
     *     type = 1
     *     name = "AVR"
     */
    if (type ==
        AVR_NOTE_TYPE_DEVICEINFO &&
        namesz >= 4U)
    {
      const uint8_t *name =
        elf->image +
        section->offset +
        name_offset;

      /*
       * The AVR note name is "AVR\0".
       */
      if (name[0] == 'A' &&
          name[1] == 'V' &&
          name[2] == 'R' &&
          name[3] == '\0')
      {
        const uint8_t *desc =
          elf->image +
          section->offset +
          desc_offset;

        char *device_name =
          parse_deviceinfo_descriptor(
            desc,
            descsz
          );

        if (device_name != NULL)
          return device_name;
      }
    }

    cursor =
      desc_offset +
      desc_padding;

    /*
     * Protect against malformed notes that do not advance.
     */
    if (cursor <= name_offset)
      return NULL;
  }

  return NULL;
}


/*
 * Locate the standard AVR device-info note section.
 */
/**
 * @brief Extract the AVR device name from the standard device-info section.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
static bool
parse_deviceinfo(
  AvrElf *elf)
{
  const AvrElfSection *section;

  if (elf == NULL)
    return false;

  section =
    avr_elf_find_section(
      elf,
      ".note.gnu.avr.deviceinfo"
    );

  if (section == NULL)
    return true;

  elf->device_name =
    parse_deviceinfo_note_section(
      elf,
      section
    );

  /**
   * Absence of a usable device-name note should not invalidate an
   * otherwise valid ELF file.
   */
  return true;
}


/*
 * ----------------------------------------------------------------------
 * ELF opening
 * ----------------------------------------------------------------------
 */

/**
 * @brief Open and completely parse an ELF32 AVR executable.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_open(
  const char *filename,
  AvrElf **out_elf)
{
  AvrElf *elf;
  uint8_t *image;
  size_t image_size;

  if (filename == NULL ||
      out_elf == NULL)
  {
    return false;
  }

  *out_elf = NULL;

  if (!load_file(
        filename,
        &image,
        &image_size
      ))
  {
    return false;
  }

  if (image_size <
      ELF32_EHDR_SIZE)
  {
    free(image);
    return false;
  }

  /*
   * ELF magic.
   */
  if (image[0] != 0x7f ||
      image[1] != 'E' ||
      image[2] != 'L' ||
      image[3] != 'F')
  {
    free(image);
    return false;
  }

  /*
   * avrmem currently supports ELF32 little-endian files.
   */
  if (image[EI_CLASS] !=
        ELFCLASS32 ||
      image[EI_DATA] !=
        ELFDATA2LSB)
  {
    free(image);
    return false;
  }

  elf =
    calloc(
      1,
      sizeof(*elf)
    );

  if (elf == NULL) {
    free(image);
    return false;
  }

  elf->image =
    image;

  elf->image_size =
    image_size;

  /*
   * ELF32 header:
   *
   *   16 e_type
   *   18 e_machine
   *   24 e_entry
   *   28 e_phoff
   *   32 e_shoff
   *   42 e_phentsize
   *   44 e_phnum
   *   46 e_shentsize
   *   48 e_shnum
   *   50 e_shstrndx
   */
  elf->type =
    read_u16(image + 16);

  elf->machine =
    read_u16(image + 18);

  elf->entry =
    read_u32(image + 24);

  elf->program_offset =
    read_u32(image + 28);

  elf->section_offset =
    read_u32(image + 32);

  elf->program_entry_size =
    read_u16(image + 42);

  elf->program_count =
    read_u16(image + 44);

  elf->section_entry_size =
    read_u16(image + 46);

  elf->section_count =
    read_u16(image + 48);

  elf->section_name_index =
    read_u16(image + 50);

  /*
   * This parser is specifically for AVR ELF.
   */
  if (elf->machine !=
      AVR_ELF_MACHINE_AVR)
  {
    avr_elf_close(elf);
    return false;
  }

  if (elf->type != ET_EXEC) {
    avr_elf_close(elf);
    return false;
  }

  /*
   * Validate the program-header table.
   */
  if (!parse_program_headers(elf)) {
    avr_elf_close(elf);
    return false;
  }

  /*
   * Validate and parse sections.
   */
  if (!parse_sections(elf)) {
    avr_elf_close(elf);
    return false;
  }

  /*
   * Parse the normal ELF symbol table if present.
   */
  if (!parse_symbols(elf)) {
    avr_elf_close(elf);
    return false;
  }

  /*
   * Extract AVR device identity metadata when available.
   *
   * Older/stripped files may not contain the note, so failure to find
   * a device name is deliberately not treated as an ELF parse error.
   */
  if (!parse_deviceinfo(elf)) {
    avr_elf_close(elf);
    return false;
  }

  *out_elf =
    elf;

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Destruction
 * ----------------------------------------------------------------------
 */

/**
 * @brief Release an AvrElf object and all owned storage.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
void
avr_elf_close(
  AvrElf *elf)
{
  if (elf == NULL)
    return;

  free(elf->device_name);

  free(elf->program_headers);
  free(elf->raw_symbols);
  free(elf->symbols);
  free(elf->raw_sections);
  free(elf->sections);
  free(elf->image);

  free(elf);
}


/*
 * ----------------------------------------------------------------------
 * Basic ELF information
 * ----------------------------------------------------------------------
 */

/**
 * @brief Return the ELF machine identifier.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
uint16_t
avr_elf_machine(
  const AvrElf *elf)
{
  if (elf == NULL)
    return 0;

  return elf->machine;
}


/**
 * @brief Return the ELF machine identifier through the public accessor.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
uint16_t
avr_elf_get_machine(
  const AvrElf *elf)
{
  return avr_elf_machine(elf);
}


/**
 * @brief Return the ELF entry point.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
uint32_t
avr_elf_entry(
  const AvrElf *elf)
{
  if (elf == NULL)
    return 0;

  return elf->entry;
}


/**
 * @brief Return the device name discovered from ELF AVR device-info metadata.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
const char *
avr_elf_get_device_name(
  const AvrElf *elf)
{
  if (elf == NULL)
    return NULL;

  return elf->device_name;
}


/*
 * ----------------------------------------------------------------------
 * Section access
 * ----------------------------------------------------------------------
 */

/**
 * @brief Return the number of parsed ELF sections.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
size_t
avr_elf_section_count(
  const AvrElf *elf)
{
  if (elf == NULL)
    return 0;

  return elf->section_count;
}


/**
 * @brief Return an ELF section by section-table index.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
const AvrElfSection *
avr_elf_section(
  const AvrElf *elf,
  size_t index)
{
  if (elf == NULL ||
      index >= elf->section_count)
  {
    return NULL;
  }

  return &elf->sections[index];
}


/**
 * @brief Return an ELF section by explicit section-table index.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
const AvrElfSection *
avr_elf_section_at(
  const AvrElf *elf,
  uint16_t index)
{
  return avr_elf_section(
    elf,
    index
  );
}


/**
 * @brief Find an ELF section by name.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
const AvrElfSection *
avr_elf_find_section(
  const AvrElf *elf,
  const char *name)
{
  size_t i;

  if (elf == NULL ||
      name == NULL)
  {
    return NULL;
  }

  for (i = 0;
       i < elf->section_count;
       ++i)
  {
    if (strcmp(
          elf->sections[i].name,
          name
        ) == 0)
    {
      return &elf->sections[i];
    }
  }

  return NULL;
}


/*
 * ----------------------------------------------------------------------
 * Symbol access
 * ----------------------------------------------------------------------
 */

/**
 * @brief Return the number of parsed ELF symbols.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
size_t
avr_elf_symbol_count(
  const AvrElf *elf)
{
  if (elf == NULL)
    return 0;

  return elf->symbol_count;
}


/**
 * @brief Return an ELF symbol by symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
const AvrElfSymbol *
avr_elf_symbol(
  const AvrElf *elf,
  size_t index)
{
  if (elf == NULL ||
      index >= elf->symbol_count)
  {
    return NULL;
  }

  return &elf->symbols[index];
}


/**
 * @brief Return an ELF symbol by explicit symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
const AvrElfSymbol *
avr_elf_symbol_at(
  const AvrElf *elf,
  size_t index)
{
  return avr_elf_symbol(
    elf,
    index
  );
}


/**
 * @brief Determine whether an ELF symbol is useful for normal presentation.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_symbol_is_useful(
  const AvrElfSymbol *symbol)
{
  if (symbol == NULL)
    return false;

  return symbol->type == STT_OBJECT ||
         symbol->type == STT_FUNC ||
         symbol->type == STT_NOTYPE;
}


/*
 * ----------------------------------------------------------------------
 * LMA calculation
 * ----------------------------------------------------------------------
 *
 * ELF section headers do not contain an LMA.
 *
 * The AVR linker places the file-backed section bytes into PT_LOAD
 * segments.  For a section:
 *
 *     section_file_offset =
 *         segment.p_offset + delta
 *
 * the corresponding load address is:
 *
 *     segment.p_paddr + delta
 *
 * Therefore:
 *
 *     LMA =
 *         segment.p_paddr
 *         + (section.sh_offset - segment.p_offset)
 *         + section_offset
 *
 * This handles the important .data case where:
 *
 *     VMA = SRAM
 *     LMA = FLASH
 */
/**
 * @brief Resolve the load-memory address of a section-relative position.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_section_lma(
  const AvrElf *elf,
  const AvrElfSection *section,
  uint32_t section_offset,
  uint32_t *lma)
{
  size_t i;

  if (elf == NULL ||
      section == NULL ||
      lma == NULL)
  {
    return false;
  }

  /*
   * NOBITS sections have no bytes in the ELF file.
   *
   * Therefore .bss and similar sections do not have a byte-image LMA.
   */
  if (section->nobits)
    return false;

  /*
   * Permit the one-past-the-end offset for linker-generated end
   * symbols, but never an offset beyond the section.
   */
  if (section_offset >
      section->size)
  {
    return false;
  }

  for (i = 0;
       i < elf->program_count;
       ++i)
  {
    const AvrElfProgramHeader *ph =
      &elf->program_headers[i];

    uint32_t delta;
    uint32_t load_base;

    if (ph->type != PT_LOAD)
      continue;

    if (ph->filesz == 0)
      continue;

    /*
     * The section's file bytes must begin within this segment's
     * file-backed region.
     */
    if (section->offset <
        ph->offset)
    {
      continue;
    }

    delta =
      section->offset -
      ph->offset;

    if (delta >
        ph->filesz)
    {
      continue;
    }

    /*
     * Verify the requested section byte is within the file-backed
     * portion of the segment.
     */
    if (section_offset >
        ph->filesz - delta)
    {
      continue;
    }

    /*
     * Calculate p_paddr + delta safely.
     */
    if (delta >
        UINT32_MAX - ph->paddr)
    {
      return false;
    }

    load_base =
      ph->paddr +
      delta;

    if (section_offset >
        UINT32_MAX - load_base)
    {
      return false;
    }

    *lma =
      load_base +
      section_offset;

    return true;
  }

  return false;
}


/**
 * @brief Resolve the load-memory address of an ELF symbol.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_symbol_lma(
  const AvrElf *elf,
  const AvrElfSymbol *symbol,
  uint32_t *lma)
{
  const AvrElfSection *section;
  uint32_t offset;

  if (elf == NULL ||
      symbol == NULL ||
      lma == NULL)
  {
    return false;
  }

  if (symbol->section_index ==
        AVR_ELF_SHN_UNDEF ||
      symbol->section_index ==
        AVR_ELF_SHN_ABS ||
      symbol->section_index ==
        AVR_ELF_SHN_COMMON)
  {
    return false;
  }

  if (symbol->section_index >=
      elf->section_count)
  {
    return false;
  }

  section =
    &elf->sections[
      symbol->section_index
    ];

  if (symbol->value <
      section->addr)
  {
    return false;
  }

  offset =
    symbol->value -
    section->addr;

  return avr_elf_section_lma(
    elf,
    section,
    offset,
    lma
  );
}


/*
 * ----------------------------------------------------------------------
 * Section resolution
 * ----------------------------------------------------------------------
 */

/**
 * @brief Resolve an ELF section against a selected AVR device.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_resolve_section(
  const AvrElf *elf,
  const AvrDevice *device,
  uint16_t section_index,
  AvrResolvedSection *resolved)
{
  const AvrElfSection *section;
  AvrMemorySpace memory_space;
  uint32_t avr_address;
  uint32_t physical_address;

  if (elf == NULL ||
      device == NULL ||
      resolved == NULL)
  {
    return false;
  }

  memset(
    resolved,
    0,
    sizeof(*resolved)
  );

  section =
    avr_elf_section_at(
      elf,
      section_index
    );

  if (section == NULL)
    return false;

  resolved->section =
    section;

  resolved->value =
    section->addr;

  resolved->size =
    section->size;

  /*
   * Ask AvrDevice to interpret the section VMA.
   */
  if (avr_device_resolve_address(
        device,
        section->addr,
        &memory_space,
        &avr_address,
        &physical_address
      ))
  {
    resolved->memory_space =
      memory_space;

    resolved->avr_address =
      avr_address;

    resolved->physical_address =
      physical_address;

    resolved->has_physical_address =
      true;
  }
  else {
    resolved->memory_space =
      avr_device_classify_address(
        device,
        section->addr
      );

    resolved->avr_address =
      section->addr;

    resolved->has_physical_address =
      false;
  }

  /*
   * Resolve the section's load address using the exact section start.
   */
  resolved->has_lma =
    avr_elf_section_lma(
      elf,
      section,
      0,
      &resolved->lma
    );

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Symbol resolution by index
 * ----------------------------------------------------------------------
 */

/**
 * @brief Resolve an ELF symbol by symbol-table index.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_resolve_symbol_at(
  const AvrElf *elf,
  const AvrDevice *device,
  size_t symbol_index,
  AvrResolvedSymbol *resolved)
{
  const AvrElfSymbol *symbol;
  uint32_t physical_address;
  uint32_t avr_address;
  AvrMemorySpace memory_space;

  if (elf == NULL ||
      device == NULL ||
      resolved == NULL)
  {
    return false;
  }

  memset(
    resolved,
    0,
    sizeof(*resolved)
  );

  symbol =
    avr_elf_symbol_at(
      elf,
      symbol_index
    );

  if (symbol == NULL)
    return false;

  resolved->name =
    symbol->name;

  resolved->value =
    symbol->value;

  resolved->size =
    symbol->size;

  resolved->section_index =
    symbol->section_index;

  resolved->symbol =
    *symbol;


  /*
   * Undefined, absolute and COMMON symbols do not have ordinary
   * section-relative device mappings.
   */
  if (symbol->section_index ==
        AVR_ELF_SHN_UNDEF ||
      symbol->section_index ==
        AVR_ELF_SHN_ABS ||
      symbol->section_index ==
        AVR_ELF_SHN_COMMON)
  {
    resolved->memory_space =
      AVR_MEM_UNKNOWN;

    resolved->avr_address =
      symbol->value;

    resolved->has_physical_address =
      false;

    return true;
  }


  /*
   * Locate the containing section.
   */
  if (symbol->section_index >=
      elf->section_count)
  {
    return false;
  }

  resolved->section =
    avr_elf_section_at(
      elf,
      symbol->section_index
    );


  /*
   * Resolve the symbol VMA through AvrDevice.
   */
  if (!avr_device_resolve_address(
        device,
        symbol->value,
        &memory_space,
        &avr_address,
        &physical_address
      ))
  {
    /*
     * Keep unresolved addresses available to the caller instead of
     * treating an unknown device mapping as a malformed ELF symbol.
     */
    resolved->memory_space =
      AVR_MEM_UNKNOWN;

    resolved->avr_address =
      symbol->value;

    resolved->has_physical_address =
      false;
  }
  else {
    resolved->memory_space =
      memory_space;

    resolved->avr_address =
      avr_address;

    resolved->physical_address =
      physical_address;

    resolved->has_physical_address =
      true;
  }


  /*
   * --------------------------------------------------------------
   * SFR association
   * --------------------------------------------------------------
   *
   * AvrDevice has already normalized _SFR_IO8() and _SFR_MEM8()
   * definitions into physical DATA-space addresses.
   *
   * Therefore a DATA-space symbol can be matched directly against
   * resolved->avr_address.
   */
  if (resolved->memory_space ==
        AVR_MEM_REGISTER ||
      resolved->memory_space ==
        AVR_MEM_IO)
  {
    if (resolved->avr_address <=
        UINT16_MAX)
    {
      resolved->sfr =
        avr_device_find_register_address(
          device,
          (uint16_t)resolved->avr_address
        );
    }
  }


  /*
   * --------------------------------------------------------------
   * LMA
   * --------------------------------------------------------------
   *
   * The symbol's section-relative offset is:
   *
   *     symbol.value - section.addr
   */
  if (resolved->section != NULL) {
    uint32_t section_offset;

    if (symbol->value <
        resolved->section->addr)
    {
      return false;
    }

    section_offset =
      symbol->value -
      resolved->section->addr;

    if (section_offset >
        resolved->section->size)
    {
      return false;
    }

    resolved->has_lma =
      avr_elf_section_lma(
        elf,
        resolved->section,
        section_offset,
        &resolved->lma
      );
  }


  /*
   * --------------------------------------------------------------
   * Flash CPU word address
   * --------------------------------------------------------------
   *
   * flash_word_address is based on the physical FLASH byte address,
   * not directly on the ELF VMA.
   */
  if (resolved->memory_space ==
        AVR_MEM_FLASH &&
      resolved->has_physical_address)
  {
    resolved->has_flash_word_address =
      avr_device_flash_byte_to_word(
        device,
        resolved->physical_address,
        &resolved->flash_word_address
      );
  }

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Compatibility indexed resolver
 * ----------------------------------------------------------------------
 *
 * Keep the older API as a thin wrapper so existing callers continue to
 * compile while all actual resolution semantics remain centralized in
 * avr_elf_resolve_symbol_at().
 */

/**
 * @brief Compatibility wrapper for indexed symbol resolution.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_resolve_symbol_index(
  const AvrElf *elf,
  const AvrDevice *device,
  size_t symbol_index,
  AvrResolvedSymbol *result)
{
  return avr_elf_resolve_symbol_at(
    elf,
    device,
    symbol_index,
    result
  );
}


/*
 * ----------------------------------------------------------------------
 * Symbol resolution by name
 * ----------------------------------------------------------------------
 */

/**
 * @brief Resolve an ELF symbol by name.
 *
 * @param[in] elf
 *   Parsed ELF object when this function operates on an ELF instance.
 *
 * @param[out] result
 *   Output object when applicable.
 *
 * @return
 *   Function-specific success/result value.
 */
bool
avr_elf_resolve_symbol(
  const AvrElf *elf,
  const AvrDevice *device,
  const char *name,
  AvrResolvedSymbol *result)
{
  size_t i;

  if (elf == NULL ||
      device == NULL ||
      name == NULL ||
      result == NULL)
  {
    return false;
  }

  /*
   * Search all symbols, including local symbols.
   */
  for (i = 0;
       i < elf->symbol_count;
       ++i)
  {
    const AvrElfSymbol *symbol =
      &elf->symbols[i];

    if (symbol->name == NULL)
      continue;

    if (strcmp(
          symbol->name,
          name
        ) != 0)
    {
      continue;
    }

    return avr_elf_resolve_symbol_at(
      elf,
      device,
      i,
      result
    );
  }

  return false;
}
