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
 * @file test_elf_negative.c
 * @brief greatest.h unit suite for ELF parser validation and LMA rules.
 *
 * @details
 * Toolchain-free: links `src/avr_elf.c` + `src/avr_device.c` +
 * `src/avr_sfr.c` + `src/avr_device_probe.c`. Build with
 * `-Iinclude -Itests/vendor -D_POSIX_C_SOURCE=200809L`.
 *
 * Blobs are crafted byte arrays written to `mkstemp` files, then opened
 * via @ref avr_elf_open. The 144-byte minimal blob is the shared base:
 * `accept_minimal_blob` validates the builder first, and most negative
 * cases are single-field mutations of a fresh clone.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "greatest.h"
#include "avr_elf.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


GREATEST_MAIN_DEFS();


/*
 * ----------------------------------------------------------------------
 * ELF layout constants (ELF32, little-endian)
 * ----------------------------------------------------------------------
 */

enum {
  OFF_EI_CLASS = 4,
  OFF_EI_DATA = 5,
  OFF_E_TYPE = 16,
  OFF_E_MACHINE = 18,
  OFF_E_PHOFF = 28,
  OFF_E_SHOFF = 32,
  OFF_E_PHENTSIZE = 42,
  OFF_E_PHNUM = 44,
  OFF_E_SHENTSIZE = 46,
  OFF_E_SHNUM = 48,
  OFF_E_SHSTRNDX = 50
};

enum {
  SH_NAME = 0,
  SH_TYPE = 4,
  SH_FLAGS = 8,
  SH_ADDR = 12,
  SH_OFFSET = 16,
  SH_SIZE = 20,
  SH_LINK = 24,
  SH_INFO = 28,
  SH_ALIGN = 32,
  SH_ENTSIZE = 36,
  SHDR_SIZE = 40
};

enum {
  PH_TYPE = 0,
  PH_OFFSET = 4,
  PH_VADDR = 8,
  PH_PADDR = 12,
  PH_FILESZ = 16,
  PH_MEMSZ = 20,
  PH_FLAGS = 24,
  PH_ALIGN = 28,
  PHDR_SIZE = 32
};

enum {
  SHT_PROGBITS = 1,
  SHT_SYMTAB = 2,
  SHT_STRTAB = 3,
  SHT_NOTE = 7,
  SHT_NOBITS = 8
};

/* Minimal blob: EHDR 52 + shstrtab 11 at 52 + pad + SHDRs at 64. */
enum {
  MIN_LEN = 144,
  MIN_SHSTR_OFF = 52,
  MIN_SHSTR_SIZE = 11,
  MIN_SHOFF = 64
};

/*
 * Symtab/LMA blob layout (6 sections, 1 PT_LOAD, 576 bytes total):
 *
 *   0x000  EHDR (52)
 *   0x034  PHDR (32)
 *   0x054  shstrtab (43)
 *   0x07F  symtab, 2 x 16 (32)
 *   0x09F  symstr (8)
 *   0x100  PT_LOAD file image (0x50)
 *   0x150  SHDR table, 6 x 40 (240)
 *
 * Sections: [0] NULL, [1] shstrtab, [2] symtab (link 3), [3] symstr,
 * [4] .lmadat PROGBITS {addr 0x1000, offset 0x110, size 0x10},
 * [5] .lmanob NOBITS {addr 0x2000, size 0x10}.
 * PT_LOAD {offset 0x100, paddr 0x200, filesz 0x50}, so section [4]
 * has LMA base 0x210 (delta 0x10 into the segment).
 */
enum {
  SYM_LEN = 0x240,
  SYM_PHDR_OFF = 52,
  SYM_SHSTR_OFF = 84,
  SYM_SHSTR_SIZE = 43,
  SYM_SYMTAB_OFF = 127,
  SYM_SYMTAB_SIZE = 32,
  SYM_SYMSTR_OFF = 159,
  SYM_SYMSTR_SIZE = 8,
  SYM_SEG_OFF = 0x100,
  SYM_SEG_PADDR = 0x200,
  SYM_SEG_FILESZ = 0x50,
  SYM_SHOFF = 0x150,
  SYM_NSECTIONS = 6,
  SYM_ENTRY1_OFF = 143,
  SYM_PROGBITS_IDX = 4,
  SYM_NOBITS_IDX = 5,
  SYM_PROGBITS_ADDR = 0x1000
};

static const char sym_shstrtab_content[] =
  "\0.shstrtab\0.symtab\0.symstr\0.lmadat\0.lmanob\0";

static const char sym_symstr_content[] =
  "\0my_sym\0";


/**
 * @brief Store a 16-bit value little-endian.
 */
static void
put_u16le(
  unsigned char *p,
  uint16_t value)
{
  p[0] =
    (unsigned char)(value & 0xFFU);
  p[1] =
    (unsigned char)((value >> 8) & 0xFFU);
}


/**
 * @brief Store a 32-bit value little-endian.
 */
static void
put_u32le(
  unsigned char *p,
  uint32_t value)
{
  p[0] =
    (unsigned char)(value & 0xFFU);
  p[1] =
    (unsigned char)((value >> 8) & 0xFFU);
  p[2] =
    (unsigned char)((value >> 16) & 0xFFU);
  p[3] =
    (unsigned char)((value >> 24) & 0xFFU);
}


/**
 * @brief Patch a 16-bit little-endian field inside a blob.
 */
static void
patch_u16(
  unsigned char *blob,
  size_t offset,
  uint16_t value)
{
  put_u16le(blob + offset, value);
}


/**
 * @brief Patch a 32-bit little-endian field inside a blob.
 */
static void
patch_u32(
  unsigned char *blob,
  size_t offset,
  uint32_t value)
{
  put_u32le(blob + offset, value);
}


/**
 * @brief Write a whole buffer to an fd, retrying short writes.
 */
static bool
write_all(
  int fd,
  const unsigned char *data,
  size_t len)
{
  size_t written = 0;

  while (written < len) {
    ssize_t n =
      write(fd, data + written, len - written);

    if (n <= 0)
      return false;

    written +=
      (size_t)n;
  }

  return true;
}


/**
 * @brief Write a blob to a temp file and open it with the ELF parser.
 *
 * @details
 * fd-free pattern: write, close, open by name, unlink immediately.
 * Returns NULL when writing or parsing fails.
 */
static AvrElf *
open_blob(
  const unsigned char *data,
  size_t len)
{
  char path[] =
    "/tmp/avrmem_test_XXXXXX";
  int fd;
  AvrElf *elf = NULL;

  fd =
    mkstemp(path);

  if (fd < 0)
    return NULL;

  if (!write_all(fd, data, len)) {
    close(fd);
    unlink(path);
    return NULL;
  }

  close(fd);

  if (!avr_elf_open(path, &elf))
    elf = NULL;

  unlink(path);

  return elf;
}


