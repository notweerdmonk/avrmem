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
 * @file avr_device.h
 * @author notweerdmonk, gpt-5.6-luna
 * @brief AVR device description and address-space abstraction.
 *
 * @details
 * The public device model deliberately separates:
 *
 * - physical target memories,
 * - ELF/linker address spaces,
 * - physical AVR DATA-space subregions,
 * - the discovered Special Function Register (SFR) database.
 *
 * Device-specific values are obtained through the AVR toolchain probing
 * interface exposed by @ref avr_device_probe, rather than through an
 * MCU-specific table embedded in this module.
 *
 * The device abstraction is responsible for interpreting ELF/linker
 * addresses for a selected AVR target.  The ELF parser therefore does not
 * need to know how a particular MCU represents SRAM, I/O, EEPROM, or SFRs.
 */

#ifndef AVR_DEVICE_H
#define AVR_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "avr_device_probe.h"
#include "avr_sfr.h"


/**
 * @enum AvrMemorySpace
 * @brief Classification of an AVR address after device-specific resolution.
 *
 * @details
 * The value describes the physical AVR memory or address-space category
 * to which an ELF/linker address resolves.
 */
typedef enum {
  /** Address could not be classified. */
  AVR_MEM_UNKNOWN = 0,

  /** Program memory / FLASH. */
  AVR_MEM_FLASH,

  /** CPU register file at the bottom of AVR DATA space. */
  AVR_MEM_REGISTER,

  /**
   * AVR I/O and extended-I/O registers.
   *
   * @note
   * The public classification intentionally groups conventional I/O
   * and extended-I/O into the same category.  Their physical regions
   * remain separately available through @ref AvrDevice::io and
   * @ref AvrDevice::ext_io.
   */
  AVR_MEM_IO,

  /**
   * Physical SRAM.
   *
   * @details
   * This includes runtime storage used by sections such as `.data`,
   * `.bss`, `.noinit`, the heap, and the stack.
   */
  AVR_MEM_SRAM,

  /** Non-volatile EEPROM memory. */
  AVR_MEM_EEPROM

} AvrMemorySpace;


/**
 * @struct AvrMemoryRegion
 * @brief Describes a contiguous physical AVR memory region.
 *
 * @details
 * The region is expressed in physical AVR address space, independent
 * of the ELF/linker address representation.
 */
typedef struct {
  /**
   * Physical address of the first byte in the region.
   */
  uint32_t start;

  /**
   * Number of bytes contained in the region.
   */
  uint32_t size;

} AvrMemoryRegion;


/**
 * @struct AvrAddressSpace
 * @brief Describes an ELF/linker address space.
 *
 * @details
 * This type is deliberately distinct from @ref AvrMemoryRegion.
 *
 * An ELF/linker address-space region describes addresses as they appear
 * in the executable, while a physical memory region describes actual
 * target-memory addresses.
 *
 * For example, an AVR DATA-space address may be represented as:
 *
 * @code
 * ELF address:
 *     0x00800100
 *
 * Physical AVR address:
 *     0x00000100
 * @endcode
 *
 * The conversion between the two representations is owned by
 * @ref AvrDevice.
 */
typedef struct {
  /**
   * ELF/linker address at which the region begins.
   */
  uint32_t origin;

  /**
   * Number of bytes represented by the linker address space.
   */
  uint32_t length;

} AvrAddressSpace;


/**
 * @struct AvrDevice
 * @brief Complete description of one selected AVR device.
 *
 * @details
 * An @ref AvrDevice contains all device-specific information required
 * to interpret ELF/linker addresses for the selected MCU.
 *
 * The structure separates:
 *
 * - physical memories,
 * - ELF/linker address spaces,
 * - physical AVR DATA-space subregions,
 * - the discovered SFR database,
 * - FLASH byte-to-CPU-word addressing information.
 *
 * The object is populated by @ref avr_device_init and releases all
 * owned resources through @ref avr_device_destroy.
 */
