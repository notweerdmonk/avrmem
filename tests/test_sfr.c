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
 * @file test_sfr.c
 * @brief greatest.h unit suite for the SFR module (`src/avr_sfr.c`).
 *
 * @details
 * Toolchain-free: links `src/avr_sfr.c` only. Build with
 * `-Iinclude -Itests/vendor`. Each test owns its database; no state
 * is shared between tests.
 */

#include "greatest.h"
#include "avr_sfr.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>


GREATEST_MAIN_DEFS();


/**
 * @brief Init a database and parse input, or return NULL on any failure.
 *
 * @details
 * Reduces boilerplate for the success-path tests. The caller owns the
 * returned database and must pass it to @ref avr_sfr_destroy.
 */
static AvrSfrDatabase *
parse_or_null(
  const char *input)
{
  AvrSfrDatabase *db = NULL;

  if (!avr_sfr_init(&db))
    return NULL;

  if (!avr_sfr_parse(db, input)) {
    avr_sfr_destroy(db);
    return NULL;
  }

  return db;
}


/**
 * @brief Init, parse once, destroy; report whether parsing failed.
 *
 * @details
 * Gives each malformed-input case a fresh database so failures cannot
 * leak state into other cases.
 */
static bool
parse_fails(
  const char *input)
{
  AvrSfrDatabase *db = NULL;
  bool failed;

  if (!avr_sfr_init(&db))
    return false;

  failed =
    !avr_sfr_parse(db, input);

  avr_sfr_destroy(db);

  return failed;
}


TEST
init_and_destroy_ok(void)
{
  AvrSfrDatabase *db = NULL;

  ASSERT(avr_sfr_init(&db));
  ASSERT(db != NULL);
  ASSERT_EQ((size_t)0, avr_sfr_count(db));
  ASSERT_EQ(0, avr_sfr_offset(db));

  avr_sfr_destroy(db);
  avr_sfr_destroy(NULL);

  PASS();
}


TEST
parse_canonical_block(void)
{
  static const char input[] =
    "#define __STDC__ 1\n"
    "#define __AVR_ATmega328P__ 1\n"
    "#define F_CPU 16000000UL\n"
    "#define __SFR_OFFSET 0x20\n"
    "#define SREG _SFR_IO8(0x3F)\n"
    "#define PORTB _SFR_IO8(0x05)\n"
    "#define UCSR0A _SFR_MEM8(0xC0)\n"
    "#define _SFR_MEM8(mem_addr) ((mem_addr))\n";

  AvrSfrDatabase *db;
  const AvrSfr *portb;
  const AvrSfr *sreg;
  const AvrSfr *ucsr0a;

  db =
    parse_or_null(input);

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)3, avr_sfr_count(db));
  ASSERT_EQ(0x20, avr_sfr_offset(db));

  portb =
    avr_sfr_find(db, "PORTB");

  ASSERT(portb != NULL);
  ASSERT_EQ(0x25, portb->address);
  ASSERT_EQ(AVR_SFR_SPACE_IO, portb->space);
  ASSERT_EQ(0x05, portb->encoded_address);
  ASSERT_EQ(1, portb->width);

  sreg =
    avr_sfr_find(db, "SREG");

  ASSERT(sreg != NULL);
  ASSERT_EQ(0x5F, sreg->address);
  ASSERT_EQ(AVR_SFR_SPACE_IO, sreg->space);
  ASSERT_EQ(0x3F, sreg->encoded_address);
  ASSERT_EQ(1, sreg->width);

  ucsr0a =
    avr_sfr_find(db, "UCSR0A");

  ASSERT(ucsr0a != NULL);
  ASSERT_EQ(0xC0, ucsr0a->address);
  ASSERT_EQ(AVR_SFR_SPACE_DATA, ucsr0a->space);
  ASSERT_EQ(0xC0, ucsr0a->encoded_address);
  ASSERT_EQ(1, ucsr0a->width);

  ASSERT(avr_sfr_find_address(db, 0x25) == portb);

  avr_sfr_destroy(db);

  PASS();
}


TEST
ignores_non_sfr_lines(void)
{
  static const char input[] =
    "#define __SFR_OFFSET 0x20\n"
    "#define F_CPU 16000000UL\n"
    "#define _SFR_IO8(io_addr) ((io_addr) + __SFR_OFFSET)\n"
    "# 1 \"io.h\"\n"
    "\n"
    "/* a comment line */\n"
    "#define PORTB _SFR_IO8(0x05)\n"
    "#define FOO _SFR_IO16(0x06)\n"
    "#define BAR _SFR_MEM16(0xC0)\n"
    "#define BAZ _SFR_IO9(0x07)\n";

  AvrSfrDatabase *db;

  db =
    parse_or_null(input);

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)1, avr_sfr_count(db));
  ASSERT(avr_sfr_find(db, "PORTB") != NULL);
  ASSERT(avr_sfr_find(db, "F_CPU") == NULL);
  ASSERT(avr_sfr_find(db, "FOO") == NULL);
  ASSERT(avr_sfr_find(db, "BAR") == NULL);
  ASSERT(avr_sfr_find(db, "BAZ") == NULL);

  avr_sfr_destroy(db);

  PASS();
}


