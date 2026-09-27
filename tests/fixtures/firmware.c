/*
 * firmware.c — Q4/Q3 test fixture firmware (multi-device matrix).
 *
 * Build (per device): avr-gcc -mmcu=<dev> -Os -g -o firmware.elf firmware.c
 * (see gen.sh; never commit built ELFs, only this source). Matrix devices:
 * atmega328p, atmega2560, attiny85 (atxmega128a1 builds but is deferred —
 * see XMEGA note below).
 *
 * Expected sections / symbols (verified with avr-readelf -S -s -n):
 * - main               -> .text  (FLASH; function, touches PORTB so the
 *                         SFR-touching code path is exercised)
 * - initialized_value  -> .data  (SRAM VMA, FLASH LMA; initialized 0xA5)
 * - counter_bss        -> .bss   (SRAM, NOBITS, no LMA; zeroed at startup)
 * - dup_cnt (global)   -> .bss   (SRAM, GLOBAL binding)
 * - dup_cnt (main-local static, emitted as dup_cnt.NNNN)
 *                      -> .bss   (SRAM, LOCAL binding; same address space
 *                         as the global but a distinct symbol — the pair
 *                         exercises duplicate-name resolution; observed:
 *                         `--symbol dup_cnt` resolves to the GLOBAL one,
 *                         locked by Q4 test B23)
 * - preserved_value    -> .noinit (SRAM, NOBITS, no LMA; NOT zeroed —
 *                         must never be given an invented LMA)
 * - eeprom_value       -> .eeprom (EEPROM VMA 0x00810000, init 0x5A)
 * - .note.gnu.avr.deviceinfo -> toolchain-emitted note naming the -mmcu
 *                         device (used by the auto-device path; see
 *                         no_metadata.elf for the 328p note-less variant)
 *
 * Layout stability (328p): the Q3 additions keep the 328p-gated
 * cli_tests.sh exact greps green — .text stays 0xce bytes, .data stays
 * VMA 0x00800100 / LMA 0xce / 2 bytes, .bss still starts at 0x00800102.
 * How: new RAM symbols are uninitialized (tentative definitions go to
 * .bss, never .data); the local static carries __attribute__((used)) so
 * it is emitted with zero codegen and no -Wunused-variable diagnostic;
 * eeprom_value lives in the separate .eeprom address space.
 *
 * XMEGA note: classic PORTB/DDRB are uint8_t SFRs, but on XMEGA PORTB
 * is a PORT_t struct — hence the __AVR_XMEGA__ branch (DIRSET/OUTSET).
 * Fixture-only portability; no product-code family special-casing.
 * (R8 triage 2026-09-27: xmega is deferred from the gen.sh matrix — not
 * because of this file, which builds cleanly for atxmega128a1, but
 * because avrmem's probe cannot parse xmega's indirect geometry macros.
 * The branch below is therefore build-verified manually, not by the
 * matrix. See gen.sh MATRIX_DEVICES comment for the full evidence.)
 *
 * The #include <avr/io.h> also smoke-proves avr-libc presence: if the
 * include fails, gen.sh reports SKIP (incomplete toolchain), not failure.
 */
#include <avr/io.h>
#include <avr/eeprom.h>
#include <stdint.h>

/* .bss: uninitialized volatile; startup code zeroes it. */
volatile uint8_t counter_bss;

/* .data: initialized; VMA in SRAM, LMA in FLASH (copy loop at startup). */
uint8_t initialized_value = 0xA5;

/* .noinit: preserved across resets; SRAM, no LMA, never zeroed/copied. */
uint8_t preserved_value __attribute__((section(".noinit")));

/* .eeprom: EEPROM byte, VMA 0x00810000; programmed separately, not copied. */
uint8_t eeprom_value EEMEM = 0x5A;

/* Duplicate-name pair: file-scope tentative definition (-> .bss, GLOBAL)
 * plus a function-local static of the same name (-> .bss, LOCAL, emitted
 * as dup_cnt.NNNN thanks to __attribute__((used))). Both link and both
 * are emitted; see header comment for the resolution winner. */
uint8_t dup_cnt;

int main(void) {
  static uint8_t dup_cnt __attribute__((used));
#ifdef __AVR_XMEGA__
  PORTB.DIRSET = 0xFF;
  PORTB.OUTSET = initialized_value;
#else
  DDRB |= 0xFF;
  PORTB = initialized_value;
#endif
  counter_bss++;
  preserved_value = counter_bss;
  return 0;
}
