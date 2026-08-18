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
 * @file avr_device.c
 * @author notweerdmonk, gpt-5.6-luna
 * @brief AVR device abstraction and device-specific address translation.
 *
 * @details
 * This module converts the information discovered by
 * @ref avr_device_probe into the public @ref AvrDevice representation and
 * performs device-specific interpretation of ELF/linker addresses.
 *
 * Responsibilities include:
 *
 * - owning the discovered device description;
 * - exposing physical FLASH, SRAM, and EEPROM regions;
 * - exposing ELF/linker address spaces;
 * - classifying physical AVR DATA-space subregions;
 * - translating ELF/linker addresses to physical addresses;
 * - exposing the discovered SFR database;
 * - converting physical FLASH byte addresses to AVR CPU word addresses.
 *
 * No MCU-specific memory table belongs in this module.  Device-specific
 * values are obtained through the toolchain probing layer.
 */

#include "avr_device.h"

#include <stdlib.h>
#include <string.h>


/**
 * @brief Test whether an address belongs to a half-open address range.
 *
 * @param[in] address
 *   Address to test.
 *
 * @param[in] origin
 *   First address in the range.
 *
 * @param[in] length
 *   Number of addresses in the range.
 *
 * @return
 *   `true` when:
 *
 * @code
 * origin <= address < origin + length
 * @endcode
 *
 *   `false` otherwise.
 *
 * @details
 * The subtraction-based implementation avoids computing
 * `origin + length` directly and therefore avoids overflow in that
 * intermediate expression.
 *
 * An empty range (`length == 0`) never contains an address.
 */
static bool
address_in_region(
  uint32_t address,
  uint32_t origin,
  uint32_t length)
{
  if (length == 0)
    return false;

  if (address < origin)
    return false;

  return (uint32_t)(address - origin) < length;
}


/**
 * @brief Initialize an @ref AvrDevice from toolchain-discovered data.
 *
 * @param[out] device
 *   Device object to initialize.
 *
 * @param[in] device_name
 *   AVR MCU name accepted by:
 *
 * @code
 * avr-gcc -mmcu=<device>
 * @endcode
 *
 * @return
 *   `true` when device probing and initialization succeed; `false`
 *   otherwise.
 *
 * @details
 * The function delegates device discovery to @ref avr_device_probe and
 * transfers ownership of dynamically allocated probe resources into
 * the resulting @ref AvrDevice.
 *
 * The initialized device contains:
 *
 * - physical FLASH, SRAM, and EEPROM regions;
 * - ELF/linker DATA, TEXT, and EEPROM address spaces;
 * - physical register-file, I/O, and extended-I/O subregions;
 * - the discovered SFR database;
 * - the FLASH byte-to-CPU-word address shift.
 *
 * No MCU-specific device initialization is implemented here.
 */
