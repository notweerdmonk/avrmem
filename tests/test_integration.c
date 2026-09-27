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
 * @file test_integration.c
 * @brief greatest.h TOOLCHAIN-GATED integration suite (real probe + fixtures).
 *
 * @details
 * Gated: the whole run SKIPPs (exit 0) when `FIXTURE_DIR/firmware.elf`
 * is unreadable (no `avr-gcc` at test time). Links `src/avr_device.c` +
 * `src/avr_device_probe.c` + `src/avr_sfr.c` + `src/avr_elf.c`. Build
 * with `-Iinclude -Itests/vendor`. One shared `AvrDevice` + `AvrElf`
 * are set up ONCE in `main()` (not per-test callbacks); tests needing
 * other ELFs open/close them locally. Never asserts exact SFR counts
 * or exact byte sizes (avr-libc varies).
 *
 * Depth tiers (Plan 2 Q4) run per discovered fixture device: the legacy
 * top-level `firmware.elf` plus every `FIXTURE_DIR/<device>/firmware.elf`
 * present (see `depth_discover()` — data-driven, no device name is
 * hardcoded). SFR-name legs tolerate device differences: PORTB is read
 * from the live probe per device, UCSR0A legs skip devices without it.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "greatest.h"
#include "avr_device.h"
#include "avr_elf.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>


#ifndef FIXTURE_DIR
#define FIXTURE_DIR "tests/fixtures/out"
#endif


GREATEST_MAIN_DEFS();


enum {
  FIXTURE_PATH_LEN = 1024,
  DEPTH_DEVICE_NAME_LEN = 64,
  DEPTH_DEVICE_MAX = 8,
  SFR_WORKDIR_LEN = 64
};


/*
 * ELF symbol binding/type numbers (SysV ABI STB_/STT_): avr_elf.h
 * exposes raw bind/type bytes without named constants, so depth tests
 * name the asserted values here together with their ABI meaning.
 */
enum {
  STB_LOCAL = 0,
  STB_GLOBAL = 1,
  STB_WEAK = 2,
  STT_NOTYPE = 0,
  STT_OBJECT = 1,
  STT_FUNC = 2,
  STT_FILE = 4
};


/*
 * One discovered fixture device: the firmware ELF plus its live device.
 *
 * Entry 0 is always the top-level FIXTURE_DIR/firmware.elf (the legacy
 * 328p path); further entries come from FIXTURE_DIR/<device>/
 * firmware.elf subdirectories (the gen.sh matrix layout). Discovery is
 * data-driven — any present subdirectory holding a firmware.elf is
 * iterated — so no device name is ever hardcoded here.
 */
typedef struct {
  char name[DEPTH_DEVICE_NAME_LEN];
  char path[FIXTURE_PATH_LEN];
  AvrDevice dev;
  AvrElf *elf;
} DepthDevice;


static AvrDevice dev;
static AvrElf *firmware = NULL;

static DepthDevice depth_devices[DEPTH_DEVICE_MAX];
static size_t depth_device_count = 0;


/**
 * @brief Join FIXTURE_DIR and a file name into buf.
 *
 * @details
 * buf must hold at least FIXTURE_PATH_LEN bytes. FIXTURE_DIR may be
 * overridden at build time with `-DFIXTURE_DIR="..."`.
 */
static void
fixture_path(
  char *buf,
  const char *name)
{
  snprintf(
    buf,
    FIXTURE_PATH_LEN,
    "%s/%s",
    FIXTURE_DIR,
    name);
}


/**
 * @brief Check `[A-Za-z0-9_-]+` (mirrors `valid_device_name`, keeps the
 * avr-gcc synthesis command safe against odd directory names).
 */
