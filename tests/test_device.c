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
 * @file test_device.c
 * @brief greatest.h unit suite for the device module (`src/avr_device.c`).
 *
 * @details
 * Toolchain-free: links `src/avr_device.c` + `src/avr_sfr.c` only. Build
 * with `-Iinclude -Itests/vendor`. Fixtures are hand-built with `memset` +
 * field assignment; `avr_device_init` is never called (it needs the
 * toolchain), and `avr_device_destroy` is never called on hand-built
 * fixtures (they own nothing: `name == NULL`, `sfr_database == NULL`, so
 * there is nothing to release).
 */

#include "greatest.h"
#include "avr_device.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>


GREATEST_MAIN_DEFS();


/**
 * @brief Build an ATmega328P-like device by value.
 *
 * @details
 * Source-derived values (must match the device layer, see
 * `CODEBASE_REFERENCE.md`): SRAM base `0x0100`, DATA origin `0x00800000`,
 * register-file `{0x0000, 0x0020}`, I/O `{0x0020, 0x0040}`, extended-I/O
 * `{0x0060, 0x00A0}`, FLASH byte-to-word shift `1`.
 *
 * Test-chosen values (arbitrary filler, labeled as such): FLASH size
 * `0x8000`, SRAM size `0x0800`, EEPROM `{0x0000, 0x0400}`, TEXT space
 * `{0x00000000, 0x8000}`, EEPROM space `{0x00810000, 0x0400}`, DATA length
 * `0x010000`.
 */
static AvrDevice
make_328p_like(void)
{
  AvrDevice dev;

  memset(&dev, 0, sizeof(dev));

  dev.name = NULL;
  dev.flash.start = 0x0000;
  dev.flash.size = 0x8000;
  dev.sram.start = 0x0100;
  dev.sram.size = 0x0800;
  dev.eeprom.start = 0x0000;
  dev.eeprom.size = 0x0400;
  dev.data_space.origin = 0x00800000;
  dev.data_space.length = 0x010000;
  dev.text_space.origin = 0x00000000;
  dev.text_space.length = 0x8000;
  dev.eeprom_space.origin = 0x00810000;
  dev.eeprom_space.length = 0x0400;
  dev.register_file.start = 0x0000;
  dev.register_file.size = 0x0020;
  dev.io.start = 0x0020;
  dev.io.size = 0x0040;
  dev.ext_io.start = 0x0060;
  dev.ext_io.size = 0x00A0;
  dev.sfr_database = NULL;
  dev.flash_address_shift = 1;

  return dev;
}


TEST
classify_boundaries(void)
{
  AvrDevice dev =
    make_328p_like();

  ASSERT_EQ(AVR_MEM_REGISTER, avr_device_classify_address(&dev, 0x00800000));
  ASSERT_EQ(AVR_MEM_REGISTER, avr_device_classify_address(&dev, 0x0080001F));
  ASSERT_EQ(AVR_MEM_IO, avr_device_classify_address(&dev, 0x00800020));
  ASSERT_EQ(AVR_MEM_IO, avr_device_classify_address(&dev, 0x0080005F));
  ASSERT_EQ(AVR_MEM_IO, avr_device_classify_address(&dev, 0x00800060));
  ASSERT_EQ(AVR_MEM_IO, avr_device_classify_address(&dev, 0x008000FF));
  ASSERT_EQ(AVR_MEM_SRAM, avr_device_classify_address(&dev, 0x00800100));
  ASSERT_EQ(AVR_MEM_SRAM, avr_device_classify_address(&dev, 0x00800123));
  ASSERT_EQ(AVR_MEM_FLASH, avr_device_classify_address(&dev, 0x00000000));

  ASSERT_EQ(AVR_MEM_EEPROM, avr_device_classify_address(&dev, 0x00810000));

  /* SRAM covers 0x0100..0x08FF, so DATA offset 0x0950 is a gap. */
  ASSERT_EQ(AVR_MEM_UNKNOWN, avr_device_classify_address(&dev, 0x00800950));

  /* text_space {0, 0x8000} is half-open: origin + length is outside. */
  ASSERT_EQ(AVR_MEM_UNKNOWN, avr_device_classify_address(&dev, 0x00008000));

  PASS();
}