TEST
null_and_empty(void)
{
  AvrSfrDatabase *db = NULL;

  ASSERT(!avr_sfr_init(NULL));
  ASSERT(!avr_sfr_parse(NULL, "#define PORTB _SFR_IO8(0x05)\n"));

  ASSERT(avr_sfr_init(&db));
  ASSERT(!avr_sfr_parse(db, NULL));
  ASSERT(!avr_sfr_parse(db, ""));
  ASSERT(!avr_sfr_parse(db, "int x;\n"));

  ASSERT_EQ((size_t)0, avr_sfr_count(NULL));
  ASSERT(avr_sfr_at(NULL, 0) == NULL);
  ASSERT(avr_sfr_find(NULL, "PORTB") == NULL);
  ASSERT_EQ(0, avr_sfr_offset(NULL));

  ASSERT(avr_sfr_at(db, 0) == NULL);
  ASSERT(avr_sfr_find(db, NULL) == NULL);
  ASSERT(avr_sfr_find(db, "PORTB") == NULL);
  ASSERT(avr_sfr_find_address(db, 0x25) == NULL);

  avr_sfr_destroy(db);

  db =
    parse_or_null(
      "#define __SFR_OFFSET 0x20\n"
      "#define PORTB _SFR_IO8(0x05)\n"
    );

  ASSERT(db != NULL);
  ASSERT(avr_sfr_find(db, "portb") == NULL);
  ASSERT(avr_sfr_find(db, "PIRATE") == NULL);
  ASSERT(avr_sfr_find_address(db, 0x1234) == NULL);

  avr_sfr_destroy(db);

  PASS();
}


TEST
missing_offset_mem_only(void)
{
  AvrSfrDatabase *db;
  const AvrSfr *sfr;

  db =
    parse_or_null("#define UCSR0A _SFR_MEM8(0xC0)\n");

  ASSERT(db != NULL);
  ASSERT_EQ(0, avr_sfr_offset(db));
  ASSERT_EQ((size_t)1, avr_sfr_count(db));

  sfr =
    avr_sfr_find(db, "UCSR0A");

  ASSERT(sfr != NULL);
  ASSERT_EQ(0xC0, sfr->address);
  ASSERT_EQ(AVR_SFR_SPACE_DATA, sfr->space);
  ASSERT_EQ(0xC0, sfr->encoded_address);

  avr_sfr_destroy(db);

  PASS();
}


TEST
missing_offset_io(void)
{
  AvrSfrDatabase *db;
  const AvrSfr *sfr;

  db =
    parse_or_null("#define PORTB _SFR_IO8(0x05)\n");

  ASSERT(db != NULL);
  ASSERT_EQ(0, avr_sfr_offset(db));
  ASSERT_EQ((size_t)1, avr_sfr_count(db));

  sfr =
    avr_sfr_find(db, "PORTB");

  ASSERT(sfr != NULL);
  ASSERT_EQ(0x05, sfr->address);
  ASSERT_EQ(AVR_SFR_SPACE_IO, sfr->space);
  ASSERT_EQ(0x05, sfr->encoded_address);

  avr_sfr_destroy(db);

  PASS();
}


TEST
malformed_offset(void)
{
  ASSERT(parse_fails("#define __SFR_OFFSET BAD\n"
                     "#define PORTB _SFR_IO8(0x05)\n"));
  ASSERT(parse_fails("#define __SFR_OFFSET 0x10000\n"
                     "#define PORTB _SFR_IO8(0x05)\n"));

  PASS();
}


TEST
duplicate_first_wins(void)
{
  AvrSfrDatabase *db;
  const AvrSfr *sfr;

  db =
    parse_or_null(
      "#define __SFR_OFFSET 0x20\n"
      "#define PORTB _SFR_IO8(0x05)\n"
      "#define PORTB _SFR_MEM8(0xC0)\n"
    );

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)1, avr_sfr_count(db));

  sfr =
    avr_sfr_find(db, "PORTB");

  ASSERT(sfr != NULL);
  ASSERT_EQ(0x25, sfr->address);
  ASSERT_EQ(AVR_SFR_SPACE_IO, sfr->space);
  ASSERT_EQ(0x05, sfr->encoded_address);

  avr_sfr_destroy(db);

  PASS();
}


