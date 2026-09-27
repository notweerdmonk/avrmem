#!/bin/sh
# gen.sh — Q4/Q3 fixture generator (POSIX sh, no bashisms).
#
# Emits built fixtures into tests/fixtures/out/ from the committed sources
# in tests/fixtures/. Idempotent: re-running overwrites the outputs.
#
# Layout: atmega328p outputs keep their historical top-level names/paths
# (firmware.elf, stripped.elf, no_metadata.elf — existing tests must not
# move); every other matrix device gets out/<device>/firmware.elf.
# libfixture.a / invalid.bin / host.so are device-independent and stay
# single top-level files.
#
# SKIP policy: a missing or incomplete AVR toolchain must NEVER fail the
# suite. Missing avr-gcc/avr-objcopy/avr-ar, or a failed 328p firmware
# compile (e.g. <avr/io.h> absent), prints "SKIP: ..." and exits 0.
# Per-device: an unsupported -mmcu skips only that device ("SKIP: ..."),
# still emits the rest, and still exits 0.
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SRC_DIR=$SCRIPT_DIR
LIB_SRC=$SCRIPT_DIR/lib/libutil.c
OUT_DIR=$SCRIPT_DIR/out

# Non-328p matrix devices (328p is built separately above to keep its
# legacy top-level paths).
#
# XMEGA TRIAGE (R8, 2026-09-27): atxmega128a1 is DELIBERATELY absent.
# Its ELF is sane (standard AVR note naming atxmega128a1, data origin
# 0x800000+0x2000, normal sections — verified with avr-readelf -S -s -n),
# but avr-libc's xmega headers define geometry INDIRECTLY
# (RAMSTART->INTERNAL_SRAM_START, RAMEND->INTERNAL_SRAM_END->(START+SIZE-1),
# FLASHEND->PROGMEM_END->(START+SIZE-1), E2END->EEPROM_END->(START+SIZE-1))
# while classic parts use plain literals — so the probe's single-literal
# get_macro_u32() fails physical-memory extraction and avrmem cannot init
# ANY xmega device ("probe: physical-memory extraction FAILED"). Teaching
# the probe chained-macro/expression resolution is product work, out of
# Q3 scope (no family hacks here). Re-add the name below once the probe
# supports it; firmware.c already builds for xmega (portable PORTB).
MATRIX_DEVICES="atmega2560 attiny85"

for tool in avr-gcc avr-objcopy avr-ar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "SKIP: $tool missing, fixtures not generated"
    exit 0
  fi
done

mkdir -p "$OUT_DIR"

# firmware.elf: compile failure (e.g. missing avr-libc headers) is SKIP.
if ! avr-gcc -mmcu=atmega328p -Os -g -o "$OUT_DIR/firmware.elf" "$SRC_DIR/firmware.c"; then
  echo "SKIP: avr-gcc firmware build failed, fixtures not generated"
  exit 0
fi
echo "GEN $OUT_DIR/firmware.elf"

avr-objcopy --strip-all "$OUT_DIR/firmware.elf" "$OUT_DIR/stripped.elf"
echo "GEN $OUT_DIR/stripped.elf"

avr-objcopy -R .note.gnu.avr.deviceinfo "$OUT_DIR/firmware.elf" "$OUT_DIR/no_metadata.elf"
echo "GEN $OUT_DIR/no_metadata.elf"

# Per-device firmware.elf into out/<device>/; a device whose -mmcu the
# toolchain rejects is skipped without failing the rest.
for dev in $MATRIX_DEVICES; do
  DEV_OUT="$OUT_DIR/$dev"
  mkdir -p "$DEV_OUT"
  if avr-gcc -mmcu="$dev" -Os -g -o "$DEV_OUT/firmware.elf" "$SRC_DIR/firmware.c"; then
    echo "GEN $DEV_OUT/firmware.elf"
  else
    echo "SKIP: -mmcu=$dev unsupported, $dev fixtures not generated"
    rm -f "$DEV_OUT/firmware.elf"
    rmdir "$DEV_OUT" 2>/dev/null || true
  fi
done

# Prune stale per-device dirs (a device dropped from MATRIX_DEVICES, e.g.
# the R8 xmega deferral): gen.sh owns the out/ layout, so re-running
# converges to it. Unmatched globs stay literal in POSIX sh — skip those.
for stale in "$OUT_DIR"/*/; do
  if [ ! -d "$stale" ]; then
    continue
  fi
  stale_dev=${stale%/}
  stale_dev=${stale_dev##*/}
  keep=false
  for dev in $MATRIX_DEVICES; do
    if [ "$stale_dev" = "$dev" ]; then
      keep=true
      break
    fi
  done
  if [ "$keep" = false ]; then
    rm -rf "$stale"
    echo "PRUNE $stale"
  fi
done

avr-gcc -mmcu=atmega328p -Os -g -c -o "$OUT_DIR/libutil.o" "$LIB_SRC"
avr-ar rcs "$OUT_DIR/libfixture.a" "$OUT_DIR/libutil.o"
rm -f "$OUT_DIR/libutil.o"
echo "GEN $OUT_DIR/libfixture.a"

printf 'not an ELF file\n' > "$OUT_DIR/invalid.bin"
echo "GEN $OUT_DIR/invalid.bin"

# host.so: negative input for the ELF parser (host shared object, not AVR).
# Skipped silently when no host compiler exists; never fails the suite.
if command -v cc >/dev/null 2>&1; then
  HOST_SRC=$OUT_DIR/hostutil.c
  printf 'int hostutil_add(int a, int b) { return a + b; }\n' > "$HOST_SRC"
  if cc -shared -fPIC -o "$OUT_DIR/host.so" "$HOST_SRC" 2>/dev/null; then
    echo "GEN $OUT_DIR/host.so"
  else
    echo "SKIP: host cc failed, host.so not generated"
  fi
  rm -f "$HOST_SRC"
else
  echo "SKIP: cc missing, host.so not generated"
fi