TEST
classify_priority(void)
{
  AvrDevice overlapping =
    make_328p_like();
  AvrDevice eeprom_overflow =
    make_328p_like();

  /* TEXT wins over DATA when the linker spaces overlap. */
  overlapping.data_space.origin = 0x00000000;
  ASSERT_EQ(AVR_MEM_FLASH, avr_device_classify_address(&overlapping, 0x100));

  /*
   * In-space but past the physical EEPROM end: UNKNOWN with no DATA
   * fall-through (the EEPROM check is a hard stop).
   */
  eeprom_overflow.eeprom.size = 0x10;
  ASSERT_EQ(
    AVR_MEM_UNKNOWN,
    avr_device_classify_address(&eeprom_overflow, 0x00810020));

  PASS();
}


TEST
to_physical(void)
{
  AvrDevice dev =
    make_328p_like();
  uint32_t out = 0;

  ASSERT(avr_device_to_physical_sram(&dev, 0x00800194, &out));
  ASSERT_EQ((uint32_t)0x0194, out);

  /* I/O address: inside DATA space but below sram.start. */
  out = 0xDEADBEEF;
  ASSERT(!avr_device_to_physical_sram(&dev, 0x00800020, &out));
  ASSERT_EQ((uint32_t)0xDEADBEEF, out);

  ASSERT(avr_device_to_physical_flash(&dev, 0x0010, &out));
  ASSERT_EQ((uint32_t)0x0010, out);

  /* Out-of-range FLASH: one past the end leaves *out untouched. */
  out = 0xDEADBEEF;
  ASSERT(!avr_device_to_physical_flash(&dev, 0x8000, &out));
  ASSERT_EQ((uint32_t)0xDEADBEEF, out);

  ASSERT(avr_device_to_physical_eeprom(&dev, 0x00810010, &out));
  ASSERT_EQ((uint32_t)0x0010, out);

  out = 0xDEADBEEF;
  ASSERT(!avr_device_to_physical_eeprom(&dev, 0x00810400, &out));
  ASSERT_EQ((uint32_t)0xDEADBEEF, out);

  PASS();
}


TEST
flash_byte_to_word(void)
{
  AvrDevice dev =
    make_328p_like();
  AvrDevice shift32 =
    make_328p_like();
  uint32_t word = 0;

  ASSERT(avr_device_flash_byte_to_word(&dev, 0x2A12, &word));
  ASSERT_EQ((uint32_t)0x1509, word);

  /* Odd byte address maps to the containing instruction word. */
  ASSERT(avr_device_flash_byte_to_word(&dev, 0x2A13, &word));
  ASSERT_EQ((uint32_t)0x1509, word);

  word = 0xDEADBEEF;
  ASSERT(!avr_device_flash_byte_to_word(&dev, 0x8000, &word));
  ASSERT_EQ((uint32_t)0xDEADBEEF, word);

  shift32.flash_address_shift = 32;
  word = 0xDEADBEEF;
  ASSERT(!avr_device_flash_byte_to_word(&shift32, 0x2A12, &word));
  ASSERT_EQ((uint32_t)0xDEADBEEF, word);

  PASS();
}


