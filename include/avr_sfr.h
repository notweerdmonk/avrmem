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
 * @file avr_sfr.h
 * @author notweerdmonk, gpt-5.6-luna
 * @brief AVR Special Function Register abstraction.
 *
 * @details
 * The SFR database is populated from the concrete preprocessor definitions
 * emitted by AVR-GCC for the selected MCU.
 *
 * The implementation does not maintain an MCU-specific register table.
 * Instead, it consumes the device-specific definitions supplied by the
 * installed avr-libc headers.
 *
 * Two avr-libc address representations are recognized:
 *
 * @code
 * _SFR_IO8(x)
 * @endcode
 *
 * Here, @c x is an AVR I/O-space address and must be translated using
 * `__SFR_OFFSET`.
 *
 * @code
 * _SFR_MEM8(x)
 * @endcode
 *
 * Here, @c x is already expressed as an AVR DATA-space address.
 *
 * Regardless of the original encoding, the canonical @ref AvrSfr::address
 * stored by this module is always the physical AVR DATA-space address.
 */

#ifndef AVR_SFR_H
#define AVR_SFR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/**
 * @enum AvrSfrSpace
 * @brief Identifies the addressing mechanism used by an avr-libc SFR
 * definition.
 *
 * @details
 * This enumeration records how the original register definition was
 * encoded by avr-libc.
 *
 * The database currently recognizes:
 *
 * - `_SFR_IO8(...)`
 * - `_SFR_MEM8(...)`
 *
 * The original encoding is retained for diagnostics and tooling, while
 * @ref AvrSfr::address always contains the normalized physical AVR
 * DATA-space address.
 */
typedef enum {
  /**
   * SFR originally defined with `_SFR_IO8(...)`.
   *
   * @details
   * The encoded address must be translated using the selected device's
   * `__SFR_OFFSET`.
   */
  AVR_SFR_SPACE_IO = 0,

  /**
   * SFR originally defined with `_SFR_MEM8(...)`.
   *
   * @details
   * The encoded value is already an AVR DATA-space address.
   */
  AVR_SFR_SPACE_DATA

} AvrSfrSpace;


/**
 * @struct AvrSfr
 * @brief Describes one 8-bit AVR Special Function Register.
 *
 * @details
 * An `AvrSfr` contains both the normalized physical DATA-space address
 * and the original representation emitted by avr-libc.
 *
 * This allows clients to use the canonical physical address for address
 * resolution while retaining the original encoding for diagnostics or
 * analysis.
 */
typedef struct {

  /**
   * Symbolic SFR name.
   *
   * @details
   * Examples include:
   *
   * @code
   * PORTB
   * UCSR0A
   * SREG
   * @endcode
   *
   * The string is owned by the containing @ref AvrSfrDatabase.
   */
  const char *name;

  /**
   * Canonical physical AVR DATA-space address.
   *
   * @details
   * This is the normalized address used by address-based lookup.
   *
   * For an `_SFR_IO8(x)` definition:
   *
   * @code
   * address = x + __SFR_OFFSET
   * @endcode
   *
   * For an `_SFR_MEM8(x)` definition:
   *
   * @code
   * address = x
   * @endcode
   */
  uint16_t address;

  /**
   * Original avr-libc SFR addressing mechanism.
   */
  AvrSfrSpace space;

  /**
   * Original argument supplied to `_SFR_IO8()` or `_SFR_MEM8()`.
   *
   * @details
   * Unlike @ref address, this value is not normalized.
   */
  uint16_t encoded_address;

  /**
   * Register width in bytes.
   *
   * @details
   * The current SFR database recognizes only 8-bit SFR definitions,
   * therefore this value is currently always `1`.
   */
  uint8_t width;

} AvrSfr;


/**
 * @struct AvrSfrDatabase
 * @brief Opaque database containing discovered AVR SFR definitions.
 *
 * @details
 * The implementation owns all storage associated with the database.
 *
 * An SFR database is normally created by the device-probe layer,
 * populated from AVR-GCC preprocessor output, and then owned by an
 * @ref AvrDevice.
 */
typedef struct AvrSfrDatabase AvrSfrDatabase;


/**
 * @brief Initialize an empty SFR database.
 *
 * @param[out] out_database
 *   Receives the newly allocated database.
 *
 * @return
 *   `true` on successful initialization; `false` on failure.
 *
 * @note
 * The caller owns the resulting database and must eventually pass it to
 * @ref avr_sfr_destroy.
 */