TEST
malformed_lines(void)
{
  char long_arg[128];
  char long_line[1024];
  char long_first_pass[512];

  ASSERT(parse_fails("#define PORTB _SFR_IO8(0x05\n"));
  ASSERT(parse_fails("#define PORTB _SFR_IO8()\n"));
  ASSERT(parse_fails("#define PORTB _SFR_IO8(NOTANUMBER)\n"));
  ASSERT(parse_fails("#define PORTB _SFR_IO8(0x10000)\n"));
  ASSERT(parse_fails("#define PORTB _SFR_IO8(0x05;)\n"));
  ASSERT(parse_fails("#define __SFR_OFFSET 0x20\n"
                     "#define PORTB _SFR_IO8(0xFFF0)\n"));

  /* 64-byte argument text: the parser requires arg text < 64 bytes. */
  snprintf(long_arg, sizeof(long_arg),
           "#define PORTB _SFR_IO8(%064d)\n", 0);
  ASSERT(parse_fails(long_arg));

  /* >= 512-byte line after the offset: rejected by the second pass. */
  snprintf(long_line, sizeof(long_line),
           "#define __SFR_OFFSET 0x20\n"
           "#define PORTB _SFR_IO8(0x05)\n"
           "# %0600d\n",
           0);
  ASSERT(parse_fails(long_line));

  /* >= 256-byte line before the offset: rejected by the first pass. */
  snprintf(long_first_pass, sizeof(long_first_pass),
           "/* %0300d */\n"
           "#define __SFR_OFFSET 0x20\n"
           "#define PORTB _SFR_IO8(0x05)\n",
           0);
  ASSERT(parse_fails(long_first_pass));

  PASS();
}


TEST
find_address_uses_normalized(void)
{
  AvrSfrDatabase *db;
  const AvrSfr *by_name;

  db =
    parse_or_null(
      "#define __SFR_OFFSET 0x20\n"
      "#define PORTB _SFR_IO8(0x05)\n"
    );

  ASSERT(db != NULL);

  by_name =
    avr_sfr_find(db, "PORTB");

  ASSERT(by_name != NULL);
  ASSERT(avr_sfr_find_address(db, 0x05) == NULL);
  ASSERT(avr_sfr_find_address(db, 0x25) == by_name);

  avr_sfr_destroy(db);

  PASS();
}


TEST
space_names(void)
{
  AvrSfrDatabase *db = NULL;

  ASSERT(avr_sfr_init(&db));
  ASSERT(strcmp(avr_sfr_space_name(AVR_SFR_SPACE_IO), "IO") == 0);
  ASSERT(strcmp(avr_sfr_space_name(AVR_SFR_SPACE_DATA), "DATA") == 0);
  ASSERT(strcmp(avr_sfr_space_name((AvrSfrSpace)99), "UNKNOWN") == 0);
  avr_sfr_destroy(db);

  PASS();
}


TEST
decimal_and_suffix_args(void)
{
  AvrSfrDatabase *db;
  const AvrSfr *dec;
  const AvrSfr *hex;

  db =
    parse_or_null(
      "#define __SFR_OFFSET 0x20\n"
      "#define DEC192 _SFR_MEM8(192)\n"
      "#define HEXC0 _SFR_MEM8(0xC0UL)\n"
    );

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)2, avr_sfr_count(db));

  dec =
    avr_sfr_find(db, "DEC192");

  ASSERT(dec != NULL);
  ASSERT_EQ(192, dec->address);
  ASSERT_EQ(192, dec->encoded_address);

  hex =
    avr_sfr_find(db, "HEXC0");

  ASSERT(hex != NULL);
  ASSERT_EQ(0xC0, hex->address);
  ASSERT_EQ(0xC0, hex->encoded_address);

  avr_sfr_destroy(db);

  PASS();
}


TEST
reparse_stable(void)
{
  static const char input[] =
    "#define __SFR_OFFSET 0x20\n"
    "#define SREG _SFR_IO8(0x3F)\n"
    "#define PORTB _SFR_IO8(0x05)\n"
    "#define UCSR0A _SFR_MEM8(0xC0)\n";

  AvrSfrDatabase *db;
  const AvrSfr *portb;

  db =
    parse_or_null(input);

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)3, avr_sfr_count(db));

  /*
   * Observed 2026-09-27: re-parsing identical input on the same
   * database succeeds with a stable count. Duplicates are ignored
   * (first wins), so no entries are added. Pointers are re-fetched
   * after the second parse: growth may realloc the register array,
   * so only values are locked here, never pointer identity.
   */
  ASSERT(avr_sfr_parse(db, input));
  ASSERT_EQ((size_t)3, avr_sfr_count(db));

  portb =
    avr_sfr_find(db, "PORTB");

  ASSERT(portb != NULL);
  ASSERT_EQ(0x25, portb->address);
  ASSERT_EQ(AVR_SFR_SPACE_IO, portb->space);
  ASSERT_EQ(0x05, portb->encoded_address);

  avr_sfr_destroy(db);

  PASS();
}