bool
avr_device_init(
  AvrDevice *device,
  const char *device_name)
{
  AvrDeviceProbeResult probe;

  if (device == NULL ||
      device_name == NULL)
  {
    return false;
  }

  memset(
    device,
    0,
    sizeof(*device)
  );

  memset(
    &probe,
    0,
    sizeof(probe)
  );

  /*
   * All device-specific information comes from the probe layer.
   */
  if (!avr_device_probe(
        device_name,
        &probe))
  {
    return false;
  }

  /*
   * --------------------------------------------------------------
   * Device identity
   * --------------------------------------------------------------
   *
   * Transfer ownership of the allocated device name from the probe
   * result to AvrDevice.
   */
  device->name =
    probe.name;

  probe.name = NULL;


  /*
   * --------------------------------------------------------------
   * Physical memory regions
   * --------------------------------------------------------------
   */

  device->flash.start =
    probe.flash_start;

  device->flash.size =
    probe.flash_size;

  device->sram.start =
    probe.sram_start;

  device->sram.size =
    probe.sram_size;

  device->eeprom.start =
    probe.eeprom_start;

  device->eeprom.size =
    probe.eeprom_size;


  /*
   * --------------------------------------------------------------
   * ELF/linker address spaces
   * --------------------------------------------------------------
   *
   * These are deliberately separate from physical memory regions.
   *
   * For example:
   *
   *     physical SRAM:
   *         0x0100
   *
   *     ELF/linker DATA:
   *         0x00800100
   */
  device->data_space.origin =
    probe.data_region_origin;

  device->data_space.length =
    probe.data_region_length;

  device->text_space.origin =
    probe.text_region_origin;

  device->text_space.length =
    probe.text_region_length;

  device->eeprom_space.origin =
    probe.eeprom_region_origin;

  device->eeprom_space.length =
    probe.eeprom_region_length;


  /*
   * --------------------------------------------------------------
   * AVR DATA-space subregions
   * --------------------------------------------------------------
   *
   * The initial model describes the conventional AVR DATA-space
   * layout:
   *
   *     0x0000 - 0x001F    CPU register file
   *     0x0020 - 0x005F    I/O space
   *     0x0060 - ...       extended I/O
   *     RAMSTART - ...     SRAM
   *
   * SFR names and their exact register definitions are obtained
   * separately from avr-libc through AvrSfrDatabase.
   *
   * These architectural boundaries are used only to classify
   * physical DATA space; register names are not hardcoded here.
   */

  device->register_file.start = 0x0000;
  device->register_file.size = 0x0020;

  device->io.start = 0x0020;
  device->io.size = 0x0040;

  device->ext_io.start = 0x0060;

  if (device->sram.start > device->ext_io.start) {
    device->ext_io.size =
      device->sram.start -
      device->ext_io.start;
  }
  else {
    device->ext_io.size = 0;
  }


  /*
   * --------------------------------------------------------------
   * FLASH address representation
   * --------------------------------------------------------------
   */

  device->flash_address_shift =
    probe.flash_address_shift;


  /*
   * --------------------------------------------------------------
   * SFR database
   * --------------------------------------------------------------
   *
   * Ownership is transferred from the probe result to AvrDevice.
   */
  device->sfr_database =
    probe.sfr_database;

  probe.sfr_database = NULL;


  /*
   * The probe object no longer owns any resources transferred above.
   * Destroying it is therefore safe.
   */
  avr_device_probe_destroy(
    &probe
  );

  return true;
}


/**
 * @brief Destroy an AVR device description.
 *
 * @param[in,out] device
 *   Device object whose owned resources are to be released.
 *
 * @details
 * Releases:
 *
 * - the dynamically allocated device name;
 * - the discovered SFR database;
 *
 * and resets the complete structure to zero.
 *
 * @note
 * Passing `NULL` is safe.
 */
void
avr_device_destroy(
  AvrDevice *device)
{
  if (device == NULL)
    return;

  free(device->name);

  avr_sfr_destroy(
    device->sfr_database
  );

  memset(
    device,
    0,
    sizeof(*device)
  );
}


/**
 * @brief Return the canonical AVR device name.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   Pointer to the device name owned by @p device, or `NULL` when
 *   @p device is `NULL`.
 *
 * @warning
 * The returned string must not be freed or modified by the caller.
 *
 * @note
 * The returned pointer becomes invalid after @ref avr_device_destroy.
 */
const char *
avr_device_name(
  const AvrDevice *device)
{
  if (device == NULL)
    return NULL;

  return device->name;
}


/**
 * @brief Return the physical FLASH memory region.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   Physical FLASH region, or an empty region when @p device is `NULL`.
 */
AvrMemoryRegion
avr_device_flash_region(
  const AvrDevice *device)
{
  if (device == NULL)
    return (AvrMemoryRegion){ 0, 0 };

  return device->flash;
}


/**
 * @brief Return the physical SRAM memory region.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   Physical SRAM region, or an empty region when @p device is `NULL`.
 */