static bool
depth_name_valid(
  const char *name)
{
  size_t i = 0;

  if (name == NULL ||
      name[0] == '\0')
  {
    return false;
  }

  for (i = 0; name[i] != '\0'; ++i)
  {
    char c =
      name[i];

    if (!((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '_' ||
          c == '-'))
    {
      return false;
    }
  }

  return true;
}


/**
 * @brief Append one depth device (live init + open).
 *
 * @details
 * A device that fails to init or open is reported and skipped, never
 * fatal: a present-but-unusable matrix entry is a toolchain quirk, and
 * the suite's SKIP-over-FAIL policy applies.
 */
static void
depth_add(
  const char *name,
  const char *path)
{
  DepthDevice *slot = NULL;

  if (depth_device_count >=
        DEPTH_DEVICE_MAX ||
      strlen(name) >=
        DEPTH_DEVICE_NAME_LEN ||
      strlen(path) >=
        FIXTURE_PATH_LEN)
  {
    printf(
      "NOTE: depth device '%s' ignored (table full or name too long)\n",
      name);
    return;
  }

  slot =
    &depth_devices[depth_device_count];
  snprintf(
    slot->name,
    sizeof(slot->name),
    "%s",
    name);
  snprintf(
    slot->path,
    sizeof(slot->path),
    "%s",
    path);
  slot->elf =
    NULL;

  if (!avr_device_init(&slot->dev, slot->name))
  {
    printf(
      "NOTE: depth device '%s' skipped (probe failed)\n",
      slot->name);
    avr_device_destroy(&slot->dev);
    return;
  }

  if (!avr_elf_open(slot->path, &slot->elf))
  {
    printf(
      "NOTE: depth device '%s' skipped (ELF unreadable)\n",
      slot->name);
    avr_device_destroy(&slot->dev);
    return;
  }

  ++depth_device_count;
}


/**
 * @brief Discover depth devices: top-level firmware.elf plus any
 * `FIXTURE_DIR/<device>/firmware.elf` found by directory scan.
 *
 * @details
 * The top-level device name comes from the ELF note itself (falling back
 * to `"atmega328p"`, matching the shared setup below); subdirectory
 * devices are named by their directory, which is exactly the gen.sh
 * matrix layout (`out/<device>/firmware.elf`).
 */
static void
depth_discover(
  const char *top_path)
{
  DIR *dir = NULL;
  struct dirent *entry = NULL;
  const char *top_name = NULL;

  top_name =
    avr_elf_get_device_name(firmware);
  if (top_name == NULL)
    top_name = "atmega328p";
  depth_add(top_name, top_path);

  dir =
    opendir(FIXTURE_DIR);
  if (dir == NULL)
    return;

  while ((entry = readdir(dir)) != NULL)
  {
    char sub[FIXTURE_PATH_LEN];
    struct stat st;
    int written = 0;

    if (strcmp(entry->d_name, ".") == 0 ||
        strcmp(entry->d_name, "..") == 0)
    {
      continue;
    }

    if (!depth_name_valid(entry->d_name))
      continue;

    written =
      snprintf(
        sub,
        sizeof(sub),
        "%s/%s/firmware.elf",
        FIXTURE_DIR,
        entry->d_name);
    if (written < 0 ||
        (size_t)written >= sizeof(sub))
    {
      continue;
    }

    if (stat(sub, &st) != 0 ||
        !S_ISREG(st.st_mode))
    {
      continue;
    }

    depth_add(entry->d_name, sub);
  }

  closedir(dir);

  printf(
    "depth devices: %lu\n",
    (unsigned long)depth_device_count);
}


/*
 * Scratch SFR-probe paths (B17): fixed-size so path joins are provably
 * truncation-free under -Wformat-truncation.
 */
typedef struct {
  char dir[SFR_WORKDIR_LEN];
  char src[FIXTURE_PATH_LEN];
  char elf_path[FIXTURE_PATH_LEN];
} SfrProbe;


/*
 * Scratch SFR-probe source (B17): one byte in a dedicated section. The
 * test links it with `-Wl,--section-start=.sfr_probe=<vma>` so the
 * symbol is section-relative at the SFR linker VMA — the R7 spike winner
 * (see the B17 test comment for the losers).
 */
static const char sfr_probe_source[] =
  "unsigned char sfr_portb __attribute__((section(\".sfr_probe\")));\n"
  "int main(void) { return 0; }\n";


/**
 * @brief Build a scratch ELF carrying `sfr_portb` at `vma`, open it.
 *
 * @details
 * Synthesized at test time with the live `avr-gcc` (the suite already
 * gates on the toolchain); `gen.sh` and `firmware.c` stay untouched.
 * The device name was charset-validated at discovery, and temp paths
 * come from `mkdtemp`, so the `system()` command below is safe.
 *
 * On success `*out_elf` holds the open ELF and `probe` holds the temp
 * paths for `sfr_probe_cleanup`.
 */
static bool
build_sfr_probe_elf(
  const DepthDevice *dd,
  uint32_t vma,
  SfrProbe *probe,
  AvrElf **out_elf)
{
  char cmd[FIXTURE_PATH_LEN * 2];
  FILE *out = NULL;
  int written = 0;
  int rc = 0;

  snprintf(
    probe->dir,
    sizeof(probe->dir),
    "/tmp/avrmem_sfr_XXXXXX");
  if (mkdtemp(probe->dir) == NULL)
    return false;

  snprintf(
    probe->src,
    sizeof(probe->src),
    "%s/sfr.c",
    probe->dir);
  snprintf(
    probe->elf_path,
    sizeof(probe->elf_path),
    "%s/sfr.elf",
    probe->dir);

  out =
    fopen(probe->src, "w");
  if (out == NULL)
    return false;

  if (fputs(sfr_probe_source, out) < 0)
  {
    fclose(out);
    return false;
  }

  if (fclose(out) != 0)
    return false;

  written =
    snprintf(
      cmd,
      sizeof(cmd),
      "avr-gcc -mmcu=%s -Os -g -o '%s' '%s' "
      "-Wl,--section-start=.sfr_probe=0x%x",
      dd->name,
      probe->elf_path,
      probe->src,
      vma);
  if (written < 0 ||
      (size_t)written >= sizeof(cmd))
  {
    return false;
  }

  rc =
    system(cmd);
  if (rc != 0)
    return false;

  return avr_elf_open(probe->elf_path, out_elf);
}


/**
 * @brief Undo `build_sfr_probe_elf`: close the ELF, remove temp files.
 */
static void
sfr_probe_cleanup(
  const SfrProbe *probe,
  AvrElf *elf)
{
  avr_elf_close(elf);

  unlink(probe->src);
  unlink(probe->elf_path);
  rmdir(probe->dir);
}


TEST
probe_yields_regions(void)
{
  ASSERT(avr_device_flash_region(&dev).size != 0);
  ASSERT(avr_device_sram_region(&dev).size != 0);
  ASSERT(avr_device_eeprom_region(&dev).size != 0);
  ASSERT(avr_device_text_region_length(&dev) != 0);
  ASSERT(avr_device_data_region_length(&dev) != 0);
  ASSERT(avr_device_eeprom_region_length(&dev) != 0);

  /* SFR presence only (never an exact count — avr-libc varies). */
  ASSERT(avr_device_find_register(&dev, "PORTB") != NULL);
  ASSERT(avr_device_find_register(&dev, "SREG") != NULL);

  PASS();
}


TEST
auto_vs_explicit_agree(void)
{
  const char *name =
    avr_elf_get_device_name(firmware);
  AvrDevice second;
  AvrResolvedSymbol via_shared;
  AvrResolvedSymbol via_named;

  ASSERT(name != NULL);
  ASSERT(avr_device_init(&second, name));

  ASSERT(avr_elf_resolve_symbol(firmware, &dev, "main", &via_shared));
  ASSERT(avr_elf_resolve_symbol(firmware, &second, "main", &via_named));

  ASSERT_EQ(via_shared.avr_address, via_named.avr_address);
  ASSERT_EQ(via_shared.physical_address, via_named.physical_address);
  ASSERT_EQ(via_shared.memory_space, via_named.memory_space);

  avr_device_destroy(&second);

  PASS();
}


TEST
data_vma_lma_identity(void)
{
  AvrResolvedSymbol sym;
  uint32_t section_offset = 0;
  uint32_t section_lma = 0;

  ASSERT(avr_elf_resolve_symbol(
    firmware,
    &dev,
    "initialized_value",
    &sym));
  ASSERT_EQ(AVR_MEM_SRAM, sym.memory_space);
  ASSERT(sym.has_lma);
  ASSERT(sym.section != NULL);

  section_offset =
    sym.value - sym.section->addr;
  ASSERT(avr_elf_section_lma(
    firmware,
    sym.section,
    section_offset,
    &section_lma));
  ASSERT_EQ(section_lma, sym.lma);

  /* The .data init image lives in FLASH. */
  ASSERT_EQ(AVR_MEM_FLASH, avr_device_classify_address(&dev, sym.lma));

  PASS();
}


TEST
bss_no_lma(void)
{
  AvrResolvedSymbol sym;

  ASSERT(avr_elf_resolve_symbol(firmware, &dev, "counter_bss", &sym));
  ASSERT_EQ(AVR_MEM_SRAM, sym.memory_space);
  ASSERT(!sym.has_lma);

  PASS();
}


TEST
noinit_preserved_no_lma(void)
{
  AvrResolvedSymbol sym;

  ASSERT(avr_elf_resolve_symbol(firmware, &dev, "preserved_value", &sym));
  ASSERT_EQ(AVR_MEM_SRAM, sym.memory_space);
  ASSERT(!sym.has_lma);

  PASS();
}


TEST
flash_symbol_word(void)
{
  AvrResolvedSymbol sym;
  uint32_t shift = 0;

  ASSERT(avr_elf_resolve_symbol(firmware, &dev, "main", &sym));
  ASSERT_EQ(AVR_MEM_FLASH, sym.memory_space);
  ASSERT(sym.has_flash_word_address);

  shift =
    avr_device_flash_address_shift(&dev);
  ASSERT_EQ(
    (uint32_t)(sym.physical_address >> shift),
    sym.flash_word_address);

  PASS();
}


TEST
sections_have_names_sizes(void)
{
  const AvrElfSection *text =
    avr_elf_find_section(firmware, ".text");
  const AvrElfSection *data =
    avr_elf_find_section(firmware, ".data");
  const AvrElfSection *bss =
    avr_elf_find_section(firmware, ".bss");
  AvrResolvedSection resolved;

  ASSERT(text != NULL);
  ASSERT(text->size > 0);
  ASSERT(data != NULL);
  ASSERT(data->size > 0);
  ASSERT(bss != NULL);
  ASSERT(bss->size > 0);

  ASSERT(avr_elf_resolve_section(firmware, &dev, text->index, &resolved));
  ASSERT_EQ(AVR_MEM_FLASH, resolved.memory_space);

  ASSERT(avr_elf_resolve_section(firmware, &dev, data->index, &resolved));
  ASSERT_EQ(AVR_MEM_SRAM, resolved.memory_space);

  ASSERT(avr_elf_resolve_section(firmware, &dev, bss->index, &resolved));
  ASSERT_EQ(AVR_MEM_SRAM, resolved.memory_space);

  PASS();
}


TEST
stripped_sections_still_work(void)
{
  char path[FIXTURE_PATH_LEN];
  AvrElf *elf = NULL;
  const AvrElfSection *text = NULL;
  AvrResolvedSection resolved;
  AvrResolvedSymbol sym;

  fixture_path(path, "stripped.elf");
  ASSERT(avr_elf_open(path, &elf));
  ASSERT(avr_elf_section_count(elf) > 0);

  text =
    avr_elf_find_section(elf, ".text");
  ASSERT(text != NULL);
  ASSERT(avr_elf_resolve_section(elf, &dev, text->index, &resolved));

  /* No .symtab: symbol queries fail cleanly (no crash). */
  ASSERT(!avr_elf_resolve_symbol(elf, &dev, "main", &sym));

  avr_elf_close(elf);

  PASS();
}


TEST
no_metadata_needs_device(void)
{
  char path[FIXTURE_PATH_LEN];
  AvrElf *elf = NULL;
  AvrDevice explicit_dev;
  AvrResolvedSymbol sym;

  fixture_path(path, "no_metadata.elf");
  ASSERT(avr_elf_open(path, &elf));
  ASSERT(avr_elf_get_device_name(elf) == NULL);

  ASSERT(avr_device_init(&explicit_dev, "atmega328p"));
  ASSERT(avr_elf_resolve_symbol(elf, &explicit_dev, "main", &sym));

  avr_device_destroy(&explicit_dev);
  avr_elf_close(elf);

  PASS();
}


TEST
invalid_device_and_elf(void)
{
  AvrDevice tmp;
  char path[FIXTURE_PATH_LEN];
  AvrElf *bad = NULL;
  FILE *probe = NULL;

  ASSERT(!avr_device_init(&tmp, "this_is_not_an_avr"));

  /*
   * Safe: avr_device_init zeroes the object before probing, so destroy
   * after a failed init releases nothing (NULL name/database).
   */
  avr_device_destroy(&tmp);

  fixture_path(path, "invalid.bin");
  ASSERT(!avr_elf_open(path, &bad));
  avr_elf_close(bad);
  bad = NULL;

  fixture_path(path, "host.so");
  probe =
    fopen(path, "rb");
  if (probe != NULL)
  {
    /*
     * Host shared object present: the AVR parser must reject it.
     * (Absent host.so — no host cc at fixture time — is not a failure;
     * invalid.bin above already covers the rejection path.)
     */
    fclose(probe);
    ASSERT(!avr_elf_open(path, &bad));
    avr_elf_close(bad);
  }

  PASS();
}


TEST
ownership_lifecycle(void)
{
  AvrDevice tmp;
  char path[FIXTURE_PATH_LEN];
  AvrElf *elf = NULL;

  ASSERT(avr_device_init(&tmp, "atmega328p"));
  avr_device_destroy(&tmp);

  fixture_path(path, "firmware.elf");
  ASSERT(avr_elf_open(path, &elf));
  avr_elf_close(elf);

  /* NULL-tolerant teardown (no crash). */
  avr_device_destroy(NULL);
  avr_elf_close(NULL);

  PASS();
}


/*
 * ----------------------------------------------------------------------
 * Plan 2 Q4 depth tiers: one loop per discovered fixture device.
 * Classifications/identities/relations only; exact numbers appear solely
 * where toolchain-self-consistent (same avr-gcc builds the fixture and
 * feeds the probe). Observed deviations from the plan are documented
 * per test (trust code).
 * ----------------------------------------------------------------------
 */


TEST
eeprom_depth(void)
{
  size_t d = 0;

  /*
   * B16: the EEMEM eeprom_value resolves to EEPROM space with a physical
   * address and a file-backed LMA (observed: VMA 0x00810000, physical 0,
   * .eeprom section on every fixture device — locked as observed).
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    AvrResolvedSymbol sym;

    ASSERT(avr_elf_resolve_symbol(
      dd->elf,
      &dd->dev,
      "eeprom_value",
      &sym));
    ASSERT_EQ(AVR_MEM_EEPROM, sym.memory_space);
    ASSERT(sym.has_physical_address);
    ASSERT_EQ((uint32_t)0, sym.physical_address);
    ASSERT(sym.has_lma);
    ASSERT(sym.section != NULL);
    ASSERT_STR_EQ(".eeprom", sym.section->name);
  }

  PASS();
}


TEST
sfr_probe_symbol(void)
{
  size_t d = 0;
  size_t ran = 0;

  /*
   * B17: a symbol at a known SFR address carries sfr != NULL naming the
   * register. R7 spike result: --defsym and asm .set both emit ABS
   * symbols (resolve UNKNOWN, sfr NULL — losers); avr-objcopy
   * --add-symbol adds the section VMA onto the value (lands outside the
   * section, resolution fails — loser). Winner: a real section forced to
   * the SFR linker VMA (data origin + physical SFR address, both read
   * live per device) via -Wl,--section-start, synthesized here at test
   * time so gen.sh/firmware.c stay untouched.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    const AvrSfr *portb = NULL;
    uint32_t vma = 0;
    SfrProbe probe_elf;
    AvrElf *probe = NULL;
    AvrResolvedSymbol sym;

    /* PORTB verified on 328p/2560/t85; a device without it skips. */
    portb =
      avr_device_find_register(&dd->dev, "PORTB");
    if (portb == NULL)
    {
      printf(
        "NOTE: %s has no PORTB, sfr leg skipped\n",
        dd->name);
      continue;
    }

    vma =
      avr_device_data_region_origin(&dd->dev) +
      (uint32_t)portb->address;

    ASSERT(build_sfr_probe_elf(dd, vma, &probe_elf, &probe));
    ASSERT(probe != NULL);
    ASSERT(avr_elf_resolve_symbol(probe, &dd->dev, "sfr_portb", &sym));
    ASSERT_EQ(AVR_MEM_IO, sym.memory_space);
    ASSERT_EQ((uint32_t)portb->address, sym.avr_address);
    ASSERT(sym.sfr != NULL);
    ASSERT_STR_EQ("PORTB", sym.sfr->name);

    sfr_probe_cleanup(&probe_elf, probe);
    ++ran;
  }

  if (ran == 0)
    SKIPm("no fixture device exposes PORTB");

  PASS();
}