TEST
crlf_input(void)
{
  static const char input[] =
    "#define __SFR_OFFSET 0x20\r\n"
    "#define SREG _SFR_IO8(0x3F)\r\n"
    "#define PORTB _SFR_IO8(0x05)\r\n"
    "#define UCSR0A _SFR_MEM8(0xC0)\r\n";

  AvrSfrDatabase *db;
  const AvrSfr *portb;

  /*
   * Observed 2026-09-27: CRLF input parses to true with unchanged
   * values. Lines split on '\n', leaving a trailing '\r' per line;
   * the '\r' is harmless because parse_u16() skips trailing
   * whitespace (isspace covers '\r') on the offset, and SFR argument
   * text stops at ')' before the '\r'.
   */
  db =
    parse_or_null(input);

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)3, avr_sfr_count(db));
  ASSERT_EQ(0x20, avr_sfr_offset(db));

  portb =
    avr_sfr_find(db, "PORTB");

  ASSERT(portb != NULL);
  ASSERT_EQ(0x25, portb->address);

  avr_sfr_destroy(db);

  PASS();
}


TEST
arg_boundary(void)
{
  char input63[256];
  char input64[256];
  AvrSfrDatabase *db;
  const AvrSfr *sfr;

  /*
   * The argument text buffer is 64 bytes: arg length < 64 parses,
   * >= 64 fails. "0xC0" (4 chars) + 59 spaces = 63 chars stays valid
   * because parse_u16() allows trailing whitespace; the 64-char
   * variant is rejected by the length cap.
   */
  snprintf(input63, sizeof(input63),
           "#define __SFR_OFFSET 0x20\n"
           "#define PORTB _SFR_MEM8(0xC0%*s)\n",
           59, "");
  snprintf(input64, sizeof(input64),
           "#define __SFR_OFFSET 0x20\n"
           "#define PORTB _SFR_MEM8(0xC0%*s)\n",
           60, "");

  db =
    parse_or_null(input63);

  ASSERT(db != NULL);
  ASSERT_EQ((size_t)1, avr_sfr_count(db));

  sfr =
    avr_sfr_find(db, "PORTB");

  ASSERT(sfr != NULL);
  ASSERT_EQ(0xC0, sfr->address);

  avr_sfr_destroy(db);

  ASSERT(parse_fails(input64));

  PASS();
}


TEST
offset_bases(void)
{
  AvrSfrDatabase *dec;
  AvrSfrDatabase *oct;
  const AvrSfr *sfr;

  /*
   * parse_u16() uses strtoul() base 0: decimal "32" and octal "040"
   * both yield offset 32.
   */
  dec =
    parse_or_null(
      "#define __SFR_OFFSET 32\n"
      "#define PORTB _SFR_IO8(0x05)\n"
    );

  ASSERT(dec != NULL);
  ASSERT_EQ(32, avr_sfr_offset(dec));

  sfr =
    avr_sfr_find(dec, "PORTB");

  ASSERT(sfr != NULL);
  ASSERT_EQ(0x25, sfr->address);

  avr_sfr_destroy(dec);

  oct =
    parse_or_null(
      "#define __SFR_OFFSET 040\n"
      "#define PORTB _SFR_IO8(0x05)\n"
    );

  ASSERT(oct != NULL);
  ASSERT_EQ(32, avr_sfr_offset(oct));

  sfr =
    avr_sfr_find(oct, "PORTB");

  ASSERT(sfr != NULL);
  ASSERT_EQ(0x25, sfr->address);

  avr_sfr_destroy(oct);

  PASS();
}


SUITE(sfr)
{
  RUN_TEST(init_and_destroy_ok);
  RUN_TEST(parse_canonical_block);
  RUN_TEST(ignores_non_sfr_lines);
  RUN_TEST(null_and_empty);
  RUN_TEST(missing_offset_mem_only);
  RUN_TEST(missing_offset_io);
  RUN_TEST(malformed_offset);
  RUN_TEST(duplicate_first_wins);
  RUN_TEST(malformed_lines);
  RUN_TEST(find_address_uses_normalized);
  RUN_TEST(space_names);
  RUN_TEST(decimal_and_suffix_args);
  RUN_TEST(reparse_stable);
  RUN_TEST(crlf_input);
  RUN_TEST(arg_boundary);
  RUN_TEST(offset_bases);
}


int
main(int argc, char *argv[])
{
  GREATEST_MAIN_BEGIN();
  RUN_SUITE(sfr);
  GREATEST_MAIN_END();
}