AvrMemoryRegion
avr_device_sram_region(
  const AvrDevice *device)
{
  if (device == NULL)
    return (AvrMemoryRegion){ 0, 0 };

  return device->sram;
}


/**
 * @brief Return the physical EEPROM memory region.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   Physical EEPROM region, or an empty region when @p device is `NULL`.
 */
AvrMemoryRegion
avr_device_eeprom_region(
  const AvrDevice *device)
{
  if (device == NULL)
    return (AvrMemoryRegion){ 0, 0 };

  return device->eeprom;
}


/**
 * @brief Return the origin of the AVR-GCC DATA linker space.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   DATA linker-space origin, or `0` when @p device is `NULL`.
 */
uint32_t
avr_device_data_region_origin(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->data_space.origin;
}


/**
 * @brief Return the length of the AVR-GCC DATA linker space.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   DATA linker-space length, or `0` when @p device is `NULL`.
 */
uint32_t
avr_device_data_region_length(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->data_space.length;
}


/**
 * @brief Return the origin of the AVR-GCC TEXT linker space.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   TEXT linker-space origin, or `0` when @p device is `NULL`.
 */
uint32_t
avr_device_text_region_origin(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->text_space.origin;
}


/**
 * @brief Return the length of the AVR-GCC TEXT linker space.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   TEXT linker-space length, or `0` when @p device is `NULL`.
 */
uint32_t
avr_device_text_region_length(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->text_space.length;
}


/**
 * @brief Return the origin of the AVR-GCC EEPROM linker space.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   EEPROM linker-space origin, or `0` when @p device is `NULL`.
 */
uint32_t
avr_device_eeprom_region_origin(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->eeprom_space.origin;
}


/**
 * @brief Return the length of the AVR-GCC EEPROM linker space.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   EEPROM linker-space length, or `0` when @p device is `NULL`.
 */
uint32_t
avr_device_eeprom_region_length(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->eeprom_space.length;
}


/**
 * @brief Return the FLASH byte-to-CPU-word address shift.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   Device-specific shift value, or `0` when @p device is `NULL`.
 *
 * @details
 * The returned value is intended for:
 *
 * @code
 * word_address = byte_address >> shift;
 * @endcode
 */
uint32_t
avr_device_flash_address_shift(
  const AvrDevice *device)
{
  if (device == NULL)
    return 0;

  return device->flash_address_shift;
}


/**
 * @brief Find an SFR by its symbolic name.
 *
 * @param[in] device
 *   Device containing the SFR database.
 *
 * @param[in] name
 *   NUL-terminated symbolic register name.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no register
 *   matches.
 *
 * @warning
 * The returned object is owned by @ref AvrDevice and must not be freed
 * or modified by the caller.
 *
 * @note
 * The returned pointer becomes invalid after @ref avr_device_destroy.
 */
const AvrSfr *
avr_device_find_register(
  const AvrDevice *device,
  const char *name)
{
  if (device == NULL ||
      name == NULL)
  {
    return NULL;
  }

  return avr_sfr_find(
    device->sfr_database,
    name
  );
}


/**
 * @brief Find an SFR by canonical physical AVR DATA-space address.
 *
 * @param[in] device
 *   Device containing the SFR database.
 *
 * @param[in] data_address
 *   Canonical physical AVR DATA-space address.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no register
 *   is defined at that address.
 *
 * @warning
 * The returned object is owned by @ref AvrDevice and must not be freed
 * or modified by the caller.
 */
const AvrSfr *
avr_device_find_register_address(
  const AvrDevice *device,
  uint16_t data_address)
{
  if (device == NULL)
    return NULL;

  return avr_sfr_find_address(
    device->sfr_database,
    data_address
  );
}