/**
 * @brief Open a 0-byte temp file (mkstemp, no writes).
 */
static AvrElf *
open_empty(void)
{
  char path[] =
    "/tmp/avrmem_test_XXXXXX";
  int fd;
  AvrElf *elf = NULL;

  fd =
    mkstemp(path);

  if (fd < 0)
    return NULL;

  close(fd);

  if (!avr_elf_open(path, &elf))
    elf = NULL;

  unlink(path);

  return elf;
}


/**
 * @brief Release an ELF object opened by open_blob()/open_empty().
 */
static void
close_blob(
  AvrElf *elf)
{
  avr_elf_close(elf);
}


/**
 * @brief Build the 144-byte minimal accept blob (caller frees).
 *
 * @details
 * Byte-exact per spec: 52B EHDR (magic, class 1, data 1, type 2,
 * machine 83, e_version 1, entry 0, phoff 0, shoff 64, flags 0,
 * ehsize 52, phentsize 0, phnum 0, shentsize 40, shnum 2,
 * shstrndx 1) + 11B shstrtab at 52 + pad + SHDR[0] zeros at 64 +
 * SHDR[1] at 104 {name 1, type STRTAB, offset 52, size 11, rest 0}.
 */
static unsigned char *
build_minimal(
  size_t *out_len)
{
  unsigned char *blob;
  static const unsigned char shstrtab[MIN_SHSTR_SIZE] = {
    0x00, '.', 's', 'h', 's', 't', 'r', 't', 'a', 'b', 0x00
  };

  if (out_len == NULL)
    return NULL;

  blob =
    malloc(MIN_LEN);

  if (blob == NULL)
    return NULL;

  memset(blob, 0, MIN_LEN);

  blob[0] = 0x7F;
  blob[1] = (unsigned char)'E';
  blob[2] = (unsigned char)'L';
  blob[3] = (unsigned char)'F';
  blob[OFF_EI_CLASS] = 1;
  blob[OFF_EI_DATA] = 1;

  patch_u16(blob, OFF_E_TYPE, 2);
  patch_u16(blob, OFF_E_MACHINE, 83);
  patch_u32(blob, 20, 1);
  patch_u32(blob, 24, 0);
  patch_u32(blob, OFF_E_PHOFF, 0);
  patch_u32(blob, OFF_E_SHOFF, MIN_SHOFF);
  patch_u32(blob, 36, 0);
  patch_u16(blob, 40, 52);
  patch_u16(blob, OFF_E_PHENTSIZE, 0);
  patch_u16(blob, OFF_E_PHNUM, 0);
  patch_u16(blob, OFF_E_SHENTSIZE, SHDR_SIZE);
  patch_u16(blob, OFF_E_SHNUM, 2);
  patch_u16(blob, OFF_E_SHSTRNDX, 1);

  memcpy(blob + MIN_SHSTR_OFF, shstrtab, MIN_SHSTR_SIZE);

  patch_u32(blob, MIN_SHOFF + SHDR_SIZE + SH_NAME, 1);
  patch_u32(blob, MIN_SHOFF + SHDR_SIZE + SH_TYPE, SHT_STRTAB);
  patch_u32(blob, MIN_SHOFF + SHDR_SIZE + SH_OFFSET, MIN_SHSTR_OFF);
  patch_u32(blob, MIN_SHOFF + SHDR_SIZE + SH_SIZE, MIN_SHSTR_SIZE);

  *out_len = MIN_LEN;

  return blob;
}


/**
 * @brief Build the symtab/LMA blob described in the layout comment.
 *
 * @details
 * Caller frees. Symbol 0 is NULL; symbol 1 is
 * `{name "my_sym", value 0x1000, size 4, GLOBAL|OBJECT, shndx 4}`
 * pointing at the start of the PROGBITS section, so its LMA is 0x210.
 */
static unsigned char *
build_symtab_blob(
  size_t *out_len)
{
  unsigned char *blob;
  size_t sh;

  if (out_len == NULL)
    return NULL;

  blob =
    malloc(SYM_LEN);

  if (blob == NULL)
    return NULL;

  memset(blob, 0, SYM_LEN);

  blob[0] = 0x7F;
  blob[1] = (unsigned char)'E';
  blob[2] = (unsigned char)'L';
  blob[3] = (unsigned char)'F';
  blob[OFF_EI_CLASS] = 1;
  blob[OFF_EI_DATA] = 1;

  patch_u16(blob, OFF_E_TYPE, 2);
  patch_u16(blob, OFF_E_MACHINE, 83);
  patch_u32(blob, 20, 1);
  patch_u32(blob, 24, 0);
  patch_u32(blob, OFF_E_PHOFF, SYM_PHDR_OFF);
  patch_u32(blob, OFF_E_SHOFF, SYM_SHOFF);
  patch_u32(blob, 36, 0);
  patch_u16(blob, 40, 52);
  patch_u16(blob, OFF_E_PHENTSIZE, PHDR_SIZE);
  patch_u16(blob, OFF_E_PHNUM, 1);
  patch_u16(blob, OFF_E_SHENTSIZE, SHDR_SIZE);
  patch_u16(blob, OFF_E_SHNUM, SYM_NSECTIONS);
  patch_u16(blob, OFF_E_SHSTRNDX, 1);

  patch_u32(blob, SYM_PHDR_OFF + PH_TYPE, 1);
  patch_u32(blob, SYM_PHDR_OFF + PH_OFFSET, SYM_SEG_OFF);
  patch_u32(blob, SYM_PHDR_OFF + PH_VADDR, 0);
  patch_u32(blob, SYM_PHDR_OFF + PH_PADDR, SYM_SEG_PADDR);
  patch_u32(blob, SYM_PHDR_OFF + PH_FILESZ, SYM_SEG_FILESZ);
  patch_u32(blob, SYM_PHDR_OFF + PH_MEMSZ, SYM_SEG_FILESZ);
  patch_u32(blob, SYM_PHDR_OFF + PH_FLAGS, 5);
  patch_u32(blob, SYM_PHDR_OFF + PH_ALIGN, 4);

  memcpy(blob + SYM_SHSTR_OFF, sym_shstrtab_content, SYM_SHSTR_SIZE);
  memcpy(blob + SYM_SYMSTR_OFF, sym_symstr_content, SYM_SYMSTR_SIZE);

  /* Symbol 1: GLOBAL OBJECT "my_sym" at the PROGBITS base. */
  patch_u32(blob, SYM_ENTRY1_OFF + 0, 1);
  patch_u32(blob, SYM_ENTRY1_OFF + 4, SYM_PROGBITS_ADDR);
  patch_u32(blob, SYM_ENTRY1_OFF + 8, 4);
  blob[SYM_ENTRY1_OFF + 12] = 0x11;
  blob[SYM_ENTRY1_OFF + 13] = 0;
  patch_u16(blob, SYM_ENTRY1_OFF + 14, SYM_PROGBITS_IDX);

  /* [1] shstrtab */
  sh = SYM_SHOFF + 1 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 1);
  patch_u32(blob, sh + SH_TYPE, SHT_STRTAB);
  patch_u32(blob, sh + SH_OFFSET, SYM_SHSTR_OFF);
  patch_u32(blob, sh + SH_SIZE, SYM_SHSTR_SIZE);

  /* [2] symtab */
  sh = SYM_SHOFF + 2 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 11);
  patch_u32(blob, sh + SH_TYPE, SHT_SYMTAB);
  patch_u32(blob, sh + SH_OFFSET, SYM_SYMTAB_OFF);
  patch_u32(blob, sh + SH_SIZE, SYM_SYMTAB_SIZE);
  patch_u32(blob, sh + SH_LINK, 3);
  patch_u32(blob, sh + SH_INFO, 1);
  patch_u32(blob, sh + SH_ALIGN, 4);
  patch_u32(blob, sh + SH_ENTSIZE, 16);

  /* [3] symstr */
  sh = SYM_SHOFF + 3 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 19);
  patch_u32(blob, sh + SH_TYPE, SHT_STRTAB);
  patch_u32(blob, sh + SH_OFFSET, SYM_SYMSTR_OFF);
  patch_u32(blob, sh + SH_SIZE, SYM_SYMSTR_SIZE);

  /* [4] .lmadat PROGBITS */
  sh = SYM_SHOFF + 4 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 27);
  patch_u32(blob, sh + SH_TYPE, SHT_PROGBITS);
  patch_u32(blob, sh + SH_FLAGS, 0x3);
  patch_u32(blob, sh + SH_ADDR, SYM_PROGBITS_ADDR);
  patch_u32(blob, sh + SH_OFFSET, 0x110);
  patch_u32(blob, sh + SH_SIZE, 0x10);
  patch_u32(blob, sh + SH_ALIGN, 4);

  /* [5] .lmanob NOBITS */
  sh = SYM_SHOFF + 5 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 35);
  patch_u32(blob, sh + SH_TYPE, SHT_NOBITS);
  patch_u32(blob, sh + SH_FLAGS, 0x3);
  patch_u32(blob, sh + SH_ADDR, 0x2000);
  patch_u32(blob, sh + SH_OFFSET, 0);
  patch_u32(blob, sh + SH_SIZE, 0x10);
  patch_u32(blob, sh + SH_ALIGN, 4);

  *out_len = SYM_LEN;

  return blob;
}


