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
 * @file avrmem.c
 * @author notweerdmonk, gpt-5.6-luna
 * @brief AVR ELF memory, section, and symbol explorer frontend.
 *
 * @details
 * This translation unit implements the command-line frontend for:
 *
 * - @c avr_elf.c
 * - @c avr_device.c
 * - @c avr_device_probe.c
 * - @c avr_sfr.c
 *
 * The frontend deliberately contains neither ELF parsing logic nor
 * MCU-specific memory constants. Those responsibilities belong to the
 * ELF and device abstractions respectively.
 *
 * Command-line options must precede the ELF filename. The canonical form is:
 *
 * @code
 * avrmem [options] <elf>
 * @endcode
 *
 * Supported options include:
 *
 * - @c --device <name> : explicitly select the AVR device;
 * - @c --memory : display the selected device memory model;
 * - @c --sections : display ELF sections with VMA/LMA and device mapping;
 * - @c --symbols : display useful ELF symbols with device mapping;
 * - @c --symbol <name> : display detailed information for one symbol;
 * - @c --map : display the complete memory map and section placement;
 * - @c --help : display usage information.
 *
 * When @c --device is omitted, the device name is obtained from the AVR
 * ELF @c .note.gnu.avr.deviceinfo metadata.
 *
 * Example invocations:
 *
 * @code
 * avrmem firmware.elf
 * avrmem --memory firmware.elf
 * avrmem --sections firmware.elf
 * avrmem --symbols firmware.elf
 * avrmem --symbol counter firmware.elf
 * avrmem --map firmware.elf
 * avrmem --device atmega328p firmware.elf
 * @endcode
 */

#include "avr_device.h"
#include "avr_elf.h"

#include <inttypes.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 * ----------------------------------------------------------------------
 * Command-line state
 * ----------------------------------------------------------------------
 */

/**
 * @struct CommandLine
 * @brief Parsed command-line state.
 *
 * @details
 * String members point into the original @c argv storage and are not owned
 * by this structure. Boolean members select the requested output views.
 */
typedef struct {
  const char *device_name;

  const char *symbol_name;

  const char *elf_filename;

  bool show_memory;
  bool show_sections;
  bool show_symbols;
  bool show_symbol;
  bool show_map;

} CommandLine;


/*
 * ----------------------------------------------------------------------
 * Utility helpers
 * ----------------------------------------------------------------------
 */

/**
 * @brief Return the basename component of a program pathname.
 * @param[in] program Program pathname, typically @c argv[0].
 * @return Pointer to the final path component, or @c "avrmem" when
 *   @p program is @c NULL.
 */
static const char *
program_basename(
  const char *program)
{
  const char *slash;

  if (program == NULL)
    return "avrmem";

  slash =
    strrchr(
      program,
      '/'
    );

  if (slash != NULL)
    return slash + 1;

  return program;
}


/**
 * @brief Add two 32-bit unsigned values with overflow detection.
 * @param[in] a First operand.
 * @param[in] b Second operand.
 * @param[out] result Optional destination for the sum.
 * @return @c true when the sum fits in @c uint32_t; @c false on overflow.
 */
static bool
__attribute__((unused))
u32_add(
  uint32_t a,
  uint32_t b,
  uint32_t *result)
{
  if (b > UINT32_MAX - a)
    return false;

  if (result != NULL)
    *result = a + b;

  return true;
}


/*
 * Print a half-open region as an inclusive address range.
 */
/**
 * @brief Convert a half-open region to its inclusive final address.
 * @param[in] start Region start address.
 * @param[in] size Region size in bytes.
 * @param[out] end Optional destination for @c start + size - 1.
 * @return @c true when the region is non-empty and representable;
 *   @c false otherwise.
 */