/**
 * @brief Classify an ELF/linker address for the selected AVR device.
 *
 * @param[in] device
 *   Device description used for address interpretation.
 *
 * @param[in] address
 *   ELF/linker address to classify.
 *
 * @return
 *   The resulting @ref AvrMemorySpace, or @ref AVR_MEM_UNKNOWN when
 *   the address cannot be interpreted.
 *
 * @details
 * Classification proceeds in linker-address space first:
 *
 * @code
 * TEXT       -> FLASH
 * EEPROM     -> EEPROM
 * DATA       -> physical DATA-space subregion
 * @endcode
 *
 * DATA-space addresses are then translated relative to
 * @ref AvrDevice::data_space and classified as:
 *
 * - CPU register file;
 * - I/O;
 * - extended I/O;
 * - SRAM.
 *
 * The physical FLASH and EEPROM sizes are also checked so that a linker
 * region extending beyond the actual physical device is not silently
 * classified as valid memory.
 */
AvrMemorySpace
avr_device_classify_address(
  const AvrDevice *device,
  uint32_t address)
{
  uint32_t physical_data_address;

  if (device == NULL)
    return AVR_MEM_UNKNOWN;


  /*
   * --------------------------------------------------------------
   * FLASH
   * --------------------------------------------------------------
   */
  if (address_in_region(
        address,
        device->text_space.origin,
        device->text_space.length))
  {
    /*
     * Also ensure that the linker text region does not describe
     * bytes beyond the physical flash device.
     */
    uint32_t offset =
      address -
      device->text_space.origin;

    if (offset < device->flash.size)
      return AVR_MEM_FLASH;

    return AVR_MEM_UNKNOWN;
  }


  /*
   * --------------------------------------------------------------
   * EEPROM
   * --------------------------------------------------------------
   */
  if (address_in_region(
        address,
        device->eeprom_space.origin,
        device->eeprom_space.length))
  {
    uint32_t offset =
      address -
      device->eeprom_space.origin;

    if (offset < device->eeprom.size)
      return AVR_MEM_EEPROM;

    return AVR_MEM_UNKNOWN;
  }


  /*
   * --------------------------------------------------------------
   * DATA
   * --------------------------------------------------------------
   */
  if (!address_in_region(
        address,
        device->data_space.origin,
        device->data_space.length))
  {
    return AVR_MEM_UNKNOWN;
  }

  physical_data_address =
    address -
    device->data_space.origin;


  /*
   * CPU register file.
   */
  if (address_in_region(
        physical_data_address,
        device->register_file.start,
        device->register_file.size))
  {
    return AVR_MEM_REGISTER;
  }


  /*
   * I/O space.
   */
  if (address_in_region(
        physical_data_address,
        device->io.start,
        device->io.size))
  {
    return AVR_MEM_IO;
  }


  /*
   * Extended I/O space.
   */
  if (address_in_region(
        physical_data_address,
        device->ext_io.start,
        device->ext_io.size))
  {
    return AVR_MEM_IO;
  }


  /*
   * SRAM.
   */
  if (address_in_region(
        physical_data_address,
        device->sram.start,
        device->sram.size))
  {
    return AVR_MEM_SRAM;
  }


  return AVR_MEM_UNKNOWN;
}


/**
 * @brief Convert an AVR-GCC DATA-space address to physical SRAM.
 *
 * @param[in] device
 *   Device description used for translation.
 *
 * @param[in] avr_address
 *   ELF/linker DATA-space address.
 *
 * @param[out] physical_address
 *   Receives the physical SRAM address.
 *
 * @return
 *   `true` when the address is inside the device's linker DATA space
 *   and corresponds to physical SRAM; `false` otherwise.
 *
 * @details
 * Translation proceeds as:
 *
 * @code
 * offset =
 *   avr_address - data_space.origin
 *
 * physical address =
 *   sram.start + (offset - sram.start)
 * @endcode
 *
 * For the conventional AVR DATA-space layout the latter expression
 * reduces to the DATA-space offset itself, but the implementation
 * preserves the physical SRAM origin explicitly.
 */
