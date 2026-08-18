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
 * @file avr_device_probe.c
 * @author notweerdmonk, gpt-5.6-luna
 * @brief AVR device discovery through the installed AVR-GCC/avr-libc toolchain.
 *
 * @details
 * This module discovers device-specific information by interrogating the
 * installed AVR toolchain rather than maintaining an MCU-specific table in
 * the project source tree.
 *
 * Discovery has two primary sources:
 *
 * 1. AVR-GCC preprocessing with `<avr/io.h>`.
 *
 *    @code
 *    avr-gcc -mmcu=<device> -dM -E ...
 *    @endcode
 *
 *    This supplies:
 *
 *    - physical memory information;
 *    - canonical device identity;
 *    - concrete SFR definitions.
 *
 * 2. An actual AVR-GCC link with a generated linker map.
 *
 *    @code
 *    avr-gcc -mmcu=<device> ... -Wl,-Map=<map>
 *    @endcode
 *
 *    This supplies the resolved AVR linker MEMORY regions.
 *
 * The generated linker map is deliberately used instead of attempting to
 * parse the default linker script directly.  Linker scripts can contain
 * symbolic expressions and other constructs that are resolved only during
 * linking, while the generated map contains the final numeric values
 * selected by the linker.
 *
 * @note
 * The implementation assumes a POSIX host.
 *
 * @note
 * Device names are validated before being incorporated into shell
 * commands.
 *
 * @see avr_device_probe
 * @see avr_device_probe_destroy
 */

#define _POSIX_C_SOURCE 200809L

#include "avr_device_probe.h"

#include <limits.h>
#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


/**
 * @def AVR_PROBE_COMMAND_SIZE
 * @brief Maximum size of a generated host/toolchain command.
 */
#define AVR_PROBE_COMMAND_SIZE 1024


/**
 * @def AVR_PROBE_OUTPUT_LIMIT
 * @brief Maximum amount of captured probe output in bytes.
 *
 * @details
 * The limit prevents unbounded growth of buffers when capturing compiler
 * or linker output.
 */
#define AVR_PROBE_OUTPUT_LIMIT (4U * 1024U * 1024U)


/**
 * @def AVR_PROBE_LINE_SIZE
 * @brief Maximum supported linker-map line length.
 */
#define AVR_PROBE_LINE_SIZE 4096


/**
 * @def AVR_PROBE_VALUE_SIZE
 * @brief Maximum size of a textual numeric macro value.
 */
#define AVR_PROBE_VALUE_SIZE 256


/*
 * ----------------------------------------------------------------------
 * Temporary-file helpers
 * ----------------------------------------------------------------------
 */

#if 0

/**
 * @brief Create a temporary C source file for an AVR-GCC probe.
 *
 * @param[out] path
 *   Buffer receiving the generated temporary pathname.
 *
 * @param[in] path_size
 *   Size of @p path in bytes.
 *
 * @return
 *   `true` when the source file was successfully created and populated;
 *   `false` otherwise.
 *
 * @details
 * The generated source contains only:
 *
 * @code
 * #include <avr/io.h>
 * int main(void) { return 0; }
 * @endcode
 *
 * This helper is currently disabled because the active macro probe feeds
 * the source directly to AVR-GCC through standard input.
 */
static bool
create_probe_source(
  char *path,
  size_t path_size)
{
  const char *tmpdir;
  int fd;
  FILE *fp;

  if (path == NULL || path_size == 0)
    return false;

  tmpdir = getenv("TMPDIR");

  if (tmpdir == NULL || *tmpdir == '\0')
    tmpdir = "/tmp";

  if (snprintf(
        path,
        path_size,
        "%s/avrmem-probe-XXXXXX",
        tmpdir
      ) >= (int)path_size)
  {
    return false;
  }

  fd = mkstemp(path);

  if (fd < 0) {
    fprintf(
      stderr,
      "probe: mkstemp('%s') failed: %s\n",
      path,
      strerror(errno)
    );

    return false;
  }

  fp = fdopen(fd, "w");

  if (fp == NULL) {
    fprintf(
      stderr,
      "probe: fdopen() failed: %s\n",
      strerror(errno)
    );

    close(fd);
    unlink(path);

    return false;
  }

  if (fputs(
        "#include <avr/io.h>\n"
        "int main(void) { return 0; }\n",
        fp
      ) == EOF)
  {
    fprintf(
      stderr,
      "probe: failed to write temporary source '%s': %s\n",
      path,
      strerror(errno)
    );

    fclose(fp);
    unlink(path);

    return false;
  }

  if (fclose(fp) != 0) {
    fprintf(
      stderr,
      "probe: failed to close temporary source '%s': %s\n",
      path,
      strerror(errno)
    );

    unlink(path);

    return false;
  }

  return true;
}

#endif


/**
 * @brief Validate a device name before using it in a shell command.
 *
 * @param[in] name
 *   Candidate AVR device name.
 *
 * @return
 *   `true` when the name contains only characters permitted by the probe;
 *   `false` otherwise.
 *
 * @details
 * The current accepted character set consists of:
 *
 * - ASCII alphanumeric characters;
 * - underscore (`_`);
 * - hyphen (`-`).
 *
 * This intentionally rejects shell metacharacters and whitespace because
 * the validated value is incorporated into commands executed through
 * `popen()`.
 */