TEST
resolve_address(void)
{
  AvrDevice dev =
    make_328p_like();
  AvrMemorySpace ms = AVR_MEM_UNKNOWN;
  uint32_t avr = 0;
  uint32_t phys = 0;

  ASSERT(avr_device_resolve_address(&dev, 0x00800000, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_REGISTER, ms);
  ASSERT_EQ((uint32_t)0x0000, avr);
  ASSERT_EQ((uint32_t)0x0000, phys);

  ASSERT(avr_device_resolve_address(&dev, 0x00800025, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_IO, ms);
  ASSERT_EQ((uint32_t)0x0025, avr);
  ASSERT_EQ((uint32_t)0x0025, phys);

  ASSERT(avr_device_resolve_address(&dev, 0x00800123, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_SRAM, ms);
  ASSERT_EQ((uint32_t)0x0123, avr);
  ASSERT_EQ((uint32_t)0x0123, phys);

  ASSERT(avr_device_resolve_address(&dev, 0x0010, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_FLASH, ms);
  ASSERT_EQ((uint32_t)0x0010, avr);
  ASSERT_EQ((uint32_t)0x0010, phys);

  ASSERT(avr_device_resolve_address(&dev, 0x00810010, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_EEPROM, ms);
  ASSERT_EQ((uint32_t)0x0010, avr);
  ASSERT_EQ((uint32_t)0x0010, phys);

  /* UNKNOWN: pre-filled outs must be zeroed. */
  ms = AVR_MEM_FLASH;
  avr = 0xDEADBEEF;
  phys = 0xDEADBEEF;
  ASSERT(!avr_device_resolve_address(&dev, 0x00800950, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_UNKNOWN, ms);
  ASSERT_EQ((uint32_t)0, avr);
  ASSERT_EQ((uint32_t)0, phys);

  /* NULL device: false, and non-NULL outs still zeroed. */
  ms = AVR_MEM_FLASH;
  avr = 0xDEADBEEF;
  phys = 0xDEADBEEF;
  ASSERT(!avr_device_resolve_address(NULL, 0x00800123, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_UNKNOWN, ms);
  ASSERT_EQ((uint32_t)0, avr);
  ASSERT_EQ((uint32_t)0, phys);

  /* Each nullable out on its own still resolves. */
  ASSERT(avr_device_resolve_address(&dev, 0x00800123, NULL, &avr, &phys));
  ASSERT_EQ((uint32_t)0x0123, avr);
  ASSERT_EQ((uint32_t)0x0123, phys);

  ASSERT(avr_device_resolve_address(&dev, 0x00800123, &ms, NULL, &phys));
  ASSERT_EQ(AVR_MEM_SRAM, ms);
  ASSERT_EQ((uint32_t)0x0123, phys);

  ASSERT(avr_device_resolve_address(&dev, 0x00800123, &ms, &avr, NULL));
  ASSERT_EQ(AVR_MEM_SRAM, ms);
  ASSERT_EQ((uint32_t)0x0123, avr);

  ASSERT(avr_device_resolve_address(&dev, 0x00800123, NULL, NULL, NULL));

  PASS();
}


TEST
null_safety(void)
{
  AvrDevice dev =
    make_328p_like();
  AvrMemoryRegion region;
  uint32_t out = 0xDEADBEEF;

  ASSERT(avr_device_name(NULL) == NULL);

  region = avr_device_flash_region(NULL);
  ASSERT_EQ((uint32_t)0, region.start);
  ASSERT_EQ((uint32_t)0, region.size);

  region = avr_device_sram_region(NULL);
  ASSERT_EQ((uint32_t)0, region.start);
  ASSERT_EQ((uint32_t)0, region.size);

  region = avr_device_eeprom_region(NULL);
  ASSERT_EQ((uint32_t)0, region.start);
  ASSERT_EQ((uint32_t)0, region.size);

  ASSERT_EQ((uint32_t)0, avr_device_data_region_origin(NULL));
  ASSERT_EQ((uint32_t)0, avr_device_data_region_length(NULL));
  ASSERT_EQ((uint32_t)0, avr_device_text_region_origin(NULL));
  ASSERT_EQ((uint32_t)0, avr_device_text_region_length(NULL));
  ASSERT_EQ((uint32_t)0, avr_device_eeprom_region_origin(NULL));
  ASSERT_EQ((uint32_t)0, avr_device_eeprom_region_length(NULL));
  ASSERT_EQ((uint32_t)0, avr_device_flash_address_shift(NULL));

  ASSERT_EQ(AVR_MEM_UNKNOWN, avr_device_classify_address(NULL, 0x00800123));

  ASSERT(!avr_device_to_physical_sram(NULL, 0x00800194, &out));
  ASSERT(!avr_device_to_physical_sram(&dev, 0x00800194, NULL));
  ASSERT(!avr_device_to_physical_flash(NULL, 0x0010, &out));
  ASSERT(!avr_device_to_physical_flash(&dev, 0x0010, NULL));
  ASSERT(!avr_device_to_physical_eeprom(NULL, 0x00810010, &out));
  ASSERT(!avr_device_to_physical_eeprom(&dev, 0x00810010, NULL));
  ASSERT(!avr_device_flash_byte_to_word(NULL, 0x2A12, &out));
  ASSERT(!avr_device_flash_byte_to_word(&dev, 0x2A12, NULL));

  ASSERT(avr_device_find_register(NULL, "PORTB") == NULL);
  ASSERT(avr_device_find_register(&dev, NULL) == NULL);
  ASSERT(avr_device_find_register(&dev, "PORTB") == NULL);
  ASSERT(avr_device_find_register_address(NULL, 0x25) == NULL);
  ASSERT(avr_device_find_register_address(&dev, 0x25) == NULL);

  PASS();
}


TEST
memory_space_names(void)
{
  ASSERT(strcmp(avr_memory_space_name(AVR_MEM_FLASH), "FLASH") == 0);
  ASSERT(strcmp(avr_memory_space_name(AVR_MEM_REGISTER), "REGISTER") == 0);
  ASSERT(strcmp(avr_memory_space_name(AVR_MEM_IO), "I/O") == 0);
  ASSERT(strcmp(avr_memory_space_name(AVR_MEM_SRAM), "SRAM") == 0);
  ASSERT(strcmp(avr_memory_space_name(AVR_MEM_EEPROM), "EEPROM") == 0);
  ASSERT(strcmp(avr_memory_space_name(AVR_MEM_UNKNOWN), "UNKNOWN") == 0);

  PASS();
}


TEST
text_eeprom_overlap_priority(void)
{
  AvrDevice dev =
    make_328p_like();

  /*
   * Classify order is TEXT first, then EEPROM, then DATA
   * (`avr_device_classify_address` checks text_space before
   * eeprom_space): an address in the TEXT/EEPROM overlap classifies
   * FLASH.
   */
  dev.eeprom_space.origin = 0x00000100;
  dev.eeprom_space.length = 0x0400;
  ASSERT_EQ(
    AVR_MEM_FLASH,
    avr_device_classify_address(&dev, 0x00000100));
  ASSERT_EQ(
    AVR_MEM_FLASH,
    avr_device_classify_address(&dev, 0x00000400));

  PASS();
}


TEST
zero_length_spaces(void)
{
  AvrDevice no_text =
    make_328p_like();
  AvrDevice no_sram =
    make_328p_like();

  /* Empty TEXT space matches nothing: VMA 0x0 falls through to UNKNOWN. */
  no_text.text_space.length = 0;
  ASSERT_EQ(
    AVR_MEM_UNKNOWN,
    avr_device_classify_address(&no_text, 0x00000000));

  /*
   * Observed: sram.size 0 with VMA 0x00800100 (physical 0x100, inside
   * DATA space but past register-file, I/O, and extended-I/O) matches
   * no subregion → UNKNOWN, not IO.
   */
  no_sram.sram.size = 0;
  ASSERT_EQ(
    AVR_MEM_UNKNOWN,
    avr_device_classify_address(&no_sram, 0x00800100));

  PASS();
}


TEST
flash_shift_zero(void)
{
  AvrDevice dev =
    make_328p_like();
  uint32_t word = 0;

  /* Shift 0 is valid (< 32): word address equals byte address. */
  dev.flash_address_shift = 0;
  ASSERT(avr_device_flash_byte_to_word(&dev, 0x2A12, &word));
  ASSERT_EQ((uint32_t)0x2A12, word);

  PASS();
}


TEST
sram_size_zero_resolve(void)
{
  AvrDevice dev =
    make_328p_like();
  AvrMemorySpace ms = AVR_MEM_UNKNOWN;
  uint32_t avr = 0;
  uint32_t phys = 0;

  /*
   * Observed: with sram.size 0, classify yields UNKNOWN (no subregion
   * matches), so resolve fails with *ms == UNKNOWN and zeroed outs.
   */
  dev.sram.size = 0;
  ms = AVR_MEM_FLASH;
  avr = 0xDEADBEEF;
  phys = 0xDEADBEEF;
  ASSERT(!avr_device_resolve_address(&dev, 0x00800100, &ms, &avr, &phys));
  ASSERT_EQ(AVR_MEM_UNKNOWN, ms);
  ASSERT_EQ((uint32_t)0, avr);
  ASSERT_EQ((uint32_t)0, phys);

  PASS();
}


SUITE(device)
{
  RUN_TEST(classify_boundaries);
  RUN_TEST(classify_priority);
  RUN_TEST(text_eeprom_overlap_priority);
  RUN_TEST(zero_length_spaces);
  RUN_TEST(to_physical);
  RUN_TEST(flash_byte_to_word);
  RUN_TEST(flash_shift_zero);
  RUN_TEST(resolve_address);
  RUN_TEST(sram_size_zero_resolve);
  RUN_TEST(null_safety);
  RUN_TEST(memory_space_names);
}


int
main(int argc, char *argv[])
{
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(device);
  GREATEST_MAIN_END();
}