bool
avr_device_to_physical_sram(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address)
{
  uint32_t offset;

  if (device == NULL ||
      physical_address == NULL)
  {
    return false;
  }

  if (!address_in_region(
        avr_address,
        device->data_space.origin,
        device->data_space.length))
  {
    return false;
  }

  offset =
    avr_address -
    device->data_space.origin;


  /*
   * The DATA-space offset must lie within the physical SRAM region.
   */
  if (offset < device->sram.start)
    return false;

  if ((uint32_t)(offset - device->sram.start) >=
      device->sram.size)
  {
    return false;
  }

  /*
   * Convert:
   *
   *     linker DATA address
   *         -
   *     linker DATA origin
   *         +
   *     physical SRAM start
   *
   * Example:
   *
   *     0x00800194
   *       - 0x00800000
   *       = 0x0194
   */
  *physical_address =
    device->sram.start +
    (offset - device->sram.start);

  return true;
}


/**
 * @brief Convert an AVR-GCC TEXT-space address to physical FLASH.
 *
 * @param[in] device
 *   Device description used for translation.
 *
 * @param[in] avr_address
 *   ELF/linker TEXT-space address.
 *
 * @param[out] physical_address
 *   Receives the physical FLASH byte address.
 *
 * @return
 *   `true` when the address is inside the linker TEXT space and within
 *   physical FLASH; `false` otherwise.
 *
 * @details
 * A potentially non-zero physical FLASH origin is preserved:
 *
 * @code
 * physical =
 *   device->flash.start +
 *   (avr_address - text_space.origin)
 * @endcode
 */
bool
avr_device_to_physical_flash(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address)
{
  uint32_t offset;

  if (device == NULL ||
      physical_address == NULL)
  {
    return false;
  }

  if (!address_in_region(
        avr_address,
        device->text_space.origin,
        device->text_space.length))
  {
    return false;
  }

  offset =
    avr_address -
    device->text_space.origin;

  if (offset >= device->flash.size)
    return false;

  /*
   * Preserve a potentially non-zero physical flash origin.
   */
  *physical_address =
    device->flash.start +
    offset;

  return true;
}


/**
 * @brief Convert a physical FLASH byte address to an AVR CPU word address.
 *
 * @param[in] device
 *   Device description specifying the address shift.
 *
 * @param[in] byte_address
 *   Physical FLASH byte address.
 *
 * @param[out] word_address
 *   Receives the corresponding CPU instruction/word address.
 *
 * @return
 *   `true` when @p byte_address is within physical FLASH and the shift is
 *   valid; `false` otherwise.
 *
 * @details
 * The function accepts arbitrary byte addresses.
 *
 * For a conventional AVR:
 *
 * @code
 * word_address = byte_address >> 1
 * @endcode
 *
 * An odd byte address therefore maps to the instruction word containing
 * that byte.  This function does not impose an instruction-alignment
 * requirement.
 */
bool
avr_device_flash_byte_to_word(
  const AvrDevice *device,
  uint32_t byte_address,
  uint32_t *word_address)
{
  if (device == NULL ||
      word_address == NULL)
  {
    return false;
  }

  if (device->flash_address_shift >= 32)
    return false;

  if (!address_in_region(
        byte_address,
        device->flash.start,
        device->flash.size))
  {
    return false;
  }

  /*
   * The input is an arbitrary physical FLASH byte address.
   */
  *word_address =
    byte_address >>
    device->flash_address_shift;

  return true;
}


/**
 * @brief Convert an AVR-GCC EEPROM-space address to physical EEPROM.
 *
 * @param[in] device
 *   Device description used for translation.
 *
 * @param[in] avr_address
 *   ELF/linker EEPROM-space address.
 *
 * @param[out] physical_address
 *   Receives the physical EEPROM address.
 *
 * @return
 *   `true` when the address is inside the linker EEPROM space and the
 *   resulting offset is inside physical EEPROM; `false` otherwise.
 */