static bool
valid_device_name(
  const char *name)
{
  const unsigned char *p;

  if (name == NULL || *name == '\0')
    return false;

  for (p = (const unsigned char *)name;
       *p != '\0';
       ++p)
  {
    if (isalnum(*p) ||
        *p == '_' ||
        *p == '-')
    {
      continue;
    }

    return false;
  }

  return true;
}


/**
 * @struct ProbeOutput
 * @brief Dynamically growing buffer used to capture toolchain output.
 *
 * @details
 * The buffer is NUL-terminated after each successful append so that the
 * accumulated output can also be treated as a C string.
 */
typedef struct {
  /**
   * Dynamically allocated output buffer.
   */
  char *data;

  /**
   * Number of data bytes currently stored, excluding the NUL terminator.
   */
  size_t size;

  /**
   * Allocated capacity of @ref ProbeOutput::data in bytes.
   */
  size_t capacity;

} ProbeOutput;


/**
 * @brief Destroy a captured probe-output buffer.
 *
 * @param[in,out] output
 *   Output object to release.
 *
 * @details
 * Frees the backing storage and resets all fields to zero.
 *
 * @note
 * Passing `NULL` is safe.
 */
static void
probe_output_destroy(
  ProbeOutput *output)
{
  if (output == NULL)
    return;

  free(output->data);

  memset(
    output,
    0,
    sizeof(*output)
  );
}


/**
 * @brief Append bytes to a captured probe-output buffer.
 *
 * @param[in,out] output
 *   Destination output buffer.
 *
 * @param[in] data
 *   Bytes to append.
 *
 * @param[in] size
 *   Number of bytes to append.
 *
 * @return
 *   `true` when all bytes are appended successfully; `false` when the
 *   output limit or an allocation constraint is exceeded.
 *
 * @details
 * The output is automatically grown as needed, subject to
 * @ref AVR_PROBE_OUTPUT_LIMIT.
 */
static bool
probe_output_append(
  ProbeOutput *output,
  const char *data,
  size_t size)
{
  size_t required;
  size_t new_capacity;
  char *new_data;

  if (output == NULL || data == NULL)
    return false;

  if (size > AVR_PROBE_OUTPUT_LIMIT)
    return false;

  if (output->size >
      AVR_PROBE_OUTPUT_LIMIT - size)
  {
    return false;
  }

  required =
    output->size +
    size +
    1;

  if (required <= output->capacity) {
    memcpy(
      output->data + output->size,
      data,
      size
    );

    output->size += size;
    output->data[output->size] = '\0';

    return true;
  }

  new_capacity =
    output->capacity != 0
      ? output->capacity
      : 4096;

  while (new_capacity < required) {
    if (new_capacity >
        AVR_PROBE_OUTPUT_LIMIT / 2)
    {
      new_capacity =
        AVR_PROBE_OUTPUT_LIMIT;
      break;
    }

    new_capacity *= 2;
  }

  if (new_capacity > AVR_PROBE_OUTPUT_LIMIT)
    return false;

  new_data =
    realloc(
      output->data,
      new_capacity
    );

  if (new_data == NULL)
    return false;

  output->data = new_data;
  output->capacity = new_capacity;

  memcpy(
    output->data + output->size,
    data,
    size
  );

  output->size += size;
  output->data[output->size] = '\0';

  return true;
}


/**
 * @brief Probe the selected avr-libc device header through AVR-GCC.
 *
 * @param[in] device_name
 *   AVR device accepted by `avr-gcc -mmcu=`.
 *
 * @param[out] output
 *   Receives the complete preprocessor macro environment.
 *
 * @return
 *   `true` when preprocessing succeeds and produces output;
 *   `false` otherwise.
 *
 * @details
 * The probe feeds the following translation unit to AVR-GCC through
 * standard input:
 *
 * @code
 * #include <avr/io.h>
 * int main(void) { return 0; }
 * @endcode
 *
 * AVR-GCC is invoked with:
 *
 * @code
 * -mmcu=<device> -dM -E -x c -
 * @endcode
 *
 * The resulting macro environment is later used to discover:
 *
 * - canonical device name;
 * - physical memory;
 * - SFR definitions.
 */
static bool
probe_avr_libc_header(
  const char *device_name,
  ProbeOutput *output)
{
  char command[AVR_PROBE_COMMAND_SIZE];
  FILE *pipe;
  char buffer[8192];
  int status;

  if (!valid_device_name(device_name) ||
      output == NULL)
  {
    return false;
  }

  if (snprintf(
        command,
        sizeof(command),
        "printf '#include <avr/io.h>\\n"
        "int main(void) { return 0; }\\n' "
        "| avr-gcc "
        "-mmcu=%s "
        "-dM "
        "-E "
        "-x c "
        "- "
        "2>&1",
        device_name
      ) >= (int)sizeof(command))
  {
    return false;
  }

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: macro command:\n"
    "  %s\n",
    command
  );
#endif

  pipe =
    popen(
      command,
      "r"
    );

  if (pipe == NULL) {
    fprintf(
      stderr,
      "probe: popen() failed: %s\n",
      strerror(errno)
    );

    return false;
  }

  while (!feof(pipe)) {
    size_t bytes;

    bytes =
      fread(
        buffer,
        1,
        sizeof(buffer),
        pipe
      );

    if (bytes == 0)
      break;

    if (!probe_output_append(
          output,
          buffer,
          bytes
        ))
    {
      pclose(pipe);
      return false;
    }
  }

  status =
    pclose(pipe);

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: macro command exit status = %d\n",
    status
  );