/*
 * Overlap blob layout (3 sections, 2 PT_LOADs, 456 bytes total):
 *
 *   0x000  EHDR (52)
 *   0x034  PHDR[0] (32) {offset 0x100, paddr 0x200, filesz 0x50}
 *   0x054  PHDR[1] (32) {offset 0x100, paddr 0x300, filesz 0x50}
 *   0x074  shstrtab (16)
 *   0x100  PT_LOAD file image (0x50)
 *   0x150  SHDR table, 3 x 40 (120)
 *
 * Sections: [0] NULL, [1] shstrtab, [2] .ovl PROGBITS
 * {addr 0x1000, offset 0x110, size 0x10}. Both segments cover the
 * section bytes; the first match must win (linear scan from index 0
 * in avr_elf_section_lma(), src/avr_elf.c), so off 0 → 0x210.
 */
enum {
  OVL_LEN = 456,
  OVL_PHDR_OFF = 52,
  OVL_SHSTR_OFF = 116,
  OVL_SHSTR_SIZE = 16,
  OVL_SEG_OFF = 0x100,
  OVL_SEG_FILESZ = 0x50,
  OVL_SHOFF = 0x150,
  OVL_PROGBITS_IDX = 2,
  OVL_PROGBITS_ADDR = 0x1000
};

static const char ovl_shstrtab_content[] =
  "\0.shstrtab\0.ovl\0";


/**
 * @brief Build the two-PT_LOAD overlap blob (caller frees).
 */
static unsigned char *
build_overlap_blob(
  size_t *out_len)
{
  unsigned char *blob;
  size_t sh;

  if (out_len == NULL)
    return NULL;

  blob =
    malloc(OVL_LEN);

  if (blob == NULL)
    return NULL;

  memset(blob, 0, OVL_LEN);

  blob[0] = 0x7F;
  blob[1] = (unsigned char)'E';
  blob[2] = (unsigned char)'L';
  blob[3] = (unsigned char)'F';
  blob[OFF_EI_CLASS] = 1;
  blob[OFF_EI_DATA] = 1;

  patch_u16(blob, OFF_E_TYPE, 2);
  patch_u16(blob, OFF_E_MACHINE, 83);
  patch_u32(blob, 20, 1);
  patch_u32(blob, 24, 0);
  patch_u32(blob, OFF_E_PHOFF, OVL_PHDR_OFF);
  patch_u32(blob, OFF_E_SHOFF, OVL_SHOFF);
  patch_u32(blob, 36, 0);
  patch_u16(blob, 40, 52);
  patch_u16(blob, OFF_E_PHENTSIZE, PHDR_SIZE);
  patch_u16(blob, OFF_E_PHNUM, 2);
  patch_u16(blob, OFF_E_SHENTSIZE, SHDR_SIZE);
  patch_u16(blob, OFF_E_SHNUM, 3);
  patch_u16(blob, OFF_E_SHSTRNDX, 1);

  patch_u32(blob, OVL_PHDR_OFF + PH_TYPE, 1);
  patch_u32(blob, OVL_PHDR_OFF + PH_OFFSET, OVL_SEG_OFF);
  patch_u32(blob, OVL_PHDR_OFF + PH_VADDR, 0);
  patch_u32(blob, OVL_PHDR_OFF + PH_PADDR, 0x200);
  patch_u32(blob, OVL_PHDR_OFF + PH_FILESZ, OVL_SEG_FILESZ);
  patch_u32(blob, OVL_PHDR_OFF + PH_MEMSZ, OVL_SEG_FILESZ);
  patch_u32(blob, OVL_PHDR_OFF + PH_FLAGS, 5);
  patch_u32(blob, OVL_PHDR_OFF + PH_ALIGN, 4);

  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_TYPE, 1);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_OFFSET, OVL_SEG_OFF);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_VADDR, 0);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_PADDR, 0x300);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_FILESZ, OVL_SEG_FILESZ);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_MEMSZ, OVL_SEG_FILESZ);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_FLAGS, 5);
  patch_u32(blob, OVL_PHDR_OFF + PHDR_SIZE + PH_ALIGN, 4);

  memcpy(blob + OVL_SHSTR_OFF, ovl_shstrtab_content, OVL_SHSTR_SIZE);

  /* [1] shstrtab */
  sh = OVL_SHOFF + 1 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 1);
  patch_u32(blob, sh + SH_TYPE, SHT_STRTAB);
  patch_u32(blob, sh + SH_OFFSET, OVL_SHSTR_OFF);
  patch_u32(blob, sh + SH_SIZE, OVL_SHSTR_SIZE);

  /* [2] .ovl PROGBITS */
  sh = OVL_SHOFF + 2 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 11);
  patch_u32(blob, sh + SH_TYPE, SHT_PROGBITS);
  patch_u32(blob, sh + SH_FLAGS, 0x3);
  patch_u32(blob, sh + SH_ADDR, OVL_PROGBITS_ADDR);
  patch_u32(blob, sh + SH_OFFSET, 0x110);
  patch_u32(blob, sh + SH_SIZE, 0x10);
  patch_u32(blob, sh + SH_ALIGN, 4);

  *out_len = OVL_LEN;

  return blob;
}


