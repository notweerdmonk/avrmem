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
 * @file avr_device_probe.h
 * @author notweerdmonk, gpt-5.6-luna
 * @brief Internal interface for discovering AVR device information.
 *
 * @details
 * This interface provides the toolchain-discovery layer used to construct
 * an @ref AvrDevice.
 *
 * Device information is obtained from the installed AVR-GCC/avr-libc
 * toolchain rather than from MCU-specific constants embedded in the
 * application's source tree.
 *
 * The probe discovers:
 *
 * - physical FLASH memory;
 * - physical SRAM memory;
 * - physical EEPROM memory;
 * - AVR-GCC linker memory regions;
 * - AVR Special Function Register (SFR) definitions;
 * - the selected device's program-memory address representation.
 *
 * Physical memory and SFR information are obtained from the concrete
 * avr-libc device header selected by:
 *
 * @code
 * avr-gcc -mmcu=<device>
 * @endcode
 *
 * Linker-region information is obtained from the AVR-GCC/binutils linker
 * configuration selected by the same device option.
 *
 * This header is an internal project interface.  Applications should
 * normally use @ref avr_device.h rather than calling the probe layer
 * directly.
 */

#ifndef AVR_DEVICE_PROBE_H
#define AVR_DEVICE_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "avr_sfr.h"


/**
 * @struct AvrDeviceProbeResult
 * @brief Device information discovered from the AVR toolchain.
 *
 * @details
 * This structure is populated by @ref avr_device_probe().
 *
 * The information comes from two logically distinct sources:
 *
 * - avr-libc:
 *   physical MCU memory and concrete SFR definitions;
 *
 * - AVR-GCC/binutils:
 *   ELF/linker address-space layout.
 *
 * This distinction is intentional because physical memory addresses and
 * ELF/linker addresses are not necessarily numerically identical.
 *
 * On successful probing, dynamically allocated members are owned by the
 * probe result until ownership is transferred to an @ref AvrDevice or
 * the result is released with @ref avr_device_probe_destroy().
 */
typedef struct {

  /**
   * Canonical AVR device name reported by AVR-GCC.
   *
   * @details
   * Typical value:
   *
   * @code
   * "atmega328p"
   * @endcode
   *
   * The string is owned by the probe result.
   *
   * @see avr_device_probe()
   * @see avr_device_probe_destroy()
   */
  char *name;


  /**
   * @name Physical memory
   * @{
   */

  /**
   * Physical FLASH start address.
   *
   * @note
   * FLASH addresses are byte addresses.
   */
  uint32_t flash_start;

  /**
   * Physical FLASH size in bytes.
   */
  uint32_t flash_size;

  /**
   * Physical SRAM start address.
   */
  uint32_t sram_start;

  /**
   * Physical SRAM size in bytes.
   *
   * @details
   * This region contains runtime storage used by sections such as
   * `.data`, `.bss`, `.noinit`, the heap, and the stack.
   */
  uint32_t sram_size;

  /**
   * Physical EEPROM start address.
   */
  uint32_t eeprom_start;

  /**
   * Physical EEPROM size in bytes.
   */
  uint32_t eeprom_size;

  /** @} */


  /**
   * @name AVR-GCC linker regions
   * @{
   *
   * @details
   * These values describe the address spaces used by the AVR linker and
   * therefore correspond to addresses that can appear in ELF section and
   * symbol values.
   *
   * They must not be confused with the physical-memory addresses above.
   */

  /**
   * Origin of the AVR-GCC DATA linker region.
   */
  uint32_t data_region_origin;

  /**
   * Length of the AVR-GCC DATA linker region in bytes.
   */
  uint32_t data_region_length;

  /**
   * Origin of the AVR-GCC TEXT linker region.
   */
  uint32_t text_region_origin;

  /**
   * Length of the AVR-GCC TEXT linker region in bytes.
   */
  uint32_t text_region_length;

  /**
   * Origin of the AVR-GCC EEPROM linker region.
   */
  uint32_t eeprom_region_origin;

  /**
   * Length of the AVR-GCC EEPROM linker region in bytes.
   */
  uint32_t eeprom_region_length;

  /** @} */


  /**
   * @name Flash address representation
   * @{
   */

  /**
   * Number of bits by which a physical FLASH byte address is shifted
   * to obtain the AVR CPU instruction/word address.
   *
   * @details
   * Conventional AVR devices use:
   *
   * @code
   * word_address = byte_address >> 1;
   * @endcode
   *
   * Consequently, the value is normally `1`.
   *
   * The field is kept in the probe result because the addressing model
   * is a property of the selected device rather than of the ELF parser.
   */
  unsigned flash_address_shift;

  /** @} */


  /**
   * @name Special Function Register database
   * @{
   */

  /**
   * Discovered AVR SFR database.
   *
   * @details
   * The database contains concrete SFR definitions obtained from the
   * selected avr-libc device header.
   *
   * The database owns its internal storage.
   *
   * After successful device initialization, ownership is transferred to
   * the resulting @ref AvrDevice.
   *
   * @see avr_sfr_destroy()
   */
  AvrSfrDatabase *sfr_database;

  /** @} */

} AvrDeviceProbeResult;


/**
 * @brief Discover one AVR device from the installed toolchain.
 *
 * @param[in] device_name
 *   AVR MCU name accepted by:
 *   @code
 *   avr-gcc -mmcu=<device>
 *   @endcode
 *
 *   Example:
 *   @code
 *   "atmega328p"
 *   @endcode
 *
 * @param[out] result
 *   Probe-result object to populate.
 *
 * @return
 *   `true` if all required device information was successfully
 *   discovered; `false` otherwise.
 *
 * @details
 * On success, the result contains:
 *
 * - physical memory regions;
 * - AVR-GCC linker address-space regions;
 * - FLASH byte-to-CPU-word address representation;
 * - the canonical device name;
 * - the discovered SFR database when available.
 *
 * The caller owns the returned result and must eventually call
 * @ref avr_device_probe_destroy().
 *
 * @note
 * SFR discovery is an auxiliary capability of the probe.  A device may
 * still be usable by higher layers when basic physical-memory and linker
 * discovery succeeds but SFR parsing is unavailable.
 */
bool
avr_device_probe(
  const char *device_name,
  AvrDeviceProbeResult *result
);


/**
 * @brief Release all resources owned by a probe result.
 *
 * @param[in,out] result
 *   Probe-result object to destroy.
 *
 * @details
 * This function releases all dynamically allocated resources owned by
 * the result, including:
 *
 * - @ref AvrDeviceProbeResult::name;
 * - @ref AvrDeviceProbeResult::sfr_database.
 *
 * After the call, the structure is reset to its zero-initialized state.
 *
 * @note
 * It is safe to pass `NULL`.
 */
void
avr_device_probe_destroy(
  AvrDeviceProbeResult *result
);


#endif /* AVR_DEVICE_PROBE_H */
