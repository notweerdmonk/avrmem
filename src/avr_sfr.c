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
 * @file avr_sfr.c
 * @author notweerdmonk, gpt-5.6-luna
 * @brief AVR Special Function Register parser and database implementation.
 *
 * @details
 * This module parses the concrete SFR definitions emitted by AVR-GCC
 * preprocessing and converts them into a normalized database of AVR
 * Special Function Registers.
 *
 * The parser currently recognizes concrete object-like definitions of:
 *
 * @code
 * #define PORTB  _SFR_IO8(0x05)
 * #define UCSR0A _SFR_MEM8(0xC0)
 * @endcode
 *
 * The two representations are normalized into canonical physical AVR
 * DATA-space addresses:
 *
 * @code
 * _SFR_IO8(x):
 *     physical address = x + __SFR_OFFSET
 *
 * _SFR_MEM8(x):
 *     physical address = x
 * @endcode
 *
 * The database deliberately does not contain MCU-specific register tables.
 * All register definitions come from the concrete avr-libc device header
 * selected by AVR-GCC.
 *
 * @par Ownership
 * The database owns the register-name strings and the dynamically
 * allocated register array.  Ownership is released by
 * @ref avr_sfr_destroy.
 */

#include "avr_sfr.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>


/**
 * @struct AvrSfrDatabase
 * @brief Internal representation of the AVR SFR database.
 *
 * @details
 * This is the concrete implementation of the opaque
 * @ref AvrSfrDatabase type declared by `avr_sfr.h`.
 */
struct AvrSfrDatabase {

  /**
   * Dynamically allocated array of discovered SFR definitions.
   *
   * @ownership
   * The database owns this allocation.
   */
  AvrSfr *registers;

  /**
   * Number of valid entries currently stored in @ref registers.
   */
  size_t count;

  /**
   * Number of entries for which storage is currently allocated.
   */
  size_t capacity;

  /**
   * AVR-LibC I/O-space to DATA-space offset.
   *
   * @details
   * For a classic AVR such as the ATmega328P this is typically:
   *
   * @code
   * __SFR_OFFSET = 0x20
   * @endcode
   *
   * It is used to normalize `_SFR_IO8()` definitions.
   */
  uint16_t sfr_offset;
};


/*
 * ----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------
 */


/**
 * @brief Parse an unsigned 16-bit integer from textual input.
 *
 * @param[in] text
 *   NUL-terminated integer expression.
 *
 * @param[out] value
 *   Receives the parsed value.
 *
 * @return
 *   `true` if the text is a valid unsigned 16-bit integer with optional
 *   C integer suffixes; `false` otherwise.
 *
 * @details
 * The parser accepts the integer syntax supported by `strtoul()` with
 * base `0`, including decimal and hexadecimal values.
 *
 * Common unsigned/long suffixes are accepted:
 *
 * - `U` / `u`
 * - `L` / `l`
 */
static bool
parse_u16(
  const char *text,
  uint16_t *value)
{
  char *end;
  unsigned long parsed;

  if (text == NULL ||
      value == NULL)
  {
    return false;
  }

  errno = 0;

  parsed =
    strtoul(
      text,
      &end,
      0
    );

  if (errno != 0 ||
      end == text ||
      parsed > UINT16_MAX)
  {
    return false;
  }

  /*
   * Permit whitespace and the usual C integer suffixes after the
   * numeric portion.
   */
  while (*end != '\0') {
    if (isspace((unsigned char)*end)) {
      ++end;
      continue;
    }

    if (*end == 'U' || *end == 'u' ||
        *end == 'L' || *end == 'l')
    {
      ++end;
      continue;
    }

    return false;
  }

  *value =
    (uint16_t)parsed;

  return true;
}


/**
 * @brief Ensure that the SFR database can hold a requested number of
 * entries.
 *
 * @param[in,out] database
 *   Database whose backing array should be expanded.
 *
 * @param[in] required
 *   Minimum number of entries that must be representable.
 *
 * @return
 *   `true` when sufficient storage is available after the operation;
 *   `false` when the required capacity cannot be represented or memory
 *   allocation fails.
 *
 * @details
 * The capacity is doubled as required, beginning with an initial capacity
 * of 32 entries.
 */