/**
 * @brief Build a 3-section blob with one extra section (caller frees).
 *
 * @details
 * Layout: EHDR + shstrtab (`"\0.shstrtab\0<name>\0"`) + note payload +
 * SHDR[0..2]. The extra section takes `section_type`/`note_len`; its
 * name offset is always 11.
 */
static unsigned char *
build_note_blob(
  uint32_t section_type,
  const unsigned char *note,
  size_t note_len,
  const char *section_name,
  size_t *out_len)
{
  unsigned char *blob;
  size_t name_len;
  size_t shstr_len;
  size_t note_off;
  size_t shoff;
  size_t total;
  size_t sh;

  if (section_name == NULL ||
      out_len == NULL)
    return NULL;

  if (note_len != 0 &&
      note == NULL)
    return NULL;

  name_len =
    strlen(section_name) + 1;
  shstr_len =
    11 + name_len;
  note_off =
    52 + shstr_len;
  shoff =
    note_off + note_len;
  total =
    shoff + 3 * SHDR_SIZE;

  blob =
    malloc(total);

  if (blob == NULL)
    return NULL;

  memset(blob, 0, total);

  blob[0] = 0x7F;
  blob[1] = (unsigned char)'E';
  blob[2] = (unsigned char)'L';
  blob[3] = (unsigned char)'F';
  blob[OFF_EI_CLASS] = 1;
  blob[OFF_EI_DATA] = 1;

  patch_u16(blob, OFF_E_TYPE, 2);
  patch_u16(blob, OFF_E_MACHINE, 83);
  patch_u32(blob, 20, 1);
  patch_u32(blob, 24, 0);
  patch_u32(blob, OFF_E_PHOFF, 0);
  patch_u32(blob, OFF_E_SHOFF, (uint32_t)shoff);
  patch_u32(blob, 36, 0);
  patch_u16(blob, 40, 52);
  patch_u16(blob, OFF_E_PHENTSIZE, 0);
  patch_u16(blob, OFF_E_PHNUM, 0);
  patch_u16(blob, OFF_E_SHENTSIZE, SHDR_SIZE);
  patch_u16(blob, OFF_E_SHNUM, 3);
  patch_u16(blob, OFF_E_SHSTRNDX, 1);

  blob[52 + 1] = (unsigned char)'.';
  memcpy(blob + 52 + 1, ".shstrtab", 9);
  memcpy(blob + 52 + 11, section_name, name_len);

  if (note_len != 0)
    memcpy(blob + note_off, note, note_len);

  /* [1] shstrtab */
  sh = shoff + 1 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 1);
  patch_u32(blob, sh + SH_TYPE, SHT_STRTAB);
  patch_u32(blob, sh + SH_OFFSET, 52);
  patch_u32(blob, sh + SH_SIZE, (uint32_t)shstr_len);

  /* [2] extra section */
  sh = shoff + 2 * SHDR_SIZE;
  patch_u32(blob, sh + SH_NAME, 11);
  patch_u32(blob, sh + SH_TYPE, section_type);
  patch_u32(blob, sh + SH_OFFSET, (uint32_t)note_off);
  patch_u32(blob, sh + SH_SIZE, (uint32_t)note_len);

  *out_len = total;

  return blob;
}


/**
 * @brief Build a minimal modern device-info note (caller frees).
 *
 * @details
 * namesz 4 (`"AVR\0"`), type 1, 43-byte descriptor: 24 zero bytes,
 * table size 8, name offset 0, `"atmega328p\0"`. Total 60 bytes
 * with descriptor padding.
 */
static unsigned char *
build_modern_note(
  size_t *out_len)
{
  unsigned char *note;
  static const char name[] =
    "atmega328p";

  if (out_len == NULL)
    return NULL;

  note =
    malloc(60);

  if (note == NULL)
    return NULL;

  memset(note, 0, 60);

  put_u32le(note + 0, 4);
  put_u32le(note + 4, 43);
  put_u32le(note + 8, 1);
  note[12] = (unsigned char)'A';
  note[13] = (unsigned char)'V';
  note[14] = (unsigned char)'R';
  note[15] = 0;

  put_u32le(note + 16 + 24, 8);
  put_u32le(note + 16 + 28, 0);
  memcpy(note + 16 + 32, name, sizeof(name));

  *out_len = 60;

  return note;
}


/**
 * @brief Build a legacy device-info note (caller frees).
 *
 * @details
 * namesz 4 (`"AVR\0"`), type 1, 16-byte descriptor: length 11,
 * `"atmega328p\0"`, one pad byte. Total 32 bytes.
 */
static unsigned char *
build_legacy_note(
  size_t *out_len)
{
  unsigned char *note;
  static const char name[] =
    "atmega328p";

  if (out_len == NULL)
    return NULL;

  note =
    malloc(32);

  if (note == NULL)
    return NULL;

  memset(note, 0, 32);

  put_u32le(note + 0, 4);
  put_u32le(note + 4, 16);
  put_u32le(note + 8, 1);
  note[12] = (unsigned char)'A';
  note[13] = (unsigned char)'V';
  note[14] = (unsigned char)'R';
  note[15] = 0;

  put_u32le(note + 16, 11);
  memcpy(note + 20, name, sizeof(name));

  *out_len = 32;

  return note;
}


/**
 * @brief Hand-build a device by value (test-chosen filler + spec recipe).
 *
 * @details
 * `text_space`/`flash` {0, 0x10000}, `data_space` {0x800000, 0x10000},
 * `sram` {0x100, 0x800}, everything else zero, shift 1. The fixture
 * owns nothing, so `avr_device_destroy` is never called on it.
 */
static AvrDevice
make_test_device(void)
{
  AvrDevice dev;

  memset(&dev, 0, sizeof(dev));

  dev.text_space.origin = 0x00000000;
  dev.text_space.length = 0x10000;
  dev.flash.start = 0x0000;
  dev.flash.size = 0x10000;
  dev.data_space.origin = 0x00800000;
  dev.data_space.length = 0x10000;
  dev.sram.start = 0x0100;
  dev.sram.size = 0x0800;
  dev.flash_address_shift = 1;

  return dev;
}