static bool
region_end(
  uint32_t start,
  uint32_t size,
  uint32_t *end)
{
  if (size == 0)
    return false;

  if (size - 1U >
      UINT32_MAX - start)
  {
    return false;
  }

  if (end != NULL)
    *end =
      start + size - 1U;

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Usage
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print command-line usage information.
 * @param[in] program Program pathname used to derive the displayed name.
 */
static void
print_usage(
  const char *program)
{
  program =
    program_basename(program);

  printf(
    "Usage:\n"
    "\n"
    "  %s [options] <elf>\n"
    "\n"
    "Options:\n"
    "\n"
    "  --device <name>\n"
    "      Explicitly select the AVR device.\n"
    "\n"
    "  --memory\n"
    "      Show the selected AVR memory model.\n"
    "\n"
    "  --sections\n"
    "      Show ELF sections with VMA/LMA and device mapping.\n"
    "\n"
    "  --symbols\n"
    "      Show useful symbols with AVR addresses.\n"
    "\n"
    "  --symbol <name>\n"
    "      Show detailed information about one symbol.\n"
    "\n"
    "  --map\n"
    "      Show the complete AVR memory map.\n"
    "\n"
    "  --help\n"
    "      Show this help text.\n"
    "\n"
    "If --device is omitted, the AVR device name is inferred from\n"
    "the ELF .note.gnu.avr.deviceinfo metadata.\n"
    "\n"
    "Examples:\n"
    "\n"
    "  %s firmware.elf\n"
    "  %s --memory firmware.elf\n"
    "  %s --sections firmware.elf\n"
    "  %s --symbols firmware.elf\n"
    "  %s --symbol counter firmware.elf\n"
    "  %s --map firmware.elf\n"
    "  %s --device atmega328p firmware.elf\n",
    program,
    program,
    program,
    program,
    program,
    program,
    program,
    program
  );
}


/*
 * ----------------------------------------------------------------------
 * Option parsing
 * ----------------------------------------------------------------------
 *
 * Options are intentionally parsed only before the ELF filename.
 *
 * Supported forms:
 *
 *     --device atmega328p
 *     --device=atmega328p
 *     --symbol counter
 *     --symbol=counter
 *
 * A standalone "--" terminates option parsing.
 */
/**
 * @typedef parse_options_fn
 * @brief Function-pointer type for command-line parsers.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param command Destination command-line state.
 * @return @c true when parsing succeeds; @c false otherwise.
 */
typedef bool (*parse_options_fn)(int, char**, CommandLine*);

/**
 * @brief Parse command-line options using the project's native parser.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] command Parsed command-line state.
 * @return @c true on success; @c false when the command line is invalid.
 *
 * @details This implementation is retained as a compatibility/reference
 * parser; the active frontend uses @ref parse_options_posix.
 */
static bool
__attribute__((unused))
parse_options_native(
  int argc,
  char **argv,
  CommandLine *command)
{
  int i;
  bool found_elf = false;

  if (command == NULL)
    return false;

  memset(
    command,
    0,
    sizeof(*command)
  );

  for (i = 1;
       i < argc;
       ++i)
  {
    const char *arg =
      argv[i];

    if (arg == NULL)
      return false;

    /*
     * The first non-option argument is the ELF filename.
     *
     * Everything after it must be absent; this enforces the chosen
     * command-line convention that options precede the ELF filename.
     */
    if (found_elf) {
      fprintf(
        stderr,
        "error: unexpected argument after ELF file: %s\n",
        arg
      );

      return false;
    }

    if (strcmp(arg, "--") == 0) {
      if (i + 1 >= argc) {
        fprintf(
          stderr,
          "error: missing ELF filename\n"
        );

        return false;
      }

      if (i + 2 != argc) {
        fprintf(
          stderr,
          "error: exactly one ELF filename is required\n"
        );

        return false;
      }

      command->elf_filename =
        argv[i + 1];

      found_elf = true;
      break;
    }


    /*
     * Long options.
     */
    if (arg[0] == '-' &&
        arg[1] == '-') {

      const char *equals =
        strchr(
          arg + 2,
          '='
        );

      size_t option_length;

      if (equals != NULL)
        option_length =
          (size_t)(equals - (arg + 2));
      else
        option_length =
          strlen(arg + 2);


      /*
       * --help
       */
      if (option_length == 4 &&
          strncmp(
            arg + 2,
            "help",
            4
          ) == 0)
      {
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
      }


      /*
       * --memory
       */
      if (option_length == 6 &&
          strncmp(
            arg + 2,
            "memory",
            6
          ) == 0)
      {
        if (equals != NULL) {
          fprintf(
            stderr,
            "error: --memory does not take a value\n"
          );

          return false;
        }

        command->show_memory = true;
        continue;
      }


      /*
       * --sections
       */
      if (option_length == 8 &&
          strncmp(
            arg + 2,
            "sections",
            8
          ) == 0)
      {
        if (equals != NULL) {
          fprintf(
            stderr,
            "error: --sections does not take a value\n"
          );

          return false;
        }

        command->show_sections = true;
        continue;
      }


      /*
       * --symbols
       */
      if (option_length == 7 &&
          strncmp(
            arg + 2,
            "symbols",
            7
          ) == 0)
      {
        if (equals != NULL) {
          fprintf(
            stderr,
            "error: --symbols does not take a value\n"
          );

          return false;
        }

        command->show_symbols = true;
        continue;
      }


      /*
       * --map
       */
      if (option_length == 3 &&
          strncmp(
            arg + 2,
            "map",
            3
          ) == 0)
      {
        if (equals != NULL) {
          fprintf(
            stderr,
            "error: --map does not take a value\n"
          );

          return false;
        }

        command->show_map = true;
        continue;
      }


      /*
       * --device
       */
      if (option_length == 6 &&
          strncmp(
            arg + 2,
            "device",
            6
          ) == 0)
      {
        const char *value;

        if (equals != NULL) {
          value =
            equals + 1;

          if (*value == '\0') {
            fprintf(
              stderr,
              "error: --device requires a device name\n"
            );

            return false;
          }
        }
        else {
          if (i + 1 >= argc) {
            fprintf(
              stderr,
              "error: --device requires a device name\n"
            );

            return false;
          }

          value =
            argv[++i];
        }

        if (command->device_name != NULL) {
          fprintf(
            stderr,
            "error: --device specified more than once\n"
          );

          return false;
        }

        command->device_name =
          value;

        continue;
      }


      /*
       * --symbol
       */
      if (option_length == 6 &&
          strncmp(
            arg + 2,
            "symbol",
            6
          ) == 0)
      {
        const char *value;

        if (equals != NULL) {
          value =
            equals + 1;

          if (*value == '\0') {
            fprintf(
              stderr,
              "error: --symbol requires a symbol name\n"
            );

            return false;
          }
        }
        else {
          if (i + 1 >= argc) {
            fprintf(
              stderr,
              "error: --symbol requires a symbol name\n"
            );

            return false;
          }

          value =
            argv[++i];
        }

        if (command->show_symbol) {
          fprintf(
            stderr,
            "error: --symbol specified more than once\n"
          );

          return false;
        }

        command->symbol_name =
          value;

        command->show_symbol =
          true;

        continue;
      }


      fprintf(
        stderr,
        "error: unknown option: %s\n",
        arg
      );

      return false;
    }


    /*
     * Short option retained for the standard help convention.
     */
    if (strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      exit(EXIT_SUCCESS);
    }

    if (arg[0] == '-') {
      fprintf(
        stderr,
        "error: unknown option: %s\n",
        arg
      );

      return false;
    }


    /*
     * This is the ELF filename.
     */
    if (i + 1 != argc) {
      /*
       * Any following argument would violate the option-before-file
       * convention and the single-ELF-file requirement.
       */
      fprintf(
        stderr,
        "error: exactly one ELF filename is required\n"
      );

      return false;
    }

    command->elf_filename =
      arg;

    found_elf = true;
  }

  if (!found_elf ||
      command->elf_filename == NULL ||
      *command->elf_filename == '\0')
  {
    fprintf(
      stderr,
      "error: missing ELF filename\n"
    );

    return false;
  }

  return true;
}

/**
 * @brief Parse command-line options using POSIX/GNU @c getopt_long.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[out] command Parsed command-line state.
 * @return @c true on success; @c false when the command line is invalid.
 *
 * @details The leading @c + in the option string stops option processing
 * at the first non-option argument, enforcing the project convention that
 * all options precede the ELF filename.
 */
static bool
__attribute__((unused))
parse_options_posix(
  int argc,
  char **argv,
  CommandLine *command)
{
  /*
   * getopt_long() option table.
   *
   * The leading '+' in the short-option string is intentional:
   *
   *     getopt() / getopt_long()
   *         stop option processing at the first non-option argument.
   *
   * This enforces our command-line convention:
   *
   *     avrmem [options] <elf>
   *
   * and rejects:
   *
   *     avrmem firmware.elf --symbols
   */
  static const struct option long_options[] = {
    {
      "device",
      required_argument,
      NULL,
      'd'
    },
    {
      "symbol",
      required_argument,
      NULL,
      'y'
    },
    {
      "memory",
      no_argument,
      NULL,
      'm'
    },
    {
      "sections",
      no_argument,
      NULL,
      'c'
    },
    {
      "symbols",
      no_argument,
      NULL,
      's'
    },
    {
      "map",
      no_argument,
      NULL,
      'p'
    },
    {
      "help",
      no_argument,
      NULL,
      'h'
    },
    {
      NULL,
      0,
      NULL,
      0
    }
  };

  /*
   * '+' is required here to prevent GNU getopt_long() from permuting
   * non-option arguments to the end of argv.
   *
   * Thus the ELF filename is the first non-option argument, and all
   * options must precede it.
   */
  const char *optstring =
    "+d:y:mcsp:h";

  int option;

  if (command == NULL)
    return false;

  memset(
    command,
    0,
    sizeof(*command)
  );

  opterr = 0;

  /*
   * Reset getopt state so this function behaves predictably even if it
   * is called more than once during testing.
   */
  optind = 1;

  for (;;) {
    int option_index = 0;

    option =
      getopt_long(
        argc,
        argv,
        optstring,
        long_options,
        &option_index
      );

    if (option == -1)
      break;

    switch (option) {

      /*
       * ------------------------------------------------------------
       * --device <name>
       * ------------------------------------------------------------
       */
      case 'd':
        if (optarg == NULL ||
            *optarg == '\0')
        {
          fprintf(
            stderr,
            "error: --device requires a device name\n"
          );

          return false;
        }

        if (command->device_name != NULL) {
          fprintf(
            stderr,
            "error: --device specified more than once\n"
          );

          return false;
        }

        command->device_name =
          optarg;

        break;


      /*
       * ------------------------------------------------------------
       * --symbol <name>
       * ------------------------------------------------------------
       */
      case 'y':
        if (optarg == NULL ||
            *optarg == '\0')
        {
          fprintf(
            stderr,
            "error: --symbol requires a symbol name\n"
          );

          return false;
        }

        if (command->show_symbol) {
          fprintf(
            stderr,
            "error: --symbol specified more than once\n"
          );

          return false;
        }

        command->symbol_name =
          optarg;

        command->show_symbol =
          true;

        break;


      /*
       * ------------------------------------------------------------
       * Display flags
       * ------------------------------------------------------------
       */

      case 'm':
        command->show_memory = true;
        break;

      case 'c':
        command->show_sections = true;
        break;

      case 's':
        command->show_symbols = true;
        break;

      case 'p':
        command->show_map = true;
        break;


      /*
       * ------------------------------------------------------------
       * Help
       * ------------------------------------------------------------
       */
      case 'h':
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);


      /*
       * ------------------------------------------------------------
       * Unknown option / malformed option argument
       * ------------------------------------------------------------
       *
       * getopt_long() returns '?' for:
       *
       *   - unknown options
       *   - missing required arguments
       */
      case '?':
      default:
        if (optind > 0 &&
            optind <= argc)
        {
          fprintf(
            stderr,
            "error: invalid option near '%s'\n",
            argv[optind - 1]
          );
        }
        else {
          fprintf(
            stderr,
            "error: invalid command-line option\n"
          );
        }

        return false;
    }
  }


  /*
   * --------------------------------------------------------------
   * ELF filename
   * --------------------------------------------------------------
   *
   * Because '+' was used in optstring, getopt_long() stops at the
   * first non-option argument.
   */
  if (optind >= argc) {
    fprintf(
      stderr,
      "error: missing ELF filename\n"
    );

    return false;
  }

  command->elf_filename =
    argv[optind];


  /*
   * Exactly one non-option argument must remain.
   */
  if (optind + 1 != argc) {
    fprintf(
      stderr,
      "error: exactly one ELF filename is required\n"
    );

    return false;
  }

  return true;
}