typedef struct {

  /**
   * Canonical AVR device name.
   *
   * @details
   * This string is owned by the @ref AvrDevice after successful
   * initialization.
   *
   * Example:
   *
   * @code
   * "atmega328p"
   * @endcode
   */
  char *name;


  /**
   * @name Physical memories
   * @{
   */

  /**
   * Physical program FLASH region.
   *
   * @details
   * Addresses are physical byte addresses.
   */
  AvrMemoryRegion flash;

  /**
   * Physical SRAM region.
   *
   * @details
   * Runtime `.data`, `.bss`, `.noinit`, heap, and stack storage reside
   * in this physical region.
   */
  AvrMemoryRegion sram;

  /**
   * Physical EEPROM region.
   */
  AvrMemoryRegion eeprom;

  /** @} */


  /**
   * @name ELF/linker address spaces
   * @{
   */

  /**
   * AVR-GCC DATA linker space.
   *
   * @details
   * This address space may cover more than physical SRAM because AVR
   * register and I/O regions can be represented as part of the linker
   * DATA space.
   */
  AvrAddressSpace data_space;

  /**
   * AVR-GCC program/TEXT linker space.
   */
  AvrAddressSpace text_space;

  /**
   * AVR-GCC EEPROM linker space.
   */
  AvrAddressSpace eeprom_space;

  /** @} */


  /**
   * @name Physical AVR DATA-space subregions
   * @{
   */

  /**
   * Physical CPU register-file region.
   *
   * @details
   * This is the register-file portion at the bottom of AVR DATA space.
   */
  AvrMemoryRegion register_file;

  /**
   * Physical conventional AVR I/O region.
   */
  AvrMemoryRegion io;

  /**
   * Physical extended-I/O region.
   */
  AvrMemoryRegion ext_io;

  /** @} */


  /**
   * @name Special Function Register database
   * @{
   */

  /**
   * Discovered SFR database.
   *
   * @details
   * The database is populated from the selected avr-libc device header.
   *
   * Ownership belongs to the @ref AvrDevice after successful
   * initialization.
   */
  AvrSfrDatabase *sfr_database;

  /** @} */


  /**
   * @name Program-memory address representation
   * @{
   */

  /**
   * Number of bits by which a physical FLASH byte address is shifted
   * to obtain the AVR CPU instruction/word address.
   *
   * @details
   * For conventional classic AVR:
   *
   * @code
   * word = byte >> 1
   * @endcode
   *
   * Therefore:
   *
   * @code
   * flash_address_shift == 1
   * @endcode
   */
  unsigned flash_address_shift;

  /** @} */

} AvrDevice;


/**
 * @brief Initialize an AVR device description.
 *
 * @param[out] device
 *   Device object to initialize.
 *
 * @param[in] device_name
 *   MCU name accepted by:
 *   @code
 *   avr-gcc -mmcu=<device>
 *   @endcode
 *
 *   Example:
 *   @code
 *   "atmega328p"
 *   @endcode
 *
 * @return
 *   `true` when the selected device was successfully discovered and
 *   initialized; `false` otherwise.
 *
 * @note
 *   Device-specific initialization must be performed through the
 *   toolchain probe.  MCU-specific initialization functions do not
 *   belong in this module.
 */
bool
avr_device_init(
  AvrDevice *device,
  const char *device_name
);


/**
 * @brief Release all resources owned by an AVR device description.
 *
 * @param[in,out] device
 *   Device object whose resources should be released.
 *
 * @note
 *   It is safe to pass `NULL`.
 */
void
avr_device_destroy(
  AvrDevice *device
);


/**
 * @brief Return the canonical device name.
 *
 * @param[in] device
 *   Device description.
 *
 * @return
 *   Pointer to the device name owned by @p device, or `NULL` when
 *   @p device is `NULL`.
 *
 * @warning
 *   The returned string must not be freed or modified by the caller.
 *
 * @note
 *   The returned pointer becomes invalid after @ref avr_device_destroy.
 */
const char *
avr_device_name(
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
  const AvrDevice *device
);


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
 * For a conventional AVR with byte-addressed FLASH and word-addressed
 * CPU instructions:
 *
 * @code
 * word_address = byte_address >> 1
 * @endcode
 */
uint32_t
avr_device_flash_address_shift(
  const AvrDevice *device
);


/**
 * @brief Classify an ELF/linker address according to the selected device.
 *
 * @param[in] device
 *   Device description used for address interpretation.
 *
 * @param[in] address
 *   Address expressed in ELF/linker address space.
 *
 * @return
 *   The resolved @ref AvrMemorySpace, or
 *   @ref AVR_MEM_UNKNOWN when the address cannot be classified.
 *
 * @details
 * Example:
 *
 * @code
 * ELF address:
 *     0x00800123
 *
 * DATA linker space:
 *     0x00800100 ...
 *
 * physical AVR DATA address:
 *     0x0123
 *
 * memory space:
 *     AVR_MEM_SRAM
 * @endcode
 */
AvrMemorySpace
avr_device_classify_address(
  const AvrDevice *device,
  uint32_t address
);


/**
 * @brief Convert an AVR-GCC DATA-space ELF address to physical SRAM.
 *
 * @param[in] device
 *   Device description used for address translation.
 *
 * @param[in] avr_address
 *   ELF/linker DATA-space address.
 *
 * @param[out] physical_address
 *   Receives the corresponding physical SRAM address.
 *
 * @return
 *   `true` if the address belongs to the device's linker DATA space and
 *   resolves to physical SRAM; `false` otherwise.
 *
 * @details
 * Example:
 *
 * @code
 * ELF address:
 *     0x00800123
 *
 * physical SRAM:
 *     0x0123
 * @endcode
 */
bool
avr_device_to_physical_sram(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address
);


/**
 * @brief Convert an AVR-GCC TEXT-space ELF address to physical FLASH.
 *
 * @param[in] device
 *   Device description used for address translation.
 *
 * @param[in] avr_address
 *   ELF/linker TEXT-space address.
 *
 * @param[out] physical_address
 *   Receives the corresponding physical FLASH byte address.
 *
 * @return
 *   `true` if the address resolves to physical FLASH; `false` otherwise.
 */