#endif

  if (status == -1)
    return false;

  if (!WIFEXITED(status))
    return false;

  if (WEXITSTATUS(status) != 0) {
    fprintf(
      stderr,
      "probe: avr-gcc output:\n%s\n",
      output->data != NULL
        ? output->data
        : ""
    );

    return false;
  }

  return output->size != 0;
}


/**
 * @brief Find a concrete preprocessor macro definition.
 *
 * @param[in] text
 *   Complete `avr-gcc -dM` output.
 *
 * @param[in] macro
 *   Macro name to find.
 *
 * @param[out] value
 *   Buffer receiving the macro's textual value.
 *
 * @param[in] value_size
 *   Size of @p value in bytes.
 *
 * @return
 *   `true` when the macro is found and its value fits in the supplied
 *   buffer; `false` otherwise.
 *
 * @details
 * The expected form is:
 *
 * @code
 * #define MACRO value
 * @endcode
 *
 * Function-like macros are intentionally ignored.
 */
static bool
find_macro(
  const char *text,
  const char *macro,
  char *value,
  size_t value_size)
{
  const char *p;
  size_t macro_length;

  if (text == NULL ||
      macro == NULL ||
      value == NULL ||
      value_size == 0)
  {
    return false;
  }

  macro_length =
    strlen(macro);

  p = text;

  while (*p != '\0') {
    const char *name_start;
    const char *name_end;
    const char *value_start;
    const char *value_end;
    size_t length;

    if (p != text && p[-1] != '\n') {
      ++p;
      continue;
    }

    if (strncmp(
          p,
          "#define ",
          8
        ) != 0)
    {
      ++p;
      continue;
    }

    name_start =
      p + 8;

    while (*name_start == ' ' ||
           *name_start == '\t')
    {
      ++name_start;
    }

    name_end =
      name_start;

    while (*name_end != '\0' &&
           (isalnum((unsigned char)*name_end) ||
            *name_end == '_'))
    {
      ++name_end;
    }

    if ((size_t)(name_end - name_start) !=
        macro_length ||
        strncmp(
          name_start,
          macro,
          macro_length
        ) != 0)
    {
      ++p;
      continue;
    }

    if (*name_end == '(') {
      ++p;
      continue;
    }

    value_start =
      name_end;

    while (*value_start == ' ' ||
           *value_start == '\t')
    {
      ++value_start;
    }

    value_end =
      strchr(
        value_start,
        '\n'
      );

    if (value_end == NULL)
      value_end =
        value_start +
        strlen(value_start);

    while (value_end > value_start &&
           isspace(
             (unsigned char)value_end[-1]))
    {
      --value_end;
    }

    length =
      (size_t)(value_end - value_start);

    if (length == 0 ||
        length >= value_size)
    {
      return false;
    }

    memcpy(
      value,
      value_start,
      length
    );

    value[length] = '\0';

    return true;
  }

  return false;
}


/**
 * @brief Remove redundant outer parentheses from a textual expression.
 *
 * @param[in,out] text
 *   Mutable expression text.
 *
 * @details
 * Repeatedly removes a pair of parentheses when that pair encloses the
 * complete expression.
 *
 * This helper is intended for simple integer macro values such as:
 *
 * @code
 * (0x0100)
 * @endcode
 */
static void
trim_outer_parentheses(
  char *text)
{
  size_t length;

  if (text == NULL)
    return;

  for (;;) {
    size_t depth = 0;
    bool entire = true;

    while (isspace((unsigned char)*text))
      ++text;

    length =
      strlen(text);

    while (length != 0 &&
           isspace(
             (unsigned char)text[length - 1]))
    {
      text[--length] = '\0';
    }

    if (length < 2 ||
        text[0] != '(' ||
        text[length - 1] != ')')
    {
      return;
    }

    for (size_t i = 0;
         i < length;
         ++i)
    {
      if (text[i] == '(')
        ++depth;
      else if (text[i] == ')') {
        if (depth == 0)
          return;

        --depth;

        if (depth == 0 &&
            i != length - 1)
        {
          entire = false;
          break;
        }
      }
    }

    if (!entire ||
        depth != 0)
    {
      return;
    }

    memmove(
      text,
      text + 1,
      length - 2
    );

    text[length - 2] = '\0';
  }
}


/**
 * @brief Parse a textual integer macro into a 32-bit unsigned value.
 *
 * @param[in] text
 *   Textual integer expression.
 *
 * @param[out] value
 *   Receives the parsed value.
 *
 * @return
 *   `true` when the text represents a valid value in the range
 *   `0..UINT32_MAX`; `false` otherwise.
 *
 * @details
 * The parser accepts the integer syntax understood by `strtoul()` with
 * base `0`, including common C integer suffixes:
 *
 * - `U` / `u`
 * - `L` / `l`
 *
 * Simple outer parentheses are also accepted.
 */