/*
 * ----------------------------------------------------------------------
 * Basic ELF information
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print basic ELF and selected-device information.
 * @param[in] filename ELF filename supplied by the user.
 * @param[in] device Selected AVR device.
 * @param[in] elf Parsed ELF object.
 */
static void
print_basic_information(
  const char *filename,
  const AvrDevice *device,
  const AvrElf *elf)
{
  const char *device_name;

  printf(
    "ELF:       %s\n",
    filename
  );

  printf(
    "Class:     ELF32\n"
  );

  printf(
    "Endian:    little-endian\n"
  );

  printf(
    "Machine:   0x%04" PRIx16 "\n",
    avr_elf_machine(elf)
  );

  printf(
    "Entry:     0x%08" PRIx32 "\n",
    avr_elf_entry(elf)
  );

  printf(
    "Sections:  %zu\n",
    avr_elf_section_count(elf)
  );

  printf(
    "Symbols:   %zu\n",
    avr_elf_symbol_count(elf)
  );

  device_name =
    avr_device_name(device);

  printf(
    "Device:    %s\n",
    device_name != NULL
      ? device_name
      : "<unknown>"
  );
}


/*
 * ----------------------------------------------------------------------
 * Memory model
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print one physical memory region as an inclusive address range.
 * @param[in] name Human-readable region name.
 * @param[in] region Physical region to display.
 */
static void
print_physical_region(
  const char *name,
  AvrMemoryRegion region)
{
  uint32_t end;

  if (region_end(
        region.start,
        region.size,
        &end))
  {
    printf(
      "  %-12s 0x%08" PRIx32
      " - 0x%08" PRIx32
      "  (%" PRIu32 " bytes)\n",
      name,
      region.start,
      end,
      region.size
    );
  }
  else {
    printf(
      "  %-12s <empty>\n",
      name
    );
  }
}


/**
 * @brief Print one ELF/linker address-space region.
 * @param[in] name Human-readable region name.
 * @param[in] origin Linker-space origin.
 * @param[in] length Linker-space length in bytes.
 */