static bool
database_reserve(
  AvrSfrDatabase *database,
  size_t required)
{
  AvrSfr *new_registers;
  size_t new_capacity;

  if (database == NULL)
    return false;

  if (required <= database->capacity)
    return true;

  new_capacity =
    database->capacity != 0
      ? database->capacity
      : 32;

  while (new_capacity < required) {
    if (new_capacity >
        SIZE_MAX / 2)
    {
      return false;
    }

    new_capacity *= 2;
  }

  new_registers =
    realloc(
      database->registers,
      new_capacity *
      sizeof(*new_registers)
    );

  if (new_registers == NULL)
    return false;

  database->registers =
    new_registers;

  database->capacity =
    new_capacity;

  return true;
}


/**
 * @brief Duplicate a character range as a NUL-terminated string.
 *
 * @param[in] start
 *   First character to copy.
 *
 * @param[in] end
 *   One-past-the-last character to copy.
 *
 * @return
 *   Newly allocated NUL-terminated string, or `NULL` on invalid input
 *   or allocation failure.
 *
 * @ownership
 * The caller owns the returned string.
 */
static char *
duplicate_range(
  const char *start,
  const char *end)
{
  size_t length;
  char *copy;

  if (start == NULL ||
      end == NULL ||
      end < start)
  {
    return NULL;
  }

  length =
    (size_t)(end - start);

  copy =
    malloc(
      length + 1
    );

  if (copy == NULL)
    return NULL;

  memcpy(
    copy,
    start,
    length
  );

  copy[length] =
    '\0';

  return copy;
}


/*
 * ----------------------------------------------------------------------
 * Database initialization/destruction
 * ----------------------------------------------------------------------
 */


/**
 * @brief Allocate and initialize an empty SFR database.
 *
 * @param[out] out_database
 *   Receives the newly allocated database.
 *
 * @return
 *   `true` on successful allocation; `false` otherwise.
 *
 * @details
 * The resulting database initially contains no registers and has no
 * parsed `__SFR_OFFSET` value.
 *
 * @note
 * The caller owns the resulting object and must eventually call
 * @ref avr_sfr_destroy.
 */
bool
avr_sfr_init(
  AvrSfrDatabase **out_database)
{
  AvrSfrDatabase *database;

  if (out_database == NULL)
    return false;

  *out_database =
    NULL;

  database =
    calloc(
      1,
      sizeof(*database)
    );

  if (database == NULL)
    return false;

  *out_database =
    database;

  return true;
}


/**
 * @brief Destroy an SFR database and release all owned resources.
 *
 * @param[in,out] database
 *   Database to destroy.
 *
 * @details
 * Releases:
 *
 * - every dynamically allocated register name;
 * - the register array;
 * - the database object itself.
 *
 * @note
 * Passing `NULL` is safe.
 */
void
avr_sfr_destroy(
  AvrSfrDatabase *database)
{
  size_t i;

  if (database == NULL)
    return;

  for (i = 0;
       i < database->count;
       ++i)
  {
    /*
     * AvrSfr::name is stored as const char * in the public interface,
     * but the database owns the actual allocated character storage.
     */
    free(
      (char *)database->registers[i].name
    );
  }

  free(
    database->registers
  );

  free(
    database
  );
}


/*
 * ----------------------------------------------------------------------
 * SFR insertion
 * ----------------------------------------------------------------------
 */


/**
 * @brief Add one normalized SFR definition to the database.
 *
 * @param[in,out] database
 *   Database receiving the new register.
 *
 * @param[in] name
 *   Dynamically allocated symbolic register name.
 *
 * @param[in] space
 *   Original avr-libc addressing mechanism.
 *
 * @param[in] encoded_address
 *   Address argument exactly as emitted inside `_SFR_IO8()` or
 *   `_SFR_MEM8()`.
 *
 * @return
 *   `true` when the register is accepted or an identical-name entry
 *   already exists; `false` on invalid input, address overflow, capacity
 *   failure, or allocation failure.
 *
 * @ownership
 * This function takes ownership of @p name regardless of whether the
 * insertion succeeds.
 *
 * On failure, the name is freed locally.
 *
 * @details
 * The original avr-libc representation is normalized as follows:
 *
 * @code
 * _SFR_IO8(x):
 *     address = x + database->sfr_offset
 *
 * _SFR_MEM8(x):
 *     address = x
 * @endcode
 *
 * The normalized address must fit in 16 bits.
 *
 * Duplicate symbolic names are ignored.
 */