static bool
parse_integer_macro(
  const char *text,
  uint32_t *value)
{
  char buffer[AVR_PROBE_VALUE_SIZE];
  char *end;
  unsigned long parsed;

  if (text == NULL ||
      value == NULL)
    return false;

  if (strlen(text) >= sizeof(buffer))
    return false;

  strcpy(
    buffer,
    text
  );

  trim_outer_parentheses(buffer);

  errno = 0;

  parsed =
    strtoul(
      buffer,
      &end,
      0
    );

  if (errno != 0 ||
      end == buffer)
  {
    return false;
  }

  while (*end != '\0') {
    if (*end == 'U' ||
        *end == 'u' ||
        *end == 'L' ||
        *end == 'l')
    {
      ++end;
      continue;
    }

    if (isspace((unsigned char)*end)) {
      ++end;
      continue;
    }

    return false;
  }

  if (parsed > UINT32_MAX)
    return false;

  *value =
    (uint32_t)parsed;

  return true;
}


/**
 * @brief Find and parse a numeric preprocessor macro.
 *
 * @param[in] text
 *   Complete AVR-GCC macro output.
 *
 * @param[in] macro
 *   Name of the macro to find.
 *
 * @param[out] value
 *   Receives the parsed 32-bit value.
 *
 * @return
 *   `true` when the macro exists and contains a supported integer value;
 *   `false` otherwise.
 */
static bool
get_macro_u32(
  const char *text,
  const char *macro,
  uint32_t *value)
{
  char buffer[AVR_PROBE_VALUE_SIZE];

  if (!find_macro(
        text,
        macro,
        buffer,
        sizeof(buffer)))
  {
    return false;
  }

  return parse_integer_macro(
    buffer,
    value
  );
}


/**
 * @brief Validate and calculate the size of an inclusive address range.
 *
 * @param[in] start
 *   First address in the range.
 *
 * @param[in] end
 *   Last address in the range, inclusive.
 *
 * @param[out] size
 *   Receives `end - start + 1` when non-NULL.
 *
 * @return
 *   `true` when the range is valid and its size can be represented;
 *   `false` otherwise.
 */
static bool
checked_range(
  uint32_t start,
  uint32_t end,
  uint32_t *size)
{
  if (end < start)
    return false;

  if (end == UINT32_MAX)
    return false;

  if (size != NULL)
    *size =
      end - start + 1U;

  return true;
}


/**
 * @brief Extract physical FLASH, SRAM, and EEPROM regions.
 *
 * @param[in] macros
 *   AVR-GCC preprocessor output.
 *
 * @param[out] result
 *   Probe result to populate.
 *
 * @return
 *   `true` when all required physical-memory information is available
 *   and valid; `false` otherwise.
 *
 * @details
 * The extraction uses avr-libc macros:
 *
 * - `RAMSTART`
 * - `RAMEND`
 * - `FLASHSTART`
 * - `FLASHEND`
 * - `E2START`
 * - `E2END`
 *
 * `FLASHSTART` and `E2START` are optional and default to zero when they
 * are not provided by the selected device header.
 */
static bool
extract_physical_memory(
  const char *macros,
  AvrDeviceProbeResult *result)
{
  uint32_t ram_start;
  uint32_t ram_end;
  uint32_t flash_start;
  uint32_t flash_end;
  uint32_t eeprom_start;
  uint32_t eeprom_end;

  if (!get_macro_u32(
        macros,
        "RAMSTART",
        &ram_start
      ) ||
      !get_macro_u32(
        macros,
        "RAMEND",
        &ram_end
      ))
  {
    return false;
  }

  if (!checked_range(
        ram_start,
        ram_end,
        &result->sram_size
      ))
  {
    return false;
  }

  result->sram_start =
    ram_start;


  /*
   * FLASHSTART is not present in every avr-libc header.
   */
  if (!get_macro_u32(
        macros,
        "FLASHSTART",
        &flash_start
      ))
  {
    flash_start = 0;
  }

  if (!get_macro_u32(
        macros,
        "FLASHEND",
        &flash_end
      ))
  {
    return false;
  }

  if (!checked_range(
        flash_start,
        flash_end,
        &result->flash_size
      ))
  {
    return false;
  }

  result->flash_start =
    flash_start;


  /*
   * E2START is optional in older headers.
   */
  if (!get_macro_u32(
        macros,
        "E2START",
        &eeprom_start
      ))
  {
    eeprom_start = 0;
  }

  if (!get_macro_u32(
        macros,
        "E2END",
        &eeprom_end
      ))
  {
    return false;
  }

  if (!checked_range(
        eeprom_start,
        eeprom_end,
        &result->eeprom_size
      ))
  {
    return false;
  }

  result->eeprom_start =
    eeprom_start;

  return true;
}


/**
 * @brief Extract the SFR database from AVR-GCC preprocessor output.
 *
 * @param[in] macros
 *   Complete AVR-GCC preprocessor output.
 *
 * @param[out] result
 *   Probe result receiving the newly created SFR database.
 *
 * @return
 *   `true` when the SFR database is successfully initialized and parsed;
 *   `false` otherwise.
 *
 * @details
 * SFR discovery is deliberately an auxiliary capability.
 *
 * Failure to understand the selected avr-libc SFR representation does
 * not invalidate an otherwise usable physical-memory and linker
 * description.  The caller therefore logs SFR failure but continues
 * device initialization.
 */