bool
avr_device_to_physical_eeprom(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address)
{
  uint32_t offset;

  if (device == NULL ||
      physical_address == NULL)
  {
    return false;
  }

  if (!address_in_region(
        avr_address,
        device->eeprom_space.origin,
        device->eeprom_space.length))
  {
    return false;
  }

  offset =
    avr_address -
    device->eeprom_space.origin;

  if (offset >= device->eeprom.size)
    return false;

  *physical_address =
    device->eeprom.start +
    offset;

  return true;
}


/**
 * @brief Resolve an ELF/linker address into its device representation.
 *
 * @param[in] device
 *   Device description used for address interpretation.
 *
 * @param[in] address
 *   ELF/linker address.
 *
 * @param[out] memory_space
 *   Receives the resolved @ref AvrMemorySpace.
 *   May be `NULL`.
 *
 * @param[out] avr_address
 *   Receives the physical AVR address within the classified memory
 *   space.  May be `NULL`.
 *
 * @param[out] physical_address
 *   Receives the physical target-memory address.
 *   May be `NULL`.
 *
 * @return
 *   `true` when the address is understood by the selected device;
 *   `false` otherwise.
 *
 * @details
 * The function first calls @ref avr_device_classify_address and then
 * delegates the actual translation to the corresponding conversion
 * function.
 *
 * For REGISTER and I/O addresses, the resolved DATA-space offset is
 * already the physical AVR address, so no additional physical mapping
 * is required.
 */
bool
avr_device_resolve_address(
  const AvrDevice *device,
  uint32_t address,
  AvrMemorySpace *memory_space,
  uint32_t *avr_address,
  uint32_t *physical_address)
{
  AvrMemorySpace space;

  if (memory_space != NULL)
    *memory_space =
      AVR_MEM_UNKNOWN;

  if (avr_address != NULL)
    *avr_address = 0;

  if (physical_address != NULL)
    *physical_address = 0;

  if (device == NULL)
    return false;

  space =
    avr_device_classify_address(
      device,
      address
    );

  if (memory_space != NULL)
    *memory_space = space;

  switch (space) {

    case AVR_MEM_FLASH:
      if (!avr_device_to_physical_flash(
            device,
            address,
            &address))
      {
        return false;
      }

      if (avr_address != NULL)
        *avr_address = address;

      if (physical_address != NULL)
        *physical_address = address;

      return true;


    case AVR_MEM_SRAM:
      if (!avr_device_to_physical_sram(
            device,
            address,
            &address))
      {
        return false;
      }

      if (avr_address != NULL)
        *avr_address = address;

      if (physical_address != NULL)
        *physical_address = address;

      return true;


    case AVR_MEM_EEPROM:
      if (!avr_device_to_physical_eeprom(
            device,
            address,
            &address))
      {
        return false;
      }

      if (avr_address != NULL)
        *avr_address = address;

      if (physical_address != NULL)
        *physical_address = address;

      return true;


    case AVR_MEM_REGISTER:
    case AVR_MEM_IO:
      /*
       * These are already physical DATA-space addresses.
       */
      address -=
        device->data_space.origin;

      if (avr_address != NULL)
        *avr_address = address;

      if (physical_address != NULL)
        *physical_address = address;

      return true;


    case AVR_MEM_UNKNOWN:
    default:
      return false;
  }
}


/**
 * @brief Return a printable name for an AVR memory-space classification.
 *
 * @param[in] space
 *   Memory-space enumeration value.
 *
 * @return
 *   Constant human-readable name suitable for diagnostic or
 *   user-facing output.
 */
const char *
avr_memory_space_name(
  AvrMemorySpace space)
{
  switch (space) {

    case AVR_MEM_FLASH:
      return "FLASH";

    case AVR_MEM_REGISTER:
      return "REGISTER";

    case AVR_MEM_IO:
      return "I/O";

    case AVR_MEM_SRAM:
      return "SRAM";

    case AVR_MEM_EEPROM:
      return "EEPROM";

    case AVR_MEM_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}