TEST
mem8_register_identity(void)
{
  size_t d = 0;
  size_t ran = 0;

  /*
   * B18: find_register_address(MEM8 addr) == find(name). The MEM8 address
   * is read from the live probe first (UCSR0A is 0xC0 on 328p/2560);
   * attiny85 has no MEM-mapped SFR at all, so it skips this leg.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    const AvrSfr *by_name = NULL;
    const AvrSfr *by_addr = NULL;

    by_name =
      avr_device_find_register(&dd->dev, "UCSR0A");
    if (by_name == NULL)
    {
      printf(
        "NOTE: %s has no UCSR0A, MEM8 leg skipped\n",
        dd->name);
      continue;
    }

    by_addr =
      avr_device_find_register_address(&dd->dev, by_name->address);
    ASSERT(by_addr != NULL);
    ASSERT(by_addr == by_name);
    ++ran;
  }

  if (ran == 0)
    SKIPm("no fixture device exposes UCSR0A");

  PASS();
}


TEST
elf_identity(void)
{
  size_t d = 0;

  /*
   * B19 deviation (trust code): the plan asked entry != 0, but avr-gcc
   * emits entry 0 (vectors at zero) on every fixture — locked as
   * observed. Machine is EM_AVR (83); section/symbol counts are nonzero.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];

    ASSERT_EQ(
      (uint16_t)AVR_ELF_MACHINE_AVR,
      avr_elf_machine(dd->elf));
    ASSERT_EQ(
      (uint16_t)AVR_ELF_MACHINE_AVR,
      avr_elf_get_machine(dd->elf));
    ASSERT_EQ((uint32_t)0, avr_elf_entry(dd->elf));
    ASSERT(avr_elf_section_count(dd->elf) > 0);
    ASSERT(avr_elf_symbol_count(dd->elf) > 0);
  }

  PASS();
}


TEST
section_flag_letters(void)
{
  size_t d = 0;

  /*
   * B20: link letters read live first (avr-readelf -S on all fixtures):
   * .text AX, .data AW, .bss WA + NOBITS. Never exact sizes here.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    const AvrElfSection *text =
      avr_elf_find_section(dd->elf, ".text");
    const AvrElfSection *data =
      avr_elf_find_section(dd->elf, ".data");
    const AvrElfSection *bss =
      avr_elf_find_section(dd->elf, ".bss");

    ASSERT(text != NULL);
    ASSERT(text->alloc &&
           text->executable &&
           !text->writable &&
           !text->nobits);

    ASSERT(data != NULL);
    ASSERT(data->alloc &&
           data->writable &&
           !data->executable &&
           !data->nobits);

    ASSERT(bss != NULL);
    ASSERT(bss->alloc &&
           bss->writable &&
           !bss->executable &&
           bss->nobits);
  }

  PASS();
}


TEST
main_useful_file_excluded(void)
{
  size_t d = 0;

  /*
   * B21: main is GLOBAL FUNC and presentation-useful; STT_FILE rows
   * (e.g. "firmware.c") are not. The FILE row is found by iteration so
   * no object name is hardcoded.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    AvrResolvedSymbol main_sym;
    size_t n =
      avr_elf_symbol_count(dd->elf);
    size_t i = 0;
    bool saw_file = false;

    ASSERT(avr_elf_resolve_symbol(dd->elf, &dd->dev, "main", &main_sym));
    ASSERT_EQ((uint8_t)STT_FUNC, main_sym.symbol.type);
    ASSERT_EQ((uint8_t)STB_GLOBAL, main_sym.symbol.bind);
    ASSERT(avr_elf_symbol_is_useful(&main_sym.symbol));

    for (i = 0; i < n; ++i)
    {
      const AvrElfSymbol *raw =
        avr_elf_symbol_at(dd->elf, i);

      if (raw != NULL &&
          raw->type == STT_FILE)
      {
        ASSERT(!avr_elf_symbol_is_useful(raw));
        saw_file = true;
        break;
      }
    }

    ASSERT(saw_file);
  }

  PASS();
}


TEST
resolver_alias_equivalence(void)
{
  size_t d = 0;

  /*
   * B22: the by-name, by-index and _at resolvers agree on main. The
   * index is located by iteration (first "main"), never hardcoded.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    size_t n =
      avr_elf_symbol_count(dd->elf);
    size_t i = 0;
    size_t main_index = 0;
    bool found = false;
    AvrResolvedSymbol by_name;
    AvrResolvedSymbol by_index;
    AvrResolvedSymbol by_at;

    for (i = 0; i < n; ++i)
    {
      const AvrElfSymbol *raw =
        avr_elf_symbol_at(dd->elf, i);

      if (raw != NULL &&
          raw->name != NULL &&
          strcmp(raw->name, "main") == 0)
      {
        main_index = i;
        found = true;
        break;
      }
    }

    ASSERT(found);
    ASSERT(avr_elf_resolve_symbol(dd->elf, &dd->dev, "main", &by_name));
    ASSERT(avr_elf_resolve_symbol_index(
      dd->elf,
      &dd->dev,
      main_index,
      &by_index));
    ASSERT(avr_elf_resolve_symbol_at(
      dd->elf,
      &dd->dev,
      main_index,
      &by_at));

    ASSERT_EQ(by_name.value, by_index.value);
    ASSERT_EQ(by_name.value, by_at.value);
    ASSERT_EQ(by_name.memory_space, by_index.memory_space);
    ASSERT_EQ(by_name.memory_space, by_at.memory_space);
    ASSERT_EQ(by_name.avr_address, by_index.avr_address);
    ASSERT_EQ(by_name.avr_address, by_at.avr_address);
    ASSERT_EQ(by_name.physical_address, by_index.physical_address);
    ASSERT_EQ(by_name.physical_address, by_at.physical_address);
  }

  PASS();
}


TEST
duplicate_name_winner(void)
{
  size_t d = 0;

  /*
   * B23: firmware.c emits a duplicate-name pair — GLOBAL dup_cnt (file
   * scope) plus LOCAL dup_cnt.NNNN (main-local static). By-name lookup
   * matches the exact name, so the GLOBAL row wins; locked with the
   * LOCAL sibling pinned as still present.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    AvrResolvedSymbol sym;
    size_t n =
      avr_elf_symbol_count(dd->elf);
    size_t i = 0;
    bool saw_local = false;

    ASSERT(avr_elf_resolve_symbol(dd->elf, &dd->dev, "dup_cnt", &sym));
    ASSERT_EQ((uint8_t)STB_GLOBAL, sym.symbol.bind);
    ASSERT_EQ(AVR_MEM_SRAM, sym.memory_space);
    ASSERT(!sym.has_lma);

    for (i = 0; i < n; ++i)
    {
      const AvrElfSymbol *raw =
        avr_elf_symbol_at(dd->elf, i);

      if (raw != NULL &&
          raw->name != NULL &&
          strncmp(raw->name, "dup_cnt.", 8) == 0)
      {
        ASSERT_EQ((uint8_t)STB_LOCAL, raw->bind);
        saw_local = true;
        break;
      }
    }

    ASSERT(saw_local);
  }

  PASS();
}


TEST
linked_exit_in_text(void)
{
  size_t d = 0;

  /*
   * B24 deviation (trust code): the plan assumed exit is UNDEF (via
   * main's return), but every fixture links avr-libc's exit as a WEAK
   * NOTYPE .text symbol, so resolution succeeds into FLASH with a valid
   * word address. Locked as observed.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    AvrResolvedSymbol sym;

    ASSERT(avr_elf_resolve_symbol(dd->elf, &dd->dev, "exit", &sym));
    ASSERT_EQ((uint8_t)STB_WEAK, sym.symbol.bind);
    ASSERT_EQ(AVR_MEM_FLASH, sym.memory_space);
    ASSERT(sym.section != NULL);
    ASSERT_STR_EQ(".text", sym.section->name);
    ASSERT(sym.has_flash_word_address);
  }

  PASS();
}


TEST
absolute_sreg_unknown(void)
{
  size_t d = 0;

  /*
   * B25: __SREG__ is an SHN_ABS NOTYPE row (0x3F on all fixtures), so it
   * resolves with UNKNOWN space, no physical address, no section, no
   * LMA and no SFR — locked as observed.
   */
  for (d = 0; d < depth_device_count; ++d)
  {
    DepthDevice *dd =
      &depth_devices[d];
    AvrResolvedSymbol sym;

    ASSERT(avr_elf_resolve_symbol(dd->elf, &dd->dev, "__SREG__", &sym));
    ASSERT_EQ(AVR_MEM_UNKNOWN, sym.memory_space);
    ASSERT(!sym.has_physical_address);
    ASSERT(sym.section == NULL);
    ASSERT(!sym.has_lma);
    ASSERT(sym.sfr == NULL);
    ASSERT_EQ((uint32_t)0x3f, sym.avr_address);
  }

  PASS();
}