static bool
extract_sfr_database(
  const char *macros,
  AvrDeviceProbeResult *result)
{
  AvrSfrDatabase *database = NULL;

  if (macros == NULL ||
      result == NULL)
  {
    return false;
  }

  if (!avr_sfr_init(&database))
    return false;

  if (!avr_sfr_parse(
        database,
        macros
      ))
  {
    avr_sfr_destroy(database);
    return false;
  }

  result->sfr_database =
    database;

  return true;
}


/**
 * @brief Generate a resolved AVR linker memory map.
 *
 * @param[in] device_name
 *   AVR device accepted by `avr-gcc -mmcu=`.
 *
 * @param[out] output
 *   Receives the complete generated linker map.
 *
 * @return
 *   `true` when a non-empty linker map is successfully generated;
 *   `false` otherwise.
 *
 * @details
 * A temporary C source file is compiled and linked using:
 *
 * @code
 * -mmcu=<device>
 * -nostartfiles
 * -nodefaultlibs
 * -x c
 * -Wl,-Map=<map>
 * @endcode
 *
 * The `-x c` option is important because the temporary source pathname
 * has no `.c` suffix.
 *
 * The generated map is subsequently parsed by
 * @ref extract_linker_regions.
 */
static bool
probe_linker_memory_map(
  const char *device_name,
  ProbeOutput *output)
{
  char source_path[PATH_MAX];
  char map_path[PATH_MAX];
  char command[AVR_PROBE_COMMAND_SIZE];

  FILE *source;
  FILE *pipe;
  FILE *map;

  char buffer[8192];

  int source_fd;
  int map_fd;

#ifdef DEBUG
  int status;
#endif

  const char *tmpdir;

  if (!valid_device_name(device_name) ||
      output == NULL)
  {
    return false;
  }


  /*
   * Use the POSIX temporary-directory convention.
   */
  tmpdir = getenv("TMPDIR");

  if (tmpdir == NULL || *tmpdir == '\0')
    tmpdir = "/tmp";

  if (snprintf(
        source_path,
        sizeof(source_path),
        "%s/avrmem-linker-probe-XXXXXX",
        tmpdir
      ) >= (int)sizeof(source_path))
  {
    return false;
  }

  if (snprintf(
        map_path,
        sizeof(map_path),
        "%s/avrmem-map-probe-XXXXXX",
        tmpdir
      ) >= (int)sizeof(map_path))
  {
    return false;
  }

  source_fd =
    mkstemp(source_path);

  if (source_fd < 0) {
    fprintf(
      stderr,
      "probe: mkstemp('%s') failed: %s\n",
      source_path,
      strerror(errno)
    );

    return false;
  }

  source =
    fdopen(
      source_fd,
      "w"
    );

  if (source == NULL) {
    fprintf(
      stderr,
      "probe: fdopen('%s') failed: %s\n",
      source_path,
      strerror(errno)
    );

    close(source_fd);
    unlink(source_path);

    return false;
  }

  if (fputs(
        "int main(void) { return 0; }\n",
        source
      ) == EOF)
  {
    fprintf(
      stderr,
      "probe: failed to write '%s': %s\n",
      source_path,
      strerror(errno)
    );

    fclose(source);
    unlink(source_path);

    return false;
  }

  if (fclose(source) != 0) {
    fprintf(
      stderr,
      "probe: fclose('%s') failed: %s\n",
      source_path,
      strerror(errno)
    );

    unlink(source_path);

    return false;
  }


  /*
   * Create the map pathname, then close it.  GNU ld will overwrite it.
   */
  map_fd =
    mkstemp(map_path);

  if (map_fd < 0) {
    fprintf(
      stderr,
      "probe: mkstemp('%s') failed: %s\n",
      map_path,
      strerror(errno)
    );

    unlink(source_path);

    return false;
  }

  close(map_fd);


  /*
   * This is the device-specific linker invocation.
   *
   * -mmcu selects the AVR linker emulation/specs.
   * -nostartfiles and -nodefaultlibs avoid pulling in the AVR runtime.
   * -x c forces the temporary file to be treated as C source.
   * -Wl,-Map requests the resolved linker map.
   */
  if (snprintf(
        command,
        sizeof(command),
        "avr-gcc "
        "-mmcu=%s "
        "-nostartfiles "
        "-nodefaultlibs "
        "-x c "
        "-Wl,-Map=%s "
        "\"%s\" "
        "-o /dev/null "
        "2>&1",
        device_name,
        map_path,
        source_path
        ) >= (int)sizeof(command))
  {
    unlink(source_path);
    unlink(map_path);

    return false;
  }

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: linker command:\n"
    "  %s\n",
    command
  );
#endif

  pipe =
    popen(
      command,
      "r"
    );

  if (pipe == NULL) {
    fprintf(
      stderr,
      "probe: popen() failed: %s\n",
      strerror(errno)
    );

    unlink(source_path);
    unlink(map_path);

    return false;
  }

  /*
   * Capture every byte of linker diagnostics.
   */
  while (!feof(pipe)) {
    size_t bytes;

    bytes =
      fread(
        buffer,
        1,
        sizeof(buffer) - 1,
        pipe
      );

    if (bytes == 0)
      break;

    buffer[bytes] = '\0';

#ifdef DEBUG
    fprintf(
      stderr,
      "%s",
      buffer
    );
#endif
  }