static void
print_linker_space(
  const char *name,
  uint32_t origin,
  uint32_t length)
{
  uint32_t end;

  if (region_end(
        origin,
        length,
        &end))
  {
    printf(
      "  %-12s 0x%08" PRIx32
      " - 0x%08" PRIx32
      "  (%" PRIu32 " bytes)\n",
      name,
      origin,
      end,
      length
    );
  }
  else {
    printf(
      "  %-12s <empty>\n",
      name
    );
  }
}


/**
 * @brief Print the complete selected-device memory model.
 * @param[in] device Device description whose physical and linker spaces
 *   are displayed.
 *
 * @details Displays physical memories, ELF/linker spaces, AVR DATA-space
 * layout, FLASH address representation, and SFR database size.
 */
static void
print_memory_model(
  const AvrDevice *device)
{
  AvrMemoryRegion flash;
  AvrMemoryRegion sram;
  AvrMemoryRegion eeprom;

  flash =
    avr_device_flash_region(device);

  sram =
    avr_device_sram_region(device);

  eeprom =
    avr_device_eeprom_region(device);

  printf(
    "\nDevice memory model\n"
    "===================\n"
  );

  printf(
    "\nDevice\n"
    "------\n"
  );

  printf(
    "  Name:        %s\n",
    avr_device_name(device) != NULL
      ? avr_device_name(device)
      : "<unknown>"
  );

  printf(
    "\nPhysical memories\n"
    "-----------------\n"
  );

  print_physical_region(
    "FLASH",
    flash
  );

  print_physical_region(
    "SRAM",
    sram
  );

  print_physical_region(
    "EEPROM",
    eeprom
  );

  printf(
    "\nELF/linker spaces\n"
    "-----------------\n"
  );

  print_linker_space(
    "TEXT",
    avr_device_text_region_origin(device),
    avr_device_text_region_length(device)
  );

  print_linker_space(
    "DATA",
    avr_device_data_region_origin(device),
    avr_device_data_region_length(device)
  );

  print_linker_space(
    "EEPROM",
    avr_device_eeprom_region_origin(device),
    avr_device_eeprom_region_length(device)
  );

  printf(
    "\nAVR DATA-space layout\n"
    "---------------------\n"
  );

  print_physical_region(
    "Registers",
    device->register_file
  );

  print_physical_region(
    "I/O",
    device->io
  );

  print_physical_region(
    "Extended I/O",
    device->ext_io
  );

  print_physical_region(
    "SRAM",
    device->sram
  );

  printf(
    "\nFlash address representation\n"
    "----------------------------\n"
  );

  printf(
    "  Byte -> CPU word shift: %u\n",
    avr_device_flash_address_shift(device)
  );

  printf(
    "\nSFR database\n"
    "------------\n"
  );

  printf(
    "  Registers:   %zu\n",
    device->sfr_database != NULL
      ? avr_sfr_count(device->sfr_database)
      : 0
  );
}


/*
 * ----------------------------------------------------------------------
 * Section flags
 * ----------------------------------------------------------------------
 */

/**
 * @brief Build a compact textual representation of ELF section flags.
 * @param[in] section Section whose flags are inspected.
 * @param[out] buffer Destination character buffer.
 * @param[in] buffer_size Size of @p buffer in bytes.
 *
 * @details The current representation uses @c A for allocatable, @c W for
 * writable, and @c X for executable sections.
 */
static void
section_flags(
  const AvrElfSection *section,
  char *buffer,
  size_t buffer_size)
{
  size_t length = 0;

  if (buffer == NULL ||
      buffer_size == 0)
    return;

  if (section == NULL) {
    buffer[0] = '\0';
    return;
  }

  if (section->alloc &&
      length + 1 < buffer_size)
  {
    buffer[length++] = 'A';
  }

  if (section->writable &&
      length + 1 < buffer_size)
  {
    buffer[length++] = 'W';
  }

  if (section->executable &&
      length + 1 < buffer_size)
  {
    buffer[length++] = 'X';
  }

  buffer[length] =
    '\0';
}


/*
 * ----------------------------------------------------------------------
 * Section listing
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print all ELF sections with device-resolved address information.
 * @param[in] elf Parsed ELF object.
 * @param[in] device Selected AVR device.
 *
 * @details Each section is resolved through @ref avr_elf_resolve_section,
 * keeping ELF and device address semantics outside this frontend.
 */
static void
print_sections(
  const AvrElf *elf,
  const AvrDevice *device)
{
  size_t count;

  count =
    avr_elf_section_count(elf);

  printf(
    "\nSections\n"
    "========\n\n"
  );

  printf(
    "%-4s %-24s %-10s %-10s %-10s %-12s %-10s\n",
    "Idx",
    "Name",
    "Size",
    "VMA",
    "LMA",
    "Memory",
    "Flags"
  );

  printf(
    "------------------------------------------------------------------------------------------\n"
  );

  for (size_t i = 0;
       i < count;
       ++i)
  {
    const AvrElfSection *section;
    AvrResolvedSection resolved;
    char flags[8];

    section =
      avr_elf_section_at(
        elf,
        (uint16_t)i
      );

    if (section == NULL)
      continue;

    memset(
      &resolved,
      0,
      sizeof(resolved)
    );

    section_flags(
      section,
      flags,
      sizeof(flags)
    );

    if (!avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)i,
          &resolved))
    {
      printf(
        "%-4zu %-24s 0x%08" PRIx32
        " 0x%08" PRIx32
        " %-10s %-12s %-10s\n",
        i,
        section->name,
        section->size,
        section->addr,
        "-",
        "UNKNOWN",
        flags
      );

      continue;
    }

    printf(
      "%-4zu %-24s 0x%08" PRIx32
      " 0x%08" PRIx32,
      i,
      section->name,
      section->size,
      section->addr
    );

    if (resolved.has_lma)
      printf(
        " 0x%08" PRIx32,
        resolved.lma
      );
    else
      printf(
        " %-10s",
        "-"
      );

    printf(
      " %-12s %-10s\n",
      avr_memory_space_name(
        resolved.memory_space
      ),
      flags
    );

    if (resolved.has_physical_address) {
      printf(
        "      physical=0x%08" PRIx32
        "\n",
        resolved.physical_address
      );
    }
  }
}