TEST
accept_minimal_blob(void)
{
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf = NULL;

  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);
  ASSERT_EQ((size_t)MIN_LEN, len);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT_EQ((size_t)2, avr_elf_section_count(elf));
  ASSERT_EQ((size_t)0, avr_elf_symbol_count(elf));
  ASSERT(avr_elf_get_device_name(elf) == NULL);
  ASSERT_EQ((uint16_t)83, avr_elf_machine(elf));
  ASSERT_EQ((uint16_t)83, avr_elf_get_machine(elf));
  ASSERT_EQ((uint32_t)0, avr_elf_entry(elf));

  close_blob(elf);
  free(blob);

  PASS();
}


TEST
reject_non_elf(void)
{
  unsigned char mz[52];
  unsigned char script[52];
  unsigned char nul_elf[52];

  ASSERT(open_empty() == NULL);

  {
    static unsigned char zeros[51] = { 0 };

    ASSERT(open_blob(zeros, sizeof(zeros)) == NULL);
  }

  memset(mz, 0, sizeof(mz));
  mz[0] = (unsigned char)'M';
  mz[1] = (unsigned char)'Z';
  ASSERT(open_blob(mz, sizeof(mz)) == NULL);

  memset(script, 0, sizeof(script));
  memcpy(script, "#!/bin/sh", 8);
  ASSERT(open_blob(script, sizeof(script)) == NULL);

  memset(nul_elf, 0, sizeof(nul_elf));
  nul_elf[1] = (unsigned char)'E';
  nul_elf[2] = (unsigned char)'L';
  nul_elf[3] = (unsigned char)'F';
  ASSERT(open_blob(nul_elf, sizeof(nul_elf)) == NULL);

  PASS();
}


TEST
reject_class_encoding(void)
{
  size_t len = 0;
  unsigned char *blob;

  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);

  blob[OFF_EI_CLASS] = 2;
  ASSERT(open_blob(blob, len) == NULL);
  blob[OFF_EI_CLASS] = 1;

  blob[OFF_EI_DATA] = 2;
  ASSERT(open_blob(blob, len) == NULL);
  blob[OFF_EI_DATA] = 1;

  blob[OFF_EI_CLASS] = 0;
  blob[OFF_EI_DATA] = 0;
  ASSERT(open_blob(blob, len) == NULL);

  free(blob);

  PASS();
}


TEST
reject_machine_type(void)
{
  static const uint16_t bad_machines[] = {
    0, 3, 40, 62, 0xFFFF
  };
  static const uint16_t bad_types[] = {
    0, 1, 3, 4
  };
  size_t len = 0;
  unsigned char *blob;
  size_t i;

  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);

  for (i = 0;
       i < sizeof(bad_machines) / sizeof(bad_machines[0]);
       ++i)
  {
    patch_u16(blob, OFF_E_MACHINE, bad_machines[i]);
    ASSERT(open_blob(blob, len) == NULL);
  }

  patch_u16(blob, OFF_E_MACHINE, 83);

  for (i = 0;
       i < sizeof(bad_types) / sizeof(bad_types[0]);
       ++i)
  {
    patch_u16(blob, OFF_E_TYPE, bad_types[i]);
    ASSERT(open_blob(blob, len) == NULL);
  }

  free(blob);

  PASS();
}


TEST
reject_tables(void)
{
  size_t len = 0;
  unsigned char *base;
  unsigned char *m;

  base =
    build_minimal(&len);

  ASSERT(base != NULL);

  m =
    malloc(len);

  ASSERT(m != NULL);

  memcpy(m, base, len);
  patch_u16(m, OFF_E_SHNUM, 0);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u32(m, OFF_E_SHOFF, 0x1000);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u16(m, OFF_E_SHNUM, 0xFFFF);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u16(m, OFF_E_SHENTSIZE, 0);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u16(m, OFF_E_SHENTSIZE, 39);
  ASSERT(open_blob(m, len) == NULL);

  /* PROGBITS shdr[1] with an overrunning size. */
  memcpy(m, base, len);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_TYPE, SHT_PROGBITS);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_SIZE, 0xFF);
  ASSERT(open_blob(m, len) == NULL);

  /*
   * Same bytes as NOBITS skip the file-range check, but shdr[1] is
   * still the shstrtab (shstrndx 1), and a NOBITS section is not a
   * STRTAB, so the parser must reject it too. The task text claims
   * this opens; the code path in parse_sections() says otherwise.
   */
  memcpy(m, base, len);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_TYPE, SHT_NOBITS);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_SIZE, 0xFF);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u16(m, OFF_E_SHSTRNDX, 2);
  ASSERT(open_blob(m, len) == NULL);

  /* shdr[1] is not a STRTAB (size still fits, name table check fails). */
  memcpy(m, base, len);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_TYPE, SHT_PROGBITS);
  ASSERT(open_blob(m, len) == NULL);

  /* STRTAB content overruns the file. */
  memcpy(m, base, len);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_SIZE, 0xFF);
  ASSERT(open_blob(m, len) == NULL);

  /* Section name outside the string table. */
  memcpy(m, base, len);
  patch_u32(m, MIN_SHOFF + SHDR_SIZE + SH_NAME, 0xFF);
  ASSERT(open_blob(m, len) == NULL);

  /* Section name without a terminating NUL in the table. */
  memcpy(m, base, len);
  m[MIN_SHSTR_OFF + MIN_SHSTR_SIZE - 1] = (unsigned char)'X';
  ASSERT(open_blob(m, len) == NULL);

  free(m);
  free(base);

  PASS();
}