bool
avr_sfr_init(
  AvrSfrDatabase **out_database
);


/**
 * @brief Release an SFR database and all associated storage.
 *
 * @param[in,out] database
 *   Database to destroy.
 *
 * @note
 * It is safe to pass `NULL`.
 */
void
avr_sfr_destroy(
  AvrSfrDatabase *database
);


/**
 * @brief Parse concrete AVR-GCC preprocessor output into an SFR database.
 *
 * @param[in,out] database
 *   SFR database to populate.
 *
 * @param[in] preprocessor_output
 *   NUL-terminated text produced by AVR-GCC preprocessing.
 *
 * @return
 *   `true` when parsing succeeds; `false` when the supplied data cannot
 *   be parsed or the database cannot be populated.
 *
 * @details
 * The input is expected to contain concrete definitions similar to:
 *
 * @code
 * #define __SFR_OFFSET 0x20
 * #define PORTB _SFR_IO8(0x05)
 * #define UCSR0A _SFR_MEM8(0xC0)
 * @endcode
 *
 * The parser recognizes `_SFR_IO8()` and `_SFR_MEM8()` definitions.
 *
 * `_SFR_IO8()` values are normalized using `__SFR_OFFSET`.
 * `_SFR_MEM8()` values are retained as DATA-space addresses.
 *
 * @note
 * The current database contains only 8-bit SFR definitions.
 */
bool
avr_sfr_parse(
  AvrSfrDatabase *database,
  const char *preprocessor_output
);


/**
 * @brief Return the number of parsed SFR definitions.
 *
 * @param[in] database
 *   SFR database to inspect.
 *
 * @return
 *   Number of SFR entries, or `0` when @p database is `NULL` or empty.
 */
size_t
avr_sfr_count(
  const AvrSfrDatabase *database
);


/**
 * @brief Return an SFR by database index.
 *
 * @param[in] database
 *   SFR database to inspect.
 *
 * @param[in] index
 *   Zero-based SFR index.
 *
 * @return
 *   Pointer to the requested @ref AvrSfr, or `NULL` when the index is
 *   outside the database.
 *
 * @warning
 * The returned pointer is owned by the database and must not be freed
 * or modified by the caller.
 *
 * @note
 * The pointer remains valid until the database is destroyed or reparsed.
 */
const AvrSfr *
avr_sfr_at(
  const AvrSfrDatabase *database,
  size_t index
);


/**
 * @brief Find an SFR by symbolic register name.
 *
 * @param[in] database
 *   SFR database to search.
 *
 * @param[in] name
 *   NUL-terminated symbolic register name.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no register with
 *   that name exists.
 *
 * @warning
 * The returned pointer is owned by the database and must not be freed
 * or modified by the caller.
 */
const AvrSfr *
avr_sfr_find(
  const AvrSfrDatabase *database,
  const char *name
);


/**
 * @brief Find an SFR by canonical physical AVR DATA-space address.
 *
 * @param[in] database
 *   SFR database to search.
 *
 * @param[in] address
 *   Canonical physical AVR DATA-space address.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no register is
 *   defined at that address.
 *
 * @warning
 * The returned pointer is owned by the database and must not be freed
 * or modified by the caller.
 */
const AvrSfr *
avr_sfr_find_address(
  const AvrSfrDatabase *database,
  uint16_t address
);


/**
 * @brief Return the `__SFR_OFFSET` value used by the selected avr-libc
 * device header.
 *
 * @param[in] database
 *   SFR database to inspect.
 *
 * @return
 *   The parsed `__SFR_OFFSET` value.
 *
 * @note
 * A return value of `0` is also a valid offset, so callers that need to
 * distinguish "not initialized" from an actual zero offset should use
 * the database lifecycle/status APIs rather than interpreting `0` alone.
 */
uint16_t
avr_sfr_offset(
  const AvrSfrDatabase *database
);


/**
 * @brief Return a printable name for an SFR addressing mechanism.
 *
 * @param[in] space
 *   @ref AvrSfrSpace value to convert.
 *
 * @return
 *   Constant human-readable string corresponding to @p space.
 */
const char *
avr_sfr_space_name(
  AvrSfrSpace space
);


#endif /* AVR_SFR_H */