/*
 * ----------------------------------------------------------------------
 * Symbol formatting helpers
 * ----------------------------------------------------------------------
 */

/**
 * @brief Return a human-readable ELF symbol type name.
 * @param[in] symbol ELF symbol whose type is examined.
 * @return Constant textual type name.
 */
static const char *
symbol_type_name(
  const AvrElfSymbol *symbol)
{
  if (symbol == NULL)
    return "UNKNOWN";

  switch (symbol->type) {
    case 0:
      return "NOTYPE";

    case 1:
      return "OBJECT";

    case 2:
      return "FUNC";

    case 3:
      return "SECTION";

    case 4:
      return "FILE";

    default:
      return "OTHER";
  }
}


/**
 * @brief Return a human-readable ELF symbol binding name.
 * @param[in] symbol ELF symbol whose binding is examined.
 * @return Constant textual binding name.
 */
static const char *
symbol_bind_name(
  const AvrElfSymbol *symbol)
{
  if (symbol == NULL)
    return "UNKNOWN";

  switch (symbol->bind) {
    case 0:
      return "LOCAL";

    case 1:
      return "GLOBAL";

    case 2:
      return "WEAK";

    default:
      return "OTHER";
  }
}


/*
 * ----------------------------------------------------------------------
 * Symbol listing
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print useful ELF symbols and their device mappings.
 * @param[in] elf Parsed ELF object.
 * @param[in] device Selected AVR device.
 *
 * @details Symbols are filtered through @ref avr_elf_symbol_is_useful and
 * resolved through @ref avr_elf_resolve_symbol_at.
 */
static void
print_symbols(
  const AvrElf *elf,
  const AvrDevice *device)
{
  size_t count;

  count =
    avr_elf_symbol_count(elf);

  if (count == 0) {
    printf(
      "\nNo symbol table available.\n"
    );

    return;
  }

  printf(
    "\nSymbols\n"
    "=======\n\n"
  );

  printf(
    "%-30s %-12s %-7s %-20s %-12s %-16s\n",
    "Name",
    "ELF VMA",
    "Size",
    "Section",
    "Memory",
    "AVR"
  );

  printf(
    "-----------------------------------------------------------------------------------------------------\n"
  );

  for (size_t i = 0;
       i < count;
       ++i)
  {
    const AvrElfSymbol *symbol;
    AvrResolvedSymbol resolved;

    symbol =
      avr_elf_symbol_at(
        elf,
        i
      );

    if (symbol == NULL)
      continue;

    if (!avr_elf_symbol_is_useful(symbol))
      continue;

    if (symbol->name == NULL ||
        symbol->name[0] == '\0')
      continue;

    if (!avr_elf_resolve_symbol_at(
          elf,
          device,
          i,
          &resolved))
    {
      continue;
    }

    printf(
      "%-30s "
      "0x%08" PRIx32 " "
      "%-7" PRIu32 " ",
      symbol->name,
      symbol->value,
      symbol->size
    );

    if (resolved.section != NULL)
      printf(
        "%-20s ",
        resolved.section->name
      );
    else
      printf(
        "%-20s ",
        "-"
      );

    printf(
      "%-12s ",
      avr_memory_space_name(
        resolved.memory_space
      )
    );

    if (resolved.has_physical_address) {
      printf(
        "0x%08" PRIx32,
        resolved.physical_address
      );
    }
    else {
      printf(
        "-"
      );
    }

    if (resolved.sfr != NULL) {
      printf(
        "  %s",
        resolved.sfr->name
      );
    }

    if (resolved.has_lma) {
      printf(
        "  LMA=0x%08" PRIx32,
        resolved.lma
      );
    }

    printf(
      "\n"
    );
  }
}


/*
 * ----------------------------------------------------------------------
 * Individual symbol lookup
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print SFR information associated with a resolved symbol.
 * @param[in] resolved Resolved symbol containing an optional SFR reference.
 */
static void
print_sfr_information(
  const AvrResolvedSymbol *resolved)
{
  const AvrSfr *sfr;

  sfr =
    resolved->sfr;

  if (sfr == NULL)
    return;

  printf(
    "\nSpecial Function Register:\n"
    "\n"
  );

  printf(
    "  Name:               %s\n",
    sfr->name
  );

  printf(
    "  DATA address:       0x%04" PRIx16 "\n",
    sfr->address
  );

  printf(
    "  Encoding:           %s\n",
    avr_sfr_space_name(sfr->space)
  );

  printf(
    "  Encoded address:    0x%04" PRIx16 "\n",
    sfr->encoded_address
  );

  printf(
    "  Width:              %" PRIu8 " byte%s\n",
    sfr->width,
    sfr->width == 1 ? "" : "s"
  );
}


/**
 * @brief Print the LMA and load range of a resolved symbol.
 * @param[in] resolved Resolved symbol containing LMA information.
 */
static void
print_symbol_load_mapping(
  const AvrResolvedSymbol *resolved)
{
  uint32_t end;

  if (!resolved->has_lma)
    return;

  printf(
    "\nLoad mapping:\n"
    "\n"
  );

  printf(
    "  LMA:                0x%08" PRIx32 "\n",
    resolved->lma
  );

  if (resolved->size != 0 &&
      region_end(
        resolved->lma,
        resolved->size,
        &end
      ))
  {
    printf(
      "  LMA range:          0x%08" PRIx32
      " - 0x%08" PRIx32 "\n",
      resolved->lma,
      end
    );
  }
}


/**
 * @brief Print detailed information for one named ELF symbol.
 * @param[in] elf Parsed ELF object.
 * @param[in] device Selected AVR device.
 * @param[in] name Symbol name to resolve.
 *
 * @details The output includes ELF section information, memory-space
 * classification, physical mapping, SFR details, FLASH mapping, LMA, and
 * initialization semantics where applicable.
 */