static bool
add_sfr(
  AvrSfrDatabase *database,
  char *name,
  AvrSfrSpace space,
  uint16_t encoded_address)
{
  AvrSfr *sfr;
  uint32_t address;

  /*
   * `name` is owned by this function.
   *
   * On failure it is released here; on success ownership is transferred
   * into the database.
   */
  if (database == NULL ||
      name == NULL ||
      *name == '\0')
  {
    free(name);
    return false;
  }

  /*
   * Convert the original avr-libc representation into canonical
   * physical AVR DATA-space addressing.
   */
  if (space == AVR_SFR_SPACE_IO) {
    address =
      (uint32_t)encoded_address +
      database->sfr_offset;
  }
  else {
    address =
      encoded_address;
  }

  /*
   * The public SFR address representation is 16 bits wide.
   */
  if (address > UINT16_MAX) {
    free(name);
    return false;
  }

  /*
   * Avoid duplicate entries for the same symbolic register name.
   */
  for (size_t i = 0;
       i < database->count;
       ++i)
  {
    if (strcmp(
          database->registers[i].name,
          name
        ) == 0)
    {
      free(name);
      return true;
    }
  }

  if (!database_reserve(
        database,
        database->count + 1
      ))
  {
    free(name);
    return false;
  }

  sfr =
    &database->registers[
      database->count
    ];

  memset(
    sfr,
    0,
    sizeof(*sfr)
  );

  /*
   * Ownership of `name` now belongs to the database.
   */
  sfr->name =
    name;

  sfr->address =
    (uint16_t)address;

  sfr->space =
    space;

  sfr->encoded_address =
    encoded_address;

  /*
   * The current parser handles only _SFR_*8() definitions.
   */
  sfr->width =
    1;

  ++database->count;

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Definition parsing
 * ----------------------------------------------------------------------
 *
 * Only concrete object-like SFR definitions are accepted:
 *
 *   #define NAME _SFR_IO8(0x05)
 *   #define NAME _SFR_MEM8(0xC0)
 *
 * Function-like macros and unrelated preprocessor definitions are ignored.
 */


/**
 * @brief Parse one preprocessor line for a concrete SFR definition.
 *
 * @param[in,out] database
 *   SFR database receiving the parsed definition.
 *
 * @param[in] line
 *   One complete preprocessor-output line.
 *
 * @return
 *   `true` when the line is successfully processed or is unrelated to
 *   an SFR; `false` when the line looks like an SFR definition but
 *   contains malformed or unsupported data.
 *
 * @details
 * The recognized forms are:
 *
 * @code
 * #define NAME _SFR_IO8(address)
 * #define NAME _SFR_MEM8(address)
 * @endcode
 *
 * Function-like macros and all unrelated macros are ignored.
 */
static bool
parse_definition(
  AvrSfrDatabase *database,
  const char *line)
{
  const char *p;
  const char *name_start;
  const char *name_end;
  const char *argument_start;
  const char *argument_end;

  AvrSfrSpace space;

  char value[64];
  uint16_t encoded_address;

  if (database == NULL ||
      line == NULL)
  {
    return false;
  }

  /*
   * Ignore everything except preprocessor definitions.
   */
  if (strncmp(
        line,
        "#define",
        7
      ) != 0)
  {
    return true;
  }

  p =
    line + 7;

  while (*p == ' ' ||
         *p == '\t')
  {
    ++p;
  }

  name_start =
    p;

  while (*p != '\0' &&
         (isalnum((unsigned char)*p) ||
          *p == '_'))
  {
    ++p;
  }

  name_end =
    p;

  if (name_end == name_start)
    return true;

  /*
   * A function-like macro is not an SFR object definition.
   */
  if (*p == '(')
    return true;

  while (*p == ' ' ||
         *p == '\t')
  {
    ++p;
  }

  /*
   * Determine the original avr-libc SFR encoding.
   */
  if (strncmp(
        p,
        "_SFR_IO8(",
        9
      ) == 0)
  {
    space =
      AVR_SFR_SPACE_IO;

    argument_start =
      p + 9;
  }
  else if (strncmp(
             p,
             "_SFR_MEM8(",
             10
           ) == 0)
  {
    space =
      AVR_SFR_SPACE_DATA;

    argument_start =
      p + 10;
  }
  else {
    /*
     * A normal preprocessor macro, not an SFR definition.
     */
    return true;
  }

  argument_end =
    strchr(
      argument_start,
      ')'
    );

  if (argument_end == NULL)
    return false;

  if ((size_t)(argument_end - argument_start) >=
      sizeof(value))
  {
    return false;
  }

  memcpy(
    value,
    argument_start,
    (size_t)(argument_end - argument_start)
  );

  value[
    argument_end - argument_start
  ] =
    '\0';

  if (!parse_u16(
        value,
        &encoded_address
      ))
  {
    return false;
  }

  {
    char *name;

    name =
      duplicate_range(
        name_start,
        name_end
      );

    if (name == NULL)
      return false;

    /*
     * add_sfr() takes ownership of `name`.
     */
    return add_sfr(
      database,
      name,
      space,
      encoded_address
    );
  }
}


/*
 * ----------------------------------------------------------------------
 * Preprocessor-output parser
 * ----------------------------------------------------------------------
 */


/**
 * @brief Parse complete AVR-GCC preprocessor output into an SFR database.
 *
 * @param[in,out] database
 *   Database to populate.
 *
 * @param[in] preprocessor_output
 *   Complete NUL-terminated output from an AVR-GCC `-dM -E` invocation.
 *
 * @return
 *   `true` when a non-empty SFR database is produced; `false` when
 *   required data is malformed, parsing fails, or no SFR definitions are
 *   found.
 *
 * @details
 * Parsing is deliberately performed in two passes.
 *
 * First, the function locates:
 *
 * @code
 * #define __SFR_OFFSET ...
 * @endcode
 *
 * because `_SFR_IO8()` definitions cannot be normalized correctly until
 * that value is known.
 *
 * Second, every line of the preprocessor output is examined for concrete
 * `_SFR_IO8()` and `_SFR_MEM8()` definitions.
 *
 * @note
 * The parser currently requires `__SFR_OFFSET` to be present, even for
 * a device whose SFR definitions might all happen to use memory-space
 * notation.
 */
bool
avr_sfr_parse(
  AvrSfrDatabase *database,
  const char *preprocessor_output)
{
  const char *line;

  if (database == NULL ||
      preprocessor_output == NULL)
  {
    return false;
  }

  /*
   * --------------------------------------------------------------
   * First pass: locate __SFR_OFFSET.
   * --------------------------------------------------------------
   *
   * This value is required before an _SFR_IO8() address can be
   * normalized.
   */
  line =
    preprocessor_output;

  while (*line != '\0') {
    const char *end;
    size_t length;
    char buffer[256];

    end =
      strchr(
        line,
        '\n'
      );

    if (end == NULL)
      length =
        strlen(line);
    else
      length =
        (size_t)(end - line);

    if (length >= sizeof(buffer))
      return false;

    memcpy(
      buffer,
      line,
      length
    );

    buffer[length] =
      '\0';

    if (strncmp(
          buffer,
          "#define __SFR_OFFSET",
          20
        ) == 0)
    {
      const char *value =
        buffer + 20;

      while (*value == ' ' ||
             *value == '\t')
      {
        ++value;
      }

      if (!parse_u16(
            value,
            &database->sfr_offset
          ))
      {
        return false;
      }

      break;
    }

    if (end == NULL)
      break;

    line =
      end + 1;
  }

  /*
   * --------------------------------------------------------------
   * Second pass: parse all concrete SFR definitions.
   * --------------------------------------------------------------
   */
  line =
    preprocessor_output;

  while (*line != '\0') {
    const char *end;
    size_t length;
    char buffer[512];

    end =
      strchr(
        line,
        '\n'
      );

    if (end == NULL)
      length =
        strlen(line);
    else
      length =
        (size_t)(end - line);

    if (length >= sizeof(buffer))
      return false;

    memcpy(
      buffer,
      line,
      length
    );

    buffer[length] =
      '\0';

    if (!parse_definition(
          database,
          buffer
        ))
    {
      return false;
    }

    if (end == NULL)
      break;

    line =
      end + 1;
  }

  return database->count != 0;
}


/*
 * ----------------------------------------------------------------------
 * Accessors
 * ----------------------------------------------------------------------
 */


/**
 * @brief Return the number of SFR definitions in a database.
 *
 * @param[in] database
 *   SFR database to inspect.
 *
 * @return
 *   Number of stored SFR definitions, or `0` when @p database is `NULL`.
 */
size_t
avr_sfr_count(
  const AvrSfrDatabase *database)
{
  if (database == NULL)
    return 0;

  return database->count;
}


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
 *   Pointer to the requested @ref AvrSfr, or `NULL` when the database is
 *   `NULL` or @p index is out of range.
 *
 * @ownership
 * The returned object is owned by the database and must not be freed or
 * modified by the caller.
 *
 * @note
 * The returned pointer remains valid until @ref avr_sfr_destroy is
 * called, unless the database is subsequently expanded by a parsing
 * operation that reallocates its register array.
 */
const AvrSfr *
avr_sfr_at(
  const AvrSfrDatabase *database,
  size_t index)
{
  if (database == NULL ||
      index >= database->count)
  {
    return NULL;
  }

  return &database->registers[index];
}


/**
 * @brief Find an SFR by symbolic name.
 *
 * @param[in] database
 *   SFR database to search.
 *
 * @param[in] name
 *   NUL-terminated symbolic register name.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no register
 *   matches.
 *
 * @ownership
 * The returned object is owned by the database and must not be freed or
 * modified by the caller.
 */
const AvrSfr *
avr_sfr_find(
  const AvrSfrDatabase *database,
  const char *name)
{
  if (database == NULL ||
      name == NULL)
  {
    return NULL;
  }

  for (size_t i = 0;
       i < database->count;
       ++i)
  {
    if (strcmp(
          database->registers[i].name,
          name
        ) == 0)
    {
      return &database->registers[i];
    }
  }

  return NULL;
}


/**
 * @brief Find an SFR by canonical physical DATA-space address.
 *
 * @param[in] database
 *   SFR database to search.
 *
 * @param[in] address
 *   Canonical physical AVR DATA-space address.
 *
 * @return
 *   Pointer to the matching @ref AvrSfr, or `NULL` when no register is
 *   defined at the supplied address.
 *
 * @ownership
 * The returned object is owned by the database and must not be freed or
 * modified by the caller.
 *
 * @note
 * When multiple symbolic aliases occupy the same address, the first
 * matching entry in database order is returned.
 */
const AvrSfr *
avr_sfr_find_address(
  const AvrSfrDatabase *database,
  uint16_t address)
{
  if (database == NULL)
    return NULL;

  for (size_t i = 0;
       i < database->count;
       ++i)
  {
    if (database->registers[i].address ==
        address)
    {
      return &database->registers[i];
    }
  }

  return NULL;
}


/**
 * @brief Return the parsed `__SFR_OFFSET` value.
 *
 * @param[in] database
 *   SFR database to inspect.
 *
 * @return
 *   The stored I/O-to-DATA-space offset, or `0` when @p database is
 *   `NULL`.
 *
 * @details
 * This value is the offset used to normalize `_SFR_IO8()` definitions.
 */
uint16_t
avr_sfr_offset(
  const AvrSfrDatabase *database)
{
  if (database == NULL)
    return 0;

  return database->sfr_offset;
}


/**
 * @brief Return a printable name for an SFR address-space encoding.
 *
 * @param[in] space
 *   Original avr-libc SFR encoding.
 *
 * @return
 *   Constant human-readable name:
 *
 * @retval "IO"
 *   @ref AVR_SFR_SPACE_IO.
 *
 * @retval "DATA"
 *   @ref AVR_SFR_SPACE_DATA.
 *
 * @retval "UNKNOWN"
 *   Unsupported or invalid enumeration value.
 */
const char *
avr_sfr_space_name(
  AvrSfrSpace space)
{
  switch (space) {

    case AVR_SFR_SPACE_IO:
      return "IO";

    case AVR_SFR_SPACE_DATA:
      return "DATA";

    default:
      return "UNKNOWN";
  }
}