bool
avr_device_to_physical_flash(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address
);


/**
 * @brief Convert a physical FLASH byte address to a CPU instruction
 * word address.
 *
 * @param[in] device
 *   Device description specifying the addressing shift.
 *
 * @param[in] byte_address
 *   Physical FLASH byte address.
 *
 * @param[out] word_address
 *   Receives the corresponding AVR CPU instruction/word address.
 *
 * @return
 *   `true` if @p byte_address belongs to the device's physical FLASH
 *   region; `false` otherwise.
 *
 * @note
 *   The input is deliberately a physical FLASH address, not an
 *   ELF/linker address.  Use @ref avr_device_to_physical_flash when
 *   starting from an ELF symbol or section address.
 *
 * @details
 * For a conventional AVR:
 *
 * @code
 * 0x2A12 -> 0x1509
 * @endcode
 */
bool
avr_device_flash_byte_to_word(
  const AvrDevice *device,
  uint32_t byte_address,
  uint32_t *word_address
);


/**
 * @brief Convert an AVR-GCC EEPROM-space ELF address to physical EEPROM.
 *
 * @param[in] device
 *   Device description used for address translation.
 *
 * @param[in] avr_address
 *   ELF/linker EEPROM-space address.
 *
 * @param[out] physical_address
 *   Receives the corresponding physical EEPROM address.
 *
 * @return
 *   `true` if the address resolves to physical EEPROM; `false` otherwise.
 */
bool
avr_device_to_physical_eeprom(
  const AvrDevice *device,
  uint32_t avr_address,
  uint32_t *physical_address
);


/**
 * @brief Resolve an ELF/linker address in one operation.
 *
 * @param[in] device
 *   Device description used for address interpretation.
 *
 * @param[in] address
 *   ELF/linker address to resolve.
 *
 * @param[out] memory_space
 *   Receives the resolved @ref AvrMemorySpace.
 *   May be `NULL` when classification is not required.
 *
 * @param[out] avr_address
 *   Receives the physical AVR address corresponding to the classified
 *   memory space.  May be `NULL` when the caller does not require it.
 *
 * @param[out] physical_address
 *   Receives the corresponding physical memory address.
 *   May be `NULL` when the caller does not require it.
 *
 * @return
 *   `true` when the address is understood by the selected device;
 *   `false` when the address cannot be classified or translated.
 *
 * @details
 * Example:
 *
 * @code
 * ELF DATA address:
 *     0x00800123
 *
 * memory space:
 *     AVR_MEM_SRAM
 *
 * avr_address:
 *     0x0123
 *
 * physical_address:
 *     0x0123
 * @endcode
 *
 * @note
 *   The meaning of @p avr_address is the physical AVR address within
 *   the classified memory space, not the original ELF/linker address.
 */
bool
avr_device_resolve_address(
  const AvrDevice *device,
  uint32_t address,
  AvrMemorySpace *memory_space,
  uint32_t *avr_address,
  uint32_t *physical_address
);


/**
 * @brief Find an SFR by symbolic name.
 *
 * @param[in] device
 *   Device description containing the discovered SFR database.
 *
 * @param[in] name
 *   Symbolic SFR name, for example `"PORTB"`.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no matching
 *   register is found.
 *
 * @warning
 *   The returned object is owned by @ref AvrDevice and must not be
 *   freed or modified by the caller.
 *
 * @note
 *   The returned pointer becomes invalid after @ref avr_device_destroy.
 *
 * @code
 * const AvrSfr *portb =
 *   avr_device_find_register(
 *     device,
 *     "PORTB"
 *   );
 * @endcode
 */
const AvrSfr *
avr_device_find_register(
  const AvrDevice *device,
  const char *name
);


/**
 * @brief Find an SFR by canonical physical AVR DATA-space address.
 *
 * @param[in] device
 *   Device description containing the discovered SFR database.
 *
 * @param[in] data_address
 *   Canonical physical AVR DATA-space address.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no matching
 *   register is found.
 *
 * @warning
 *   The returned object is owned by @ref AvrDevice and must not be
 *   freed or modified by the caller.
 *
 * @note
 *   The returned pointer becomes invalid after @ref avr_device_destroy.
 *
 * @code
 * const AvrSfr *portb =
 *   avr_device_find_register_address(
 *     device,
 *     0x25
 *   );
 * @endcode
 */
const AvrSfr *
avr_device_find_register_address(
  const AvrDevice *device,
  uint16_t data_address
);


/**
 * @brief Return a printable name for an AVR memory-space classification.
 *
 * @param[in] space
 *   Memory-space enumeration value.
 *
 * @return
 *   A constant string suitable for diagnostic or human-readable output.
 */
const char *
avr_memory_space_name(
  AvrMemorySpace space
);


#endif /* AVR_DEVICE_H */