static void
print_symbol_details(
  const AvrElf *elf,
  const AvrDevice *device,
  const char *name)
{
  AvrResolvedSymbol resolved;
  const AvrElfSection *section;

  if (!avr_elf_resolve_symbol(
        elf,
        device,
        name,
        &resolved
      ))
  {
    fprintf(
      stderr,
      "error: symbol '%s' not found or could not be resolved\n",
      name
    );

    return;
  }

  printf(
    "\nSymbol: %s\n"
    "==================================================\n",
    name
  );

  printf(
    "ELF VMA:             0x%08" PRIx32 "\n",
    resolved.value
  );

  printf(
    "Size:                %" PRIu32 " bytes\n",
    resolved.size
  );

  printf(
    "Binding:             %s\n",
    symbol_bind_name(
      &resolved.symbol
    )
  );

  printf(
    "Type:                %s\n",
    symbol_type_name(
      &resolved.symbol
    )
  );

  printf(
    "Section index:       %" PRIu16 "\n",
    resolved.section_index
  );

  section =
    resolved.section;

  if (section == NULL) {
    printf(
      "Section:             <none>\n"
    );

    printf(
      "Memory:              %s\n",
      avr_memory_space_name(
        resolved.memory_space
      )
    );

    return;
  }

  printf(
    "Section:             %s\n",
    section->name
  );

  printf(
    "Section VMA:         0x%08" PRIx32 "\n",
    section->addr
  );

  printf(
    "Section size:        0x%08" PRIx32 "\n",
    section->size
  );

  if (resolved.value >=
      section->addr)
  {
    uint32_t section_offset =
      resolved.value -
      section->addr;

    printf(
      "Section offset:      0x%08" PRIx32 "\n",
      section_offset
    );
  }

  printf(
    "Memory:              %s\n",
    avr_memory_space_name(
      resolved.memory_space
    )
  );

  printf(
    "AVR address:         0x%08" PRIx32 "\n",
    resolved.avr_address
  );

  if (resolved.has_physical_address) {
    printf(
      "Physical address:    0x%08" PRIx32 "\n",
      resolved.physical_address
    );
  }
  else {
    printf(
      "Physical address:    <unavailable>\n"
    );
  }


  /*
   * SRAM mapping.
   */
  if (resolved.memory_space ==
      AVR_MEM_SRAM)
  {
    uint32_t end;

    printf(
      "\nRuntime SRAM mapping:\n"
      "\n"
    );

    printf(
      "  SRAM address:       0x%08" PRIx32 "\n",
      resolved.physical_address
    );

    if (resolved.size != 0 &&
        region_end(
          resolved.physical_address,
          resolved.size,
          &end
        ))
    {
      printf(
        "  SRAM range:         0x%08" PRIx32
        " - 0x%08" PRIx32 "\n",
        resolved.physical_address,
        end
      );
    }

    /*
     * .data has an independent flash initialization image.
     */
    if (strcmp(
          section->name,
          ".data"
        ) == 0)
    {
      printf(
        "  Initialization:     FLASH\n"
      );
    }
  }


  /*
   * Register/IO mapping.
   */
  if (resolved.memory_space ==
        AVR_MEM_REGISTER ||
      resolved.memory_space ==
        AVR_MEM_IO)
  {
    printf(
      "\nAVR DATA-space mapping:\n"
      "\n"
    );

    printf(
      "  DATA address:       0x%04" PRIx32 "\n",
      resolved.avr_address
    );

    if (resolved.sfr == NULL) {
      printf(
        "  SFR:                <not identified>\n"
      );
    }
    else {
      printf(
        "  SFR:                %s\n",
        resolved.sfr->name
      );
    }
  }


  /*
   * Flash mapping.
   */
  if (resolved.memory_space ==
      AVR_MEM_FLASH)
  {
    printf(
      "\nProgram-memory mapping:\n"
      "\n"
    );

    printf(
      "  Flash byte address: 0x%08" PRIx32 "\n",
      resolved.physical_address
    );

    if (resolved.has_flash_word_address) {
      printf(
        "  AVR word address:   0x%08" PRIx32 "\n",
        resolved.flash_word_address
      );
    }
  }


  /*
   * SFR details.
   */
  print_sfr_information(
    &resolved
  );


  /*
   * LMA.
   */
  if (resolved.has_lma) {
    print_symbol_load_mapping(
      &resolved
    );

    if (strcmp(
          section->name,
          ".data"
        ) == 0)
    {
      uint32_t section_offset;

      section_offset =
        resolved.value -
        section->addr;

      printf(
        "\n.data transformation:\n"
        "\n"
      );

      printf(
        "  Section VMA:        0x%08" PRIx32 "\n",
        section->addr
      );

      printf(
        "  Section LMA:        0x%08" PRIx32 "\n",
        resolved.lma -
        section_offset
      );

      printf(
        "  Symbol VMA:         0x%08" PRIx32 "\n",
        resolved.value
      );

      printf(
        "  Symbol offset:      0x%08" PRIx32 "\n",
        section_offset
      );

      printf(
        "  Symbol LMA:         0x%08" PRIx32 "\n",
        resolved.lma
      );

      printf(
        "\n"
        "  Runtime SRAM = ELF VMA translated through the device DATA space.\n"
        "  Flash initializer = section LMA + symbol section offset.\n"
      );
    }
  }
  else if (strcmp(
             section->name,
             ".bss"
           ) == 0)
  {
    printf(
      "\nInitialization:      ZERO (.bss)\n"
    );
  }
  else if (strcmp(
             section->name,
             ".noinit"
           ) == 0)
  {
    printf(
      "\nInitialization:      PRESERVED (.noinit)\n"
    );
  }
  else {
    printf(
      "\nLoad address:        unavailable\n"
    );
  }
}


/*
 * ----------------------------------------------------------------------
 * Map helpers
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print one allocated section as part of the complete memory map.
 * @param[in] elf Parsed ELF object.
 * @param[in] device Selected AVR device.
 * @param[in] section_index ELF section-table index.
 */
static void
print_map_section(
  const AvrElf *elf,
  const AvrDevice *device,
  uint16_t section_index)
{
  const AvrElfSection *section;
  AvrResolvedSection resolved;
  uint32_t physical_end;

  section =
    avr_elf_section_at(
      elf,
      section_index
    );

  if (section == NULL)
    return;

  if (!section->alloc ||
      section->size == 0)
  {
    return;
  }

  if (!avr_elf_resolve_section(
        elf,
        device,
        section_index,
        &resolved
      ))
  {
    return;
  }

  printf(
    "  %-6s %-24s",
    avr_memory_space_name(
      resolved.memory_space
    ),
    section->name
  );

  if (resolved.has_physical_address &&
      region_end(
        resolved.physical_address,
        section->size,
        &physical_end
      ))
  {
    printf(
      "  0x%08" PRIx32
      " - 0x%08" PRIx32,
      resolved.physical_address,
      physical_end
    );
  }
  else {
    printf(
      "  %-23s",
      "-"
    );
  }

  printf(
    "  size=0x%08" PRIx32,
    section->size
  );

  if (resolved.has_lma) {
    printf(
      "  LMA=0x%08" PRIx32,
      resolved.lma
    );
  }

  printf(
    "\n"
  );
}