TEST
reject_symtab(void)
{
  size_t len = 0;
  unsigned char *base;
  unsigned char *m;
  size_t symtab_sh;
  AvrElf *elf;

  base =
    build_symtab_blob(&len);

  ASSERT(base != NULL);

  /* The unmutated symtab blob must parse: 2 symbols. */
  elf =
    open_blob(base, len);

  ASSERT(elf != NULL);
  ASSERT_EQ((size_t)2, avr_elf_symbol_count(elf));
  close_blob(elf);

  m =
    malloc(len);

  ASSERT(m != NULL);

  symtab_sh =
    SYM_SHOFF + 2 * SHDR_SIZE;

  memcpy(m, base, len);
  patch_u32(m, symtab_sh + SH_ENTSIZE, 0);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u32(m, symtab_sh + SH_ENTSIZE, 8);
  ASSERT(open_blob(m, len) == NULL);

  memcpy(m, base, len);
  patch_u32(m, symtab_sh + SH_ENTSIZE, 15);
  ASSERT(open_blob(m, len) == NULL);

  /* Size is not a multiple of the entry size. */
  memcpy(m, base, len);
  patch_u32(m, symtab_sh + SH_SIZE, 17);
  ASSERT(open_blob(m, len) == NULL);

  /* Link outside the section table. */
  memcpy(m, base, len);
  patch_u32(m, symtab_sh + SH_LINK, SYM_NSECTIONS);
  ASSERT(open_blob(m, len) == NULL);

  /* Link points at a PROGBITS section, not a STRTAB. */
  memcpy(m, base, len);
  patch_u32(m, symtab_sh + SH_LINK, SYM_PROGBITS_IDX);
  ASSERT(open_blob(m, len) == NULL);

  /* Linked string table overruns the file. */
  memcpy(m, base, len);
  patch_u32(m, SYM_SHOFF + 3 * SHDR_SIZE + SH_SIZE, 0xFFFF);
  ASSERT(open_blob(m, len) == NULL);

  /* Symbol name outside the string table. */
  memcpy(m, base, len);
  patch_u32(m, SYM_ENTRY1_OFF + 0, 0xFFFFFFFFU);
  ASSERT(open_blob(m, len) == NULL);

  /* Symbol name without a terminating NUL in the table. */
  memcpy(m, base, len);
  m[SYM_SYMSTR_OFF + SYM_SYMSTR_SIZE - 1] = (unsigned char)'X';
  ASSERT(open_blob(m, len) == NULL);

  free(m);
  free(base);

  PASS();
}


TEST
stripped_sections_resolve(void)
{
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf = NULL;
  AvrDevice dev =
    make_test_device();
  AvrResolvedSection resolved;
  AvrResolvedSymbol symbol;

  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT_EQ((size_t)0, avr_elf_symbol_count(elf));

  ASSERT(avr_elf_resolve_section(elf, &dev, 1, &resolved));

  ASSERT(!avr_elf_resolve_symbol(elf, &dev, "anything", &symbol));

  close_blob(elf);
  free(blob);

  PASS();
}


TEST
nonalloc_section_unknown(void)
{
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf = NULL;
  AvrDevice dev =
    make_test_device();
  AvrResolvedSection resolved;

  /*
   * Minimal-blob section 1 is STRTAB with flags 0 (not SHF_ALLOC):
   * its VMA is not a device address, so it resolves UNKNOWN with no
   * physical address (Q5b; non-ALLOC sections must not display as
   * resident). Return value stays true — only classification changes.
   */
  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_resolve_section(elf, &dev, 1, &resolved));
  ASSERT_EQ(AVR_MEM_UNKNOWN, resolved.memory_space);
  ASSERT(!resolved.has_physical_address);

  close_blob(elf);
  free(blob);

  PASS();
}


TEST
note_garbage_still_opens(void)
{
  static const char note_name[] =
    ".note.gnu.avr.deviceinfo";
  static const unsigned char truncated[5] = {
    0, 0, 0, 0, 0
  };
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf;

  /* NOTE section with size 0: no usable descriptor. */
  blob =
    build_note_blob(SHT_NOTE, NULL, 0, note_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_get_device_name(elf) == NULL);
  close_blob(elf);
  free(blob);

  /*
   * Same name on a PROGBITS section: not a note, so no device name.
   * (The task text says "type=3 PROGBITS", which is contradictory:
   * PROGBITS is 1, STRTAB is 3. Any non-NOTE type behaves the same
   * in parse_deviceinfo_note_section(), so PROGBITS is used here.)
   */
  blob =
    build_note_blob(SHT_PROGBITS, NULL, 0, note_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_get_device_name(elf) == NULL);
  close_blob(elf);
  free(blob);

  /* Truncated 5-byte NOTE: shorter than a 12-byte note header. */
  blob =
    build_note_blob(SHT_NOTE, truncated, sizeof(truncated),
                    note_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_get_device_name(elf) == NULL);
  close_blob(elf);
  free(blob);

  PASS();
}


TEST
note_modern_and_legacy(void)
{
  static const char note_name[] =
    ".note.gnu.avr.deviceinfo";
  static const char wrong_name[] =
    ".note.foo";
  size_t len = 0;
  size_t note_len = 0;
  unsigned char *note;
  unsigned char *blob;
  AvrElf *elf;
  const char *device;

  note =
    build_modern_note(&note_len);

  ASSERT(note != NULL);

  blob =
    build_note_blob(SHT_NOTE, note, note_len, note_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);

  device =
    avr_elf_get_device_name(elf);

  ASSERT(device != NULL);
  ASSERT(strcmp(device, "atmega328p") == 0);
  close_blob(elf);
  free(blob);

  /* Same payload under the wrong section name: ignored. */
  blob =
    build_note_blob(SHT_NOTE, note, note_len, wrong_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_get_device_name(elf) == NULL);
  close_blob(elf);
  free(blob);
  free(note);

  note =
    build_legacy_note(&note_len);

  ASSERT(note != NULL);

  blob =
    build_note_blob(SHT_NOTE, note, note_len, note_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);

  device =
    avr_elf_get_device_name(elf);

  ASSERT(device != NULL);
  ASSERT(strcmp(device, "atmega328p") == 0);
  close_blob(elf);
  free(blob);
  free(note);

  PASS();
}


