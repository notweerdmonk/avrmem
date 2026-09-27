/*
 * libutil.c — Q4 static-archive fixture source.
 *
 * Provides one tiny function, libutil_add(), archived with avr-ar into
 * libfixture.a to prove the archive code path (one more .text symbol
 * outside firmware.elf). No AVR headers needed; plain C11 + stdint.h.
 */
#include <stdint.h>

uint8_t libutil_add(uint8_t a, uint8_t b) {
  return (uint8_t)(a + b);
}