/*
 * ----------------------------------------------------------------------
 * Complete memory map
 * ----------------------------------------------------------------------
 */

/**
 * @brief Print the complete device-aware memory map.
 * @param[in] elf Parsed ELF object.
 * @param[in] device Selected AVR device.
 *
 * @details Displays FLASH, DATA/SRAM, EEPROM, section placement, and the
 * special initialization relationships of .data, .bss, and .noinit.
 */
static void
print_map(
  const AvrElf *elf,
  const AvrDevice *device)
{
  size_t section_count;
  int data_index = -1;
  int bss_index = -1;
  int noinit_index = -1;

  section_count =
    avr_elf_section_count(elf);

  printf(
    "\nMemory map\n"
    "==========\n"
  );

  printf(
    "\nFLASH\n"
    "-----\n"
  );

  print_physical_region(
    "FLASH",
    avr_device_flash_region(device)
  );

  printf(
    "\nFlash-resident sections:\n"
  );

  for (size_t i = 0;
       i < section_count;
       ++i)
  {
    const AvrElfSection *section =
      avr_elf_section_at(
        elf,
        (uint16_t)i
      );

    AvrResolvedSection resolved;

    if (section == NULL ||
        !section->alloc ||
        section->size == 0)
    {
      continue;
    }

    if (!avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)i,
          &resolved
        ))
    {
      continue;
    }

    if (resolved.memory_space ==
        AVR_MEM_FLASH)
    {
      print_map_section(
        elf,
        device,
        (uint16_t)i
      );
    }
  }


  printf(
    "\nDATA / SRAM\n"
    "-----------\n"
  );

  printf(
    "Physical DATA layout:\n"
  );

  print_physical_region(
    "Registers",
    device->register_file
  );

  print_physical_region(
    "I/O",
    device->io
  );

  print_physical_region(
    "Ext I/O",
    device->ext_io
  );

  print_physical_region(
    "SRAM",
    device->sram
  );

  printf(
    "\nAllocated DATA-space sections:\n"
  );

  for (size_t i = 0;
       i < section_count;
       ++i)
  {
    const AvrElfSection *section =
      avr_elf_section_at(
        elf,
        (uint16_t)i
      );

    AvrResolvedSection resolved;

    if (section == NULL ||
        !section->alloc ||
        section->size == 0)
    {
      continue;
    }

    if (strcmp(
          section->name,
          ".data"
        ) == 0)
    {
      data_index = (int)i;
    }

    if (strcmp(
          section->name,
          ".bss"
        ) == 0)
    {
      bss_index = (int)i;
    }

    if (strcmp(
          section->name,
          ".noinit"
        ) == 0)
    {
      noinit_index = (int)i;
    }

    if (!avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)i,
          &resolved
        ))
    {
      continue;
    }

    if (resolved.memory_space ==
        AVR_MEM_REGISTER ||
        resolved.memory_space ==
        AVR_MEM_IO ||
        resolved.memory_space ==
        AVR_MEM_SRAM)
    {
      print_map_section(
        elf,
        device,
        (uint16_t)i
      );
    }
  }


  /*
   * .data initialization relationship.
   */
  if (data_index >= 0) {
    const AvrElfSection *data;
    AvrResolvedSection resolved;
    uint32_t sram_end;
    uint32_t flash_end;

    data =
      avr_elf_section_at(
        elf,
        (uint16_t)data_index
      );

    if (data != NULL &&
        avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)data_index,
          &resolved
        ) &&
        resolved.has_physical_address &&
        resolved.has_lma)
    {
      printf(
        "\n.data initialization\n"
        "--------------------\n"
      );

      if (region_end(
            resolved.physical_address,
            data->size,
            &sram_end) &&
          region_end(
            resolved.lma,
            data->size,
            &flash_end
          ))
      {
        printf(
          "  Flash image:\n"
          "    0x%08" PRIx32
          " - 0x%08" PRIx32 "\n",
          resolved.lma,
          flash_end
        );

        printf(
          "  SRAM runtime:\n"
          "    0x%08" PRIx32
          " - 0x%08" PRIx32 "\n",
          resolved.physical_address,
          sram_end
        );

        printf(
          "\n"
          "  Startup operation:\n"
          "    FLASH 0x%08" PRIx32
          " -> SRAM 0x%08" PRIx32
          " (%" PRIu32 " bytes)\n",
          resolved.lma,
          resolved.physical_address,
          data->size
        );
      }
    }
  }


  /*
   * .bss initialization.
   */
  if (bss_index >= 0) {
    const AvrElfSection *bss;
    AvrResolvedSection resolved;
    uint32_t end;

    bss =
      avr_elf_section_at(
        elf,
        (uint16_t)bss_index
      );

    if (bss != NULL &&
        avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)bss_index,
          &resolved
        ) &&
        resolved.has_physical_address &&
        region_end(
          resolved.physical_address,
          bss->size,
          &end
        ))
    {
      printf(
        "\n.bss initialization\n"
        "-------------------\n"
      );

      printf(
        "  SRAM range:\n"
        "    0x%08" PRIx32
        " - 0x%08" PRIx32 "\n",
        resolved.physical_address,
        end
      );

      printf(
        "\n"
        "  Startup operation:\n"
        "    ZERO -> SRAM 0x%08" PRIx32
        " (%" PRIu32 " bytes)\n",
        resolved.physical_address,
        bss->size
      );
    }
  }


  /*
   * .noinit is allocated SRAM whose contents are preserved.
   */
  if (noinit_index >= 0) {
    const AvrElfSection *noinit;
    AvrResolvedSection resolved;
    uint32_t end;

    noinit =
      avr_elf_section_at(
        elf,
        (uint16_t)noinit_index
      );

    if (noinit != NULL &&
        noinit->size != 0 &&
        avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)noinit_index,
          &resolved
        ) &&
        resolved.has_physical_address &&
        region_end(
          resolved.physical_address,
          noinit->size,
          &end
        ))
    {
      printf(
        "\n.noinit\n"
        "-------\n"
      );

      printf(
        "  SRAM range:\n"
        "    0x%08" PRIx32
        " - 0x%08" PRIx32 "\n",
        resolved.physical_address,
        end
      );

      printf(
        "  Initialization: preserved\n"
      );
    }
  }


  /*
   * EEPROM sections.
   */
  printf(
    "\nEEPROM\n"
    "------\n"
  );

  print_physical_region(
    "EEPROM",
    avr_device_eeprom_region(device)
  );

  for (size_t i = 0;
       i < section_count;
       ++i)
  {
    const AvrElfSection *section =
      avr_elf_section_at(
        elf,
        (uint16_t)i
      );

    AvrResolvedSection resolved;

    if (section == NULL ||
        !section->alloc ||
        section->size == 0)
    {
      continue;
    }

    if (!avr_elf_resolve_section(
          elf,
          device,
          (uint16_t)i,
          &resolved
        ))
    {
      continue;
    }

    if (resolved.memory_space ==
        AVR_MEM_EEPROM)
    {
      print_map_section(
        elf,
        device,
        (uint16_t)i
      );
    }
  }
}