#ifdef DEBUG
  status =
#endif
    pclose(pipe);

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: linker exit status = %d\n",
    status
  );
#endif


  /*
   * Inspect the generated map even when the linker returned non-zero.
   * The final region-extraction stage determines whether the map contains
   * a usable MEMORY Configuration section.
   */
  map =
    fopen(
      map_path,
      "rb"
    );

  if (map == NULL) {
    fprintf(
      stderr,
      "probe: unable to open linker map '%s': %s\n",
      map_path,
      strerror(errno)
    );

    unlink(source_path);
    unlink(map_path);

    return false;
  }

  while (!feof(map)) {
    size_t bytes;

    bytes =
      fread(
        buffer,
        1,
        sizeof(buffer),
        map
      );

    if (bytes == 0)
      break;

    if (!probe_output_append(
          output,
          buffer,
          bytes
        ))
    {
      fprintf(
        stderr,
        "probe: linker map exceeds output limit\n"
      );

      fclose(map);
      unlink(source_path);
      unlink(map_path);

      return false;
    }
  }

  if (ferror(map)) {
    fprintf(
      stderr,
      "probe: error reading linker map '%s'\n",
      map_path
    );

    fclose(map);
    unlink(source_path);
    unlink(map_path);

    return false;
  }

  fclose(map);

  unlink(source_path);
  unlink(map_path);

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: linker map size = %zu bytes\n",
    output->size
  );
#endif

  return output->size != 0;
}


/**
 * @brief Parse a linker-map numeric value into a 32-bit unsigned integer.
 *
 * @param[in] text
 *   NUL-terminated numeric text.
 *
 * @param[out] value
 *   Receives the parsed value.
 *
 * @return
 *   `true` when the value is valid and fits in 32 bits; `false`
 *   otherwise.
 *
 * @details
 * The parser uses base `0`, allowing the normal C integer formats
 * emitted by GNU ld, including hexadecimal values.
 */
static bool
parse_map_u32(
  const char *text,
  uint32_t *value)
{
  char *end;
  unsigned long long parsed;

  if (text == NULL ||
      value == NULL)
    return false;

  errno = 0;

  parsed =
    strtoull(
      text,
      &end,
      0
    );

  if (errno != 0 ||
      end == text ||
      parsed > UINT32_MAX)
  {
    return false;
  }

  *value =
    (uint32_t)parsed;

  return true;
}


/**
 * @brief Parse one GNU ld Memory Configuration table row.
 *
 * @param[in] line
 *   One line from the generated linker map.
 *
 * @param[in] region_name
 *   Expected linker region name, such as `text`, `data`, or `eeprom`.
 *
 * @param[out] origin
 *   Receives the region origin.
 *
 * @param[out] length
 *   Receives the region length.
 *
 * @return
 *   `true` when the line describes the requested region and both numeric
 *   fields are valid; `false` otherwise.
 *
 * @details
 * The expected table format is approximately:
 *
 * @code
 * Name             Origin             Length             Attributes
 * text             0x0000000000000000 0x0000000000008000 xr
 * data             0x0000000000800100 0x0000000000000700 rw !x
 * eeprom           0x0000000000810000 0x0000000000000400 rw !x
 * @endcode
 *
 * Only the region name, origin, and length are relevant to the device
 * abstraction.
 */
static bool
parse_memory_configuration_line(
  const char *line,
  const char *region_name,
  uint32_t *origin,
  uint32_t *length)
{
  const char *p;
  const char *end;
  char token[128];
  size_t region_length;

  if (line == NULL ||
      region_name == NULL ||
      origin == NULL ||
      length == NULL)
  {
    return false;
  }

  p = line;

  while (isspace((unsigned char)*p))
    ++p;

  region_length =
    strlen(region_name);

  if (strncmp(
        p,
        region_name,
        region_length
      ) != 0)
  {
    return false;
  }

  if (!isspace(
        (unsigned char)p[region_length]
      ))
  {
    return false;
  }

  p += region_length;


  /*
   * Parse ORIGIN token.
   */
  while (isspace((unsigned char)*p))
    ++p;

  end = p;

  while (*end != '\0' &&
         !isspace((unsigned char)*end))
  {
    ++end;
  }

  if (end == p ||
      (size_t)(end - p) >= sizeof(token))
  {
    return false;
  }

  memcpy(
    token,
    p,
    (size_t)(end - p)
  );

  token[end - p] = '\0';

  if (!parse_map_u32(
        token,
        origin
      ))
  {
    return false;
  }

  p = end;


  /*
   * Parse LENGTH token.
   */
  while (isspace((unsigned char)*p))
    ++p;

  end = p;

  while (*end != '\0' &&
         !isspace((unsigned char)*end))
  {
    ++end;
  }

  if (end == p ||
      (size_t)(end - p) >= sizeof(token))
  {
    return false;
  }

  memcpy(
    token,
    p,
    (size_t)(end - p)
  );

  token[end - p] = '\0';

  if (!parse_map_u32(
        token,
        length
      ))
  {
    return false;
  }

  return true;
}