TEST
accessor_null_safety(void)
{
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf = NULL;
  AvrDevice dev =
    make_test_device();
  AvrResolvedSection resolved;
  AvrResolvedSymbol symbol;
  uint32_t lma = 0xDEADU;
  char path[] =
    "/tmp/avrmem_test_XXXXXX";
  int fd;
  AvrElfSymbol crafted;

  ASSERT(!avr_elf_open(NULL, &elf));

  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);

  fd =
    mkstemp(path);

  ASSERT(fd >= 0);
  ASSERT(write_all(fd, blob, len));
  close(fd);
  ASSERT(!avr_elf_open(path, NULL));
  unlink(path);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);

  avr_elf_close(NULL);

  ASSERT_EQ((uint16_t)0, avr_elf_machine(NULL));
  ASSERT_EQ((uint16_t)0, avr_elf_get_machine(NULL));
  ASSERT_EQ((uint32_t)0, avr_elf_entry(NULL));
  ASSERT_EQ((size_t)0, avr_elf_section_count(NULL));
  ASSERT_EQ((size_t)0, avr_elf_symbol_count(NULL));

  ASSERT(avr_elf_section(NULL, 0) == NULL);
  ASSERT(avr_elf_section(elf, 999) == NULL);
  ASSERT(avr_elf_section_at(NULL, 0) == NULL);
  ASSERT(avr_elf_section_at(elf, 999) == NULL);
  ASSERT(avr_elf_find_section(NULL, ".shstrtab") == NULL);
  ASSERT(avr_elf_find_section(elf, NULL) == NULL);
  ASSERT(avr_elf_find_section(elf, "no-such-section") == NULL);

  ASSERT(avr_elf_symbol(NULL, 0) == NULL);
  ASSERT(avr_elf_symbol(elf, 0) == NULL);
  ASSERT(avr_elf_symbol_at(NULL, 0) == NULL);
  ASSERT(avr_elf_symbol_at(elf, 999) == NULL);

  ASSERT(!avr_elf_symbol_is_useful(NULL));

  memset(&crafted, 0, sizeof(crafted));
  crafted.type = 0;
  ASSERT(avr_elf_symbol_is_useful(&crafted));
  crafted.type = 1;
  ASSERT(avr_elf_symbol_is_useful(&crafted));
  crafted.type = 2;
  ASSERT(avr_elf_symbol_is_useful(&crafted));
  crafted.type = 3;
  ASSERT(!avr_elf_symbol_is_useful(&crafted));
  crafted.type = 4;
  ASSERT(!avr_elf_symbol_is_useful(&crafted));

  ASSERT(avr_elf_get_device_name(NULL) == NULL);

  ASSERT(!avr_elf_resolve_section(NULL, &dev, 0, &resolved));
  ASSERT(!avr_elf_resolve_section(elf, NULL, 0, &resolved));
  ASSERT(!avr_elf_resolve_section(elf, &dev, 0, NULL));
  ASSERT(!avr_elf_resolve_section(elf, &dev, 999, &resolved));

  ASSERT(!avr_elf_resolve_symbol(NULL, &dev, "x", &symbol));
  ASSERT(!avr_elf_resolve_symbol(elf, NULL, "x", &symbol));
  ASSERT(!avr_elf_resolve_symbol(elf, &dev, NULL, &symbol));
  ASSERT(!avr_elf_resolve_symbol(elf, &dev, "x", NULL));

  ASSERT(!avr_elf_resolve_symbol_at(NULL, &dev, 0, &symbol));
  ASSERT(!avr_elf_resolve_symbol_index(elf, NULL, 0, &symbol));

  ASSERT(!avr_elf_section_lma(NULL, NULL, 0, NULL));
  ASSERT(!avr_elf_section_lma(elf, NULL, 0, &lma));
  ASSERT(!avr_elf_symbol_lma(NULL, NULL, NULL));
  ASSERT(!avr_elf_symbol_lma(elf, NULL, &lma));

  close_blob(elf);
  close_blob(NULL);
  free(blob);

  PASS();
}


TEST
lma_rules(void)
{
  size_t len = 0;
  unsigned char *base;
  unsigned char *m;
  AvrElf *elf;
  const AvrElfSection *progbits;
  const AvrElfSection *nobits;
  const AvrElfSymbol *sym;
  uint32_t lma = 0;

  base =
    build_symtab_blob(&len);

  ASSERT(base != NULL);

  m =
    malloc(len);

  ASSERT(m != NULL);

  elf =
    open_blob(base, len);

  ASSERT(elf != NULL);

  progbits =
    avr_elf_section(elf, SYM_PROGBITS_IDX);

  ASSERT(progbits != NULL);

  nobits =
    avr_elf_section(elf, SYM_NOBITS_IDX);

  ASSERT(nobits != NULL);
  ASSERT(nobits->nobits);

  ASSERT(avr_elf_section_lma(elf, progbits, 0, &lma));
  ASSERT_EQ((uint32_t)0x210, lma);

  /* One-past-the-end offset is allowed. */
  ASSERT(avr_elf_section_lma(elf, progbits, 0x10, &lma));
  ASSERT_EQ((uint32_t)0x220, lma);

  ASSERT(!avr_elf_section_lma(elf, progbits, 0x11, &lma));

  ASSERT(!avr_elf_section_lma(elf, nobits, 0, &lma));

  close_blob(elf);

  /* A segment with filesz 0 is not file-backed: no LMA. */
  memcpy(m, base, len);
  patch_u32(m, SYM_PHDR_OFF + PH_FILESZ, 0);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);

  progbits =
    avr_elf_section(elf, SYM_PROGBITS_IDX);

  ASSERT(progbits != NULL);
  ASSERT(!avr_elf_section_lma(elf, progbits, 0, &lma));
  close_blob(elf);

  /* Absolute symbols have no section-relative LMA. */
  memcpy(m, base, len);
  patch_u16(m, SYM_ENTRY1_OFF + 14, 0xFFF1);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);

  sym =
    avr_elf_symbol(elf, 1);

  ASSERT(sym != NULL);
  ASSERT(!avr_elf_symbol_lma(elf, sym, &lma));
  close_blob(elf);

  /* Undefined and COMMON symbols likewise have no LMA. */
  memcpy(m, base, len);
  patch_u16(m, SYM_ENTRY1_OFF + 14, 0);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);
  ASSERT(!avr_elf_symbol_lma(elf, avr_elf_symbol(elf, 1), &lma));
  close_blob(elf);

  memcpy(m, base, len);
  patch_u16(m, SYM_ENTRY1_OFF + 14, 0xFFF2);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);
  ASSERT(!avr_elf_symbol_lma(elf, avr_elf_symbol(elf, 1), &lma));
  close_blob(elf);

  /* Symbol value below its section base: no LMA. */
  memcpy(m, base, len);
  patch_u32(m, SYM_ENTRY1_OFF + 4, SYM_PROGBITS_ADDR - 1);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);
  ASSERT(!avr_elf_symbol_lma(elf, avr_elf_symbol(elf, 1), &lma));
  close_blob(elf);

  /* Symbol at one-past-the-end of its section: LMA is valid. */
  memcpy(m, base, len);
  patch_u32(m, SYM_ENTRY1_OFF + 4, SYM_PROGBITS_ADDR + 0x10);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_symbol_lma(elf, avr_elf_symbol(elf, 1), &lma));
  ASSERT_EQ((uint32_t)0x220, lma);
  close_blob(elf);

  free(m);
  free(base);

  PASS();
}


TEST
open_special_files(void)
{
  char dir_template[] =
    "/tmp/avrmem_test_dir_XXXXXX";
  char *dir;
  char path[] =
    "/tmp/avrmem_test_XXXXXX";
  int fd;
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf = NULL;

  /*
   * Opening a directory must fail: load_file() reads the path with
   * fopen/fread, which cannot produce a valid ELF image from a dir.
   */
  dir =
    mkdtemp(dir_template);

  ASSERT(dir != NULL);
  ASSERT(!avr_elf_open(dir, &elf));
  ASSERT(elf == NULL);
  ASSERT(rmdir(dir) == 0);

  /*
   * Root bypasses file permission checks, so a chmod-000 file is
   * still readable there: SKIP the unreadable case instead of
   * asserting a failure that cannot happen.
   */
  if (geteuid() == 0)
    SKIPm("running as root: chmod 000 files stay readable");

  blob =
    build_minimal(&len);

  ASSERT(blob != NULL);

  fd =
    mkstemp(path);

  ASSERT(fd >= 0);
  ASSERT(write_all(fd, blob, len));
  close(fd);
  ASSERT(chmod(path, 0) == 0);
  ASSERT(!avr_elf_open(path, &elf));
  ASSERT(elf == NULL);
  ASSERT(chmod(path, 0600) == 0);
  unlink(path);
  free(blob);

  PASS();
}