SUITE(integration)
{
  RUN_TEST(probe_yields_regions);
  RUN_TEST(auto_vs_explicit_agree);
  RUN_TEST(data_vma_lma_identity);
  RUN_TEST(bss_no_lma);
  RUN_TEST(noinit_preserved_no_lma);
  RUN_TEST(flash_symbol_word);
  RUN_TEST(sections_have_names_sizes);
  RUN_TEST(stripped_sections_still_work);
  RUN_TEST(no_metadata_needs_device);
  RUN_TEST(invalid_device_and_elf);
  RUN_TEST(ownership_lifecycle);
  RUN_TEST(eeprom_depth);
  RUN_TEST(sfr_probe_symbol);
  RUN_TEST(mem8_register_identity);
  RUN_TEST(elf_identity);
  RUN_TEST(section_flag_letters);
  RUN_TEST(main_useful_file_excluded);
  RUN_TEST(resolver_alias_equivalence);
  RUN_TEST(duplicate_name_winner);
  RUN_TEST(linked_exit_in_text);
  RUN_TEST(absolute_sreg_unknown);
}


int
main(
  int argc,
  char *argv[])
{
  char path[FIXTURE_PATH_LEN];
  FILE *probe = NULL;
  size_t i = 0;

  fixture_path(path, "firmware.elf");
  probe =
    fopen(path, "rb");
  if (probe == NULL)
  {
    printf(
      "SKIP: fixtures missing (no avr-gcc at test time) — "
      "run make test with toolchain\n");
    return 0;
  }
  fclose(probe);

  if (!avr_device_init(&dev, "atmega328p"))
  {
    printf(
      "SKIP: avr-gcc toolchain broken (probe failed) — "
      "run make test with toolchain\n");
    return 0;
  }

  if (!avr_elf_open(path, &firmware))
  {
    printf("FAIL: fixtures exist but firmware.elf is unreadable\n");
    avr_device_destroy(&dev);
    return 1;
  }

  depth_discover(path);

  GREATEST_MAIN_BEGIN();
  RUN_SUITE(integration);

  for (i = 0; i < depth_device_count; ++i)
  {
    avr_elf_close(depth_devices[i].elf);
    depth_devices[i].elf = NULL;
    avr_device_destroy(&depth_devices[i].dev);
  }

  avr_elf_close(firmware);
  firmware = NULL;
  avr_device_destroy(&dev);

  GREATEST_MAIN_END();
}