/**
 * @brief Extract AVR-GCC linker memory regions from a generated map.
 *
 * @param[in] map_output
 *   Complete generated linker map.
 *
 * @param[out] result
 *   Probe result receiving the resolved linker region information.
 *
 * @return
 *   `true` when TEXT, DATA, and EEPROM regions are all found and parsed;
 *   `false` otherwise.
 *
 * @details
 * The parser searches for the GNU ld:
 *
 * @code
 * Memory Configuration
 * @endcode
 *
 * table and extracts the three regions required by the current device
 * abstraction:
 *
 * - `text`;
 * - `data`;
 * - `eeprom`.
 */
static bool
extract_linker_regions(
  const char *map_output,
  AvrDeviceProbeResult *result)
{
  const char *line;

  bool in_memory_configuration = false;

  bool have_text = false;
  bool have_data = false;
  bool have_eeprom = false;

  if (map_output == NULL ||
      result == NULL)
  {
    return false;
  }

  line =
    map_output;

  while (*line != '\0') {
    const char *line_end;
    size_t length;
    char buffer[AVR_PROBE_LINE_SIZE];

    line_end =
      strchr(
        line,
        '\n'
      );

    if (line_end == NULL)
      length =
        strlen(line);
    else
      length =
        (size_t)(line_end - line);

    if (length >= sizeof(buffer))
      return false;

    memcpy(
      buffer,
      line,
      length
    );

    buffer[length] = '\0';

    while (length != 0 &&
           (buffer[length - 1] == '\r' ||
            buffer[length - 1] == '\n'))
    {
      buffer[--length] = '\0';
    }

    {
      const char *p =
        buffer;

      while (isspace((unsigned char)*p))
        ++p;

      if (strcmp(
            p,
            "Memory Configuration"
          ) == 0)
      {
        in_memory_configuration = true;

        if (line_end == NULL)
          break;

        line =
          line_end + 1;

        continue;
      }
    }

    if (in_memory_configuration) {
      /*
       * Ignore headings and blank lines.  Only the three named regions
       * can satisfy parse_memory_configuration_line().
       */
      if (!have_text &&
          parse_memory_configuration_line(
            buffer,
            "text",
            &result->text_region_origin,
            &result->text_region_length
          ))
      {
        have_text = true;
      }

      if (!have_data &&
          parse_memory_configuration_line(
            buffer,
            "data",
            &result->data_region_origin,
            &result->data_region_length
          ))
      {
        have_data = true;
      }

      if (!have_eeprom &&
          parse_memory_configuration_line(
            buffer,
            "eeprom",
            &result->eeprom_region_origin,
            &result->eeprom_region_length
          ))
      {
        have_eeprom = true;
      }

      if (have_text &&
          have_data &&
          have_eeprom)
      {
        return true;
      }
    }

    if (line_end == NULL)
      break;

    line =
      line_end + 1;
  }

  fprintf(
    stderr,
    "probe: Memory Configuration parse failed "
    "(text=%d data=%d eeprom=%d)\n",
    have_text,
    have_data,
    have_eeprom
  );

  return false;
}


/**
 * @brief Normalize the canonical device name emitted by AVR-GCC.
 *
 * @param[in,out] name
 *   Mutable device-name buffer.
 *
 * @return
 *   `true` when the normalized name is non-empty; `false` otherwise.
 *
 * @details
 * AVR-GCC normally emits the canonical name without quotes, for example:
 *
 * @code
 * #define __AVR_DEVICE_NAME__ atmega328p
 * @endcode
 *
 * Some toolchains may emit a quoted form.  A surrounding pair of quotes
 * is therefore removed before the name is used.
 */
static bool
normalize_device_name(
  char *name)
{
  size_t length;

  if (name == NULL)
    return false;

  length =
    strlen(name);

  if (length >= 2 &&
      name[0] == '"' &&
      name[length - 1] == '"')
  {
    memmove(
      name,
      name + 1,
      length - 2
    );

    name[length - 2] = '\0';
  }

  return name[0] != '\0';
}


/**
 * @brief Probe and construct a complete AVR device description.
 *
 * @param[in] device_name
 *   AVR device name accepted by AVR-GCC.
 *
 * @param[out] result
 *   Probe result to populate.
 *
 * @return
 *   `true` when required device information is successfully discovered;
 *   `false` otherwise.
 *
 * @details
 * Discovery proceeds in the following stages:
 *
 * 1. AVR-GCC/avr-libc macro probe;
 * 2. canonical device-name extraction;
 * 3. physical-memory extraction;
 * 4. optional SFR extraction;
 * 5. linker-map generation;
 * 6. linker-region extraction;
 * 7. FLASH addressing-model initialization;
 * 8. device-name ownership transfer.
 *
 * SFR discovery is explicitly auxiliary.  Failure to parse the selected
 * avr-libc SFR definitions produces a diagnostic but does not invalidate
 * the remainder of the device description.
 *
 * On failure, the result is cleaned up before returning.
 */
bool
avr_device_probe(
  const char *device_name,
  AvrDeviceProbeResult *result)
{
  ProbeOutput macros = { 0 };
  ProbeOutput linker_map = { 0 };

  char canonical_name[128];

  if (result == NULL ||
      !valid_device_name(device_name))
  {
    fprintf(
      stderr,
      "probe: invalid device-probe arguments\n"
    );

    return false;
  }

  memset(
    result,
    0,
    sizeof(*result)
  );


  /*
   * --------------------------------------------------------------
   * AVR-LibC / preprocessor
   * --------------------------------------------------------------
   */

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: probing '%s'\n",
    device_name
  );