/*
 * ----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------
 */

/**
 * @brief Application entry point for avrmem.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return @c EXIT_SUCCESS on successful processing; @c EXIT_FAILURE on
 *   invalid arguments, ELF/device initialization failure, or output error.
 *
 * @details The frontend parses command-line options, opens the ELF, selects
 * the AVR device explicitly or from ELF metadata, initializes the device,
 * and dispatches the requested presentation operations. Resource cleanup is
 * centralized before returning.
 */
int
main(
  int argc,
  char **argv)
{
  CommandLine command;
  AvrElf *elf = NULL;
  AvrDevice device;

  const char *elf_device_name;
  const char *selected_device_name;

  bool device_initialized = false;
  bool any_display_option;
  int exit_status = EXIT_FAILURE;

  parse_options_fn parse_options = parse_options_posix;


  memset(
    &command,
    0,
    sizeof(command)
  );

  memset(
    &device,
    0,
    sizeof(device)
  );


  /*
   * --------------------------------------------------------------
   * Command-line parsing
   * --------------------------------------------------------------
   */

  if (!parse_options(
        argc,
        argv,
        &command
      ))
  {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }


  /*
   * --------------------------------------------------------------
   * Open ELF
   * --------------------------------------------------------------
   *
   * The ELF must be opened before automatic device selection because
   * the device identity is stored in ELF metadata.
   */
  if (!avr_elf_open(
        command.elf_filename,
        &elf
      ))
  {
    fprintf(
      stderr,
      "error: unable to open or parse AVR ELF file '%s'\n",
      command.elf_filename
    );

    goto cleanup;
  }


  /*
   * avr_elf_open() already validates EM_AVR. Keep the explicit check
   * here because it makes the frontend's assumption visible.
   */
  if (avr_elf_get_machine(elf) !=
      AVR_ELF_MACHINE_AVR)
  {
    fprintf(
      stderr,
      "error: ELF file is not an AVR executable\n"
    );

    goto cleanup;
  }


  /*
   * --------------------------------------------------------------
   * Device selection
   * --------------------------------------------------------------
   *
   * Explicit --device takes precedence.
   *
   * When omitted, derive the device from the ELF metadata.
   */
  elf_device_name =
    avr_elf_get_device_name(elf);

  if (command.device_name != NULL) {
    selected_device_name =
      command.device_name;
  }
  else {
    if (elf_device_name == NULL ||
        *elf_device_name == '\0')
    {
      fprintf(
        stderr,
        "error: ELF does not contain a usable AVR device name; "
        "use --device <name>\n"
      );

      goto cleanup;
    }

    selected_device_name =
      elf_device_name;
  }


  /*
   * --------------------------------------------------------------
   * Device initialization
   * --------------------------------------------------------------
   */
  if (!avr_device_init(
        &device,
        selected_device_name
      ))
  {
    fprintf(
      stderr,
      "error: unable to initialize AVR device '%s'\n",
      selected_device_name
    );

    goto cleanup;
  }

  device_initialized = true;


  /*
   * --------------------------------------------------------------
   * Basic information
   * --------------------------------------------------------------
   *
   * This is always printed.
   */
  print_basic_information(
    command.elf_filename,
    &device,
    elf
  );


  /*
   * --------------------------------------------------------------
   * Display selection
   * --------------------------------------------------------------
   *
   * With no explicit display option, preserve the original default:
   *
   *     memory model + sections
   */
  any_display_option =
    command.show_memory ||
    command.show_sections ||
    command.show_symbols ||
    command.show_symbol ||
    command.show_map;

  if (!any_display_option) {
    command.show_memory = true;
    command.show_sections = true;
  }


  /*
   * Multiple display options may be supplied together.
   */
  if (command.show_memory) {
    print_memory_model(
      &device
    );
  }

  if (command.show_sections) {
    print_sections(
      elf,
      &device
    );
  }

  if (command.show_symbols) {
    print_symbols(
      elf,
      &device
    );
  }

  if (command.show_symbol) {
    print_symbol_details(
      elf,
      &device,
      command.symbol_name
    );

    /*
     * Detailed symbol lookup is an explicit user request.  A missing
     * symbol is reported by print_symbol_details(), but preserve a
     * failure exit status by checking the resolver once more.
     */
    {
      AvrResolvedSymbol resolved;

      if (!avr_elf_resolve_symbol(
            elf,
            &device,
            command.symbol_name,
            &resolved
          ))
      {
        goto cleanup;
      }
    }
  }

  if (command.show_map) {
    print_map(
      elf,
      &device
    );
  }

  exit_status =
    EXIT_SUCCESS;


cleanup:

  /*
   * Reverse-order destruction.
   *
   * Resolved structures contain pointers into AvrElf and AvrDevice;
   * nothing from them is used after these objects are released.
   */
  avr_elf_close(elf);

  if (device_initialized)
    avr_device_destroy(&device);

  return exit_status;
}