TEST
overlapping_phdrs_first_wins(void)
{
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf;
  const AvrElfSection *ovl;
  uint32_t lma = 0;

  blob =
    build_overlap_blob(&len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);

  ovl =
    avr_elf_section(elf, OVL_PROGBITS_IDX);

  ASSERT(ovl != NULL);
  ASSERT(avr_elf_section_lma(elf, ovl, 0, &lma));

  /*
   * LOCK-IN: avr_elf_section_lma() scans program headers linearly
   * from index 0 and returns the first covering file-backed PT_LOAD
   * (src/avr_elf.c), so the 0x200 segment wins over the 0x300 one:
   * 0x200 + (0x110 - 0x100) == 0x210.
   */
  ASSERT_EQ((uint32_t)0x210, lma);

  close_blob(elf);
  free(blob);

  PASS();
}


TEST
zero_size_section(void)
{
  size_t len = 0;
  unsigned char *base;
  unsigned char *m;
  AvrElf *elf;
  AvrDevice dev =
    make_test_device();
  AvrResolvedSection resolved;
  const AvrElfSection *progbits;
  uint32_t lma = 0;

  base =
    build_symtab_blob(&len);

  ASSERT(base != NULL);

  m =
    malloc(len);

  ASSERT(m != NULL);
  memcpy(m, base, len);

  /* Shrink the PROGBITS section to size 0, still at a covered offset. */
  patch_u32(m, SYM_SHOFF + SYM_PROGBITS_IDX * SHDR_SIZE + SH_SIZE, 0);

  elf =
    open_blob(m, len);

  ASSERT(elf != NULL);
  ASSERT(avr_elf_resolve_section(elf, &dev, SYM_PROGBITS_IDX, &resolved));
  ASSERT_EQ((uint32_t)0, resolved.size);

  progbits =
    avr_elf_section(elf, SYM_PROGBITS_IDX);

  ASSERT(progbits != NULL);

  /*
   * LOCK-IN: the one-past-the-end rule (off <= size) admits off 0
   * when size is 0, so the LMA still resolves via the covering
   * PT_LOAD; off 1 exceeds the empty section and fails.
   */
  ASSERT(avr_elf_section_lma(elf, progbits, 0, &lma));
  ASSERT_EQ((uint32_t)0x210, lma);
  ASSERT(!avr_elf_section_lma(elf, progbits, 1, &lma));

  close_blob(elf);
  free(m);
  free(base);

  PASS();
}


TEST
shstrtab_at_index_0(void)
{
  size_t len = 0;
  unsigned char *base;
  unsigned char *m;
  AvrElf *elf;
  const AvrElfSection *shstrtab;

  base =
    build_minimal(&len);

  ASSERT(base != NULL);

  m =
    malloc(len);

  ASSERT(m != NULL);
  memcpy(m, base, len);

  /*
   * Move the shstrtab entry to SHDR[0] and clear SHDR[1] to NULL,
   * pointing shstrndx at 0.
   */
  memcpy(m + MIN_SHOFF, m + MIN_SHOFF + SHDR_SIZE, SHDR_SIZE);
  memset(m + MIN_SHOFF + SHDR_SIZE, 0, SHDR_SIZE);
  patch_u16(m, OFF_E_SHSTRNDX, 0);

  elf =
    open_blob(m, len);

  /*
   * LOCK-IN: the parser does not require SHDR[0] to be NULL; the
   * file opens and section names resolve through SHDR[0].
   */
  ASSERT(elf != NULL);
  ASSERT_EQ((size_t)2, avr_elf_section_count(elf));

  shstrtab =
    avr_elf_find_section(elf, ".shstrtab");

  ASSERT(shstrtab != NULL);
  ASSERT(shstrtab == avr_elf_section(elf, 0));

  close_blob(elf);
  free(m);
  free(base);

  PASS();
}


TEST
modern_bad_fallback_legacy(void)
{
  static const char note_name[] =
    ".note.gnu.avr.deviceinfo";
  static const char dev[] =
    "atmega328p";
  unsigned char desc[36];
  unsigned char payload[52];
  size_t len = 0;
  unsigned char *blob;
  AvrElf *elf;
  const char *device;

  /*
   * 36-byte descriptor: valid legacy content at +0 (length 11 +
   * "atmega328p\0") but a corrupt modern offset table (tbl_sz 4 < 8),
   * so parse_deviceinfo_modern() rejects it and the legacy fallback
   * in parse_deviceinfo_descriptor() (src/avr_elf.c) still resolves
   * the device name.
   */
  memset(desc, 0, sizeof(desc));
  put_u32le(desc + 0, 11);
  memcpy(desc + 4, dev, sizeof(dev));
  put_u32le(desc + 24, 4);

  memset(payload, 0, sizeof(payload));
  put_u32le(payload + 0, 4);
  put_u32le(payload + 4, (uint32_t)sizeof(desc));
  put_u32le(payload + 8, 1);
  payload[12] = (unsigned char)'A';
  payload[13] = (unsigned char)'V';
  payload[14] = (unsigned char)'R';
  payload[15] = 0;
  memcpy(payload + 16, desc, sizeof(desc));

  blob =
    build_note_blob(SHT_NOTE, payload, sizeof(payload), note_name, &len);

  ASSERT(blob != NULL);

  elf =
    open_blob(blob, len);

  ASSERT(elf != NULL);

  device =
    avr_elf_get_device_name(elf);

  ASSERT(device != NULL);
  ASSERT(strcmp(device, "atmega328p") == 0);

  close_blob(elf);
  free(blob);

  PASS();
}


SUITE(elf_negative)
{
  RUN_TEST(accept_minimal_blob);
  RUN_TEST(reject_non_elf);
  RUN_TEST(reject_class_encoding);
  RUN_TEST(reject_machine_type);
  RUN_TEST(reject_tables);
  RUN_TEST(reject_symtab);
  RUN_TEST(stripped_sections_resolve);
  RUN_TEST(nonalloc_section_unknown);
  RUN_TEST(note_garbage_still_opens);
  RUN_TEST(note_modern_and_legacy);
  RUN_TEST(accessor_null_safety);
  RUN_TEST(lma_rules);
  RUN_TEST(open_special_files);
  RUN_TEST(overlapping_phdrs_first_wins);
  RUN_TEST(zero_size_section);
  RUN_TEST(shstrtab_at_index_0);
  RUN_TEST(modern_bad_fallback_legacy);
}


int
main(int argc, char *argv[])
{
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(elf_negative);
  GREATEST_MAIN_END();
}