#endif

  if (!probe_avr_libc_header(
        device_name,
        &macros
      ))
  {
    fprintf(
      stderr,
      "probe: AVR-LibC macro probe FAILED\n"
    );

    goto fail;
  }

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: macro output = %zu bytes\n",
    macros.size
  );
#endif


  /*
   * --------------------------------------------------------------
   * Canonical device name
   * --------------------------------------------------------------
   */

  if (!find_macro(
        macros.data,
        "__AVR_DEVICE_NAME__",
        canonical_name,
        sizeof(canonical_name)
      ))
  {
    fprintf(
      stderr,
      "probe: __AVR_DEVICE_NAME__ not found\n"
    );

    goto fail;
  }

  if (!normalize_device_name(
        canonical_name
      ))
  {
    fprintf(
      stderr,
      "probe: invalid canonical device name '%s'\n",
      canonical_name
    );

    goto fail;
  }

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: canonical device = %s\n",
    canonical_name
  );
#endif


  /*
   * --------------------------------------------------------------
   * Physical memories
   * --------------------------------------------------------------
   */

  if (!extract_physical_memory(
        macros.data,
        result
      ))
  {
    fprintf(
      stderr,
      "probe: physical-memory extraction FAILED\n"
    );

    goto fail;
  }

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: physical memory:\n"
    "       FLASH  start=0x%08" PRIx32
    " size=0x%08" PRIx32 "\n"
    "       SRAM   start=0x%08" PRIx32
    " size=0x%08" PRIx32 "\n"
    "       EEPROM start=0x%08" PRIx32
    " size=0x%08" PRIx32 "\n",
    result->flash_start,
    result->flash_size,
    result->sram_start,
    result->sram_size,
    result->eeprom_start,
    result->eeprom_size
  );
#endif


  /*
   * --------------------------------------------------------------
   * SFR database
   * --------------------------------------------------------------
   *
   * This is auxiliary.  Never prevent the device from loading merely
   * because the SFR parser cannot understand a particular avr-libc
   * revision.
   */

  if (!extract_sfr_database(
        macros.data,
        result
      ))
  {
    fprintf(
      stderr,
      "probe: SFR discovery unavailable; continuing\n"
    );
  }
#ifdef DEBUG
  else {
    fprintf(
      stderr,
      "probe: SFR database = %zu registers\n",
      avr_sfr_count(
        result->sfr_database
      )
    );
  }
#endif


  /*
   * --------------------------------------------------------------
   * Linker memory map
   * --------------------------------------------------------------
   */

  if (!probe_linker_memory_map(
        device_name,
        &linker_map
      ))
  {
    fprintf(
      stderr,
      "probe: linker-map probe FAILED\n"
    );

    goto fail;
  }

  if (!extract_linker_regions(
        linker_map.data,
        result
      ))
  {
    fprintf(
      stderr,
      "probe: linker-region extraction FAILED\n"
    );

    goto fail;
  }

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: linker regions:\n"
    "       TEXT   origin=0x%08" PRIx32
    " length=0x%08" PRIx32 "\n"
    "       DATA   origin=0x%08" PRIx32
    " length=0x%08" PRIx32 "\n"
    "       EEPROM origin=0x%08" PRIx32
    " length=0x%08" PRIx32 "\n",
    result->text_region_origin,
    result->text_region_length,
    result->data_region_origin,
    result->data_region_length,
    result->eeprom_region_origin,
    result->eeprom_region_length
  );
#endif


  /*
   * --------------------------------------------------------------
   * Program-memory representation
   * --------------------------------------------------------------
   *
   * Classic AVR instructions are 16 bits wide.
   *
   * The AVR ELF representation is byte addressed, while the CPU
   * instruction address is word addressed.
   */
  result->flash_address_shift =
    1;


  /*
   * --------------------------------------------------------------
   * Device-name ownership
   * --------------------------------------------------------------
   */

  result->name =
    strdup(canonical_name);

  if (result->name == NULL) {
    fprintf(
      stderr,
      "probe: device-name allocation FAILED\n"
    );

    goto fail;
  }

  probe_output_destroy(
    &macros
  );

  probe_output_destroy(
    &linker_map
  );

#ifdef DEBUG
  fprintf(
    stderr,
    "probe: SUCCESS\n"
  );
#endif

  return true;


fail:

  probe_output_destroy(
    &macros
  );

  probe_output_destroy(
    &linker_map
  );

  avr_device_probe_destroy(
    result
  );

  return false;
}


/**
 * @brief Release all dynamically allocated resources in a probe result.
 *
 * @param[in,out] result
 *   Probe result to destroy.
 *
 * @details
 * Releases:
 *
 * - the canonical device name;
 * - the discovered SFR database;
 *
 * and resets the complete result structure to zero.
 *
 * @note
 * Passing `NULL` is safe.
 */
void
avr_device_probe_destroy(
  AvrDeviceProbeResult *result)
{
  if (result == NULL)
    return;

  free(result->name);

  avr_sfr_destroy(
    result->sfr_database
  );

  memset(
    result,
    0,
    sizeof(*result)
  );
}
