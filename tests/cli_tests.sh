#!/bin/sh
# cli_tests.sh — Q6 black-box CLI suite driving bin/avrmem (POSIX sh).
#
# Invoke from the project root (do NOT cd; paths below are root-relative):
#
#   sh tests/cli_tests.sh
#   FIXTURE_DIR=tests/fixtures/out sh tests/cli_tests.sh
#
# Gate: when the fixture ELF or bin/avrmem is absent (e.g. no avr-gcc
# toolchain), print a SKIP note and exit 0 so `make test` stays green.
#
# Case matrix: docs/testing.md sections 8-13 (valid/invalid CLI matrix),
# with output-content greps from sections 15/17/18/33. Grep labels below
# were read from real `bin/avrmem` output, not guessed:
#
#   --symbol main               -> "Memory: ... FLASH",
#                                "AVR word address:   0x..."
#   --symbol initialized_value  -> "Memory: ... SRAM",
#                                "LMA:                0x..." (+ Symbol LMA)
#   --symbol counter_bss        -> "Memory: ... SRAM",
#                                "Initialization:      ZERO (.bss)"
#   --map                       -> "FLASH" / "SRAM" / "EEPROM" region lines
#
# Stripped-ELF behavior (verified by running, trusted over docs):
#   --sections stripped.elf       exits 0 (sections resolve, Symbols: 0)
#   --symbol main stripped.elf    exits non-zero, diagnostic on stderr:
#     "error: symbol 'main' not found or could not be resolved"
#
# Plan 2 Q1 adds: expect_fail_grep (nonzero + literal greps) and
# expect_ok_absent (exit 0 + literal absence) helpers; error-diagnostic
# cases (invalid.bin, missing file, empty/injected/bad --device,
# no_metadata guidance, unwritable TMPDIR); output-depth greps
# (--memory sizes, --sections rows, FILE-symbol filtering, --map
# init wording); short-option edges; archive/host negative inputs;
# 328p-gated exact-address greps; and a PATH-shim probe failure case.
set -eu

FIXTURE_DIR="${FIXTURE_DIR:-tests/fixtures/out}"
FIRMWARE="$FIXTURE_DIR/firmware.elf"
STRIPPED="$FIXTURE_DIR/stripped.elf"
NOMETA="$FIXTURE_DIR/no_metadata.elf"
INVALID="$FIXTURE_DIR/invalid.bin"
LIBA="$FIXTURE_DIR/libfixture.a"
HOSTSO="$FIXTURE_DIR/host.so"
MISSING="$FIXTURE_DIR/does-not-exist.elf"
AVRMEM="bin/avrmem"
DEVICE="atmega328p"

if [ ! -r "$FIRMWARE" ] || [ ! -x "$AVRMEM" ]; then
  echo "SKIP: fixtures or bin/avrmem missing (run make test with avr-gcc)"
  exit 0
fi

pass=0
fail=0
TMPBASE="${TMPDIR:-/tmp}"

# expect_ok <desc> -- <cmd...>: command must exit 0.
expect_ok() {
  _desc=$1
  shift
  if [ "${1:-}" != "--" ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    return 0
  fi
  shift
  _out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
  if "$@" >"$_out" 2>&1; then
    echo "ok - $_desc"
    pass=$((pass + 1))
    rm -f "$_out"
  else
    _status=$?
    echo "not ok - $_desc (exit $_status; output kept in $_out)"
    fail=$((fail + 1))
  fi
}

# expect_fail <desc> -- <cmd...>: command must exit non-zero.
expect_fail() {
  _desc=$1
  shift
  if [ "${1:-}" != "--" ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    return 0
  fi
  shift
  _out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
  if "$@" >"$_out" 2>&1; then
    echo "not ok - $_desc (expected failure, got exit 0; output kept in $_out)"
    fail=$((fail + 1))
  else
    _status=$?
    echo "ok - $_desc"
    pass=$((pass + 1))
    rm -f "$_out"
  fi
}

# expect_ok_grep <desc> <pattern>... -- <cmd...>: exit 0 AND every
# literal pattern present in captured stdout+stderr (grep -qF).
expect_ok_grep() {
  _desc=$1
  shift
  _pat=$(mktemp "$TMPBASE/avrmem_pat.XXXXXX")
  while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then
      break
    fi
    printf '%s\n' "$1" >>"$_pat"
    shift
  done
  if [ $# -eq 0 ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    rm -f "$_pat"
    return 0
  fi
  shift
  _out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
  if "$@" >"$_out" 2>&1; then
    _missing=""
    while IFS= read -r _line; do
      if ! grep -qF -- "$_line" "$_out"; then
        _missing="$_missing [$_line]"
      fi
    done <"$_pat"
    if [ -z "$_missing" ]; then
      echo "ok - $_desc"
      pass=$((pass + 1))
      rm -f "$_out"
    else
      echo "not ok - $_desc (missing output:$_missing; output kept in $_out)"
      fail=$((fail + 1))
    fi
  else
    _status=$?
    echo "not ok - $_desc (exit $_status; output kept in $_out)"
    fail=$((fail + 1))
  fi
  rm -f "$_pat"
}

# expect_fail_grep <desc> <pattern>... -- <cmd...>: exit non-zero AND
# every literal pattern present in captured stdout+stderr (grep -qF).
expect_fail_grep() {
  _desc=$1
  shift
  _pat=$(mktemp "$TMPBASE/avrmem_pat.XXXXXX")
  while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then
      break
    fi
    printf '%s\n' "$1" >>"$_pat"
    shift
  done
  if [ $# -eq 0 ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    rm -f "$_pat"
    return 0
  fi
  shift
  _out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
  if "$@" >"$_out" 2>&1; then
    echo "not ok - $_desc (expected failure, got exit 0; output kept in $_out)"
    fail=$((fail + 1))
  else
    _missing=""
    while IFS= read -r _line; do
      if ! grep -qF -- "$_line" "$_out"; then
        _missing="$_missing [$_line]"
      fi
    done <"$_pat"
    if [ -z "$_missing" ]; then
      echo "ok - $_desc"
      pass=$((pass + 1))
      rm -f "$_out"
    else
      echo "not ok - $_desc (missing output:$_missing; output kept in $_out)"
      fail=$((fail + 1))
    fi
  fi
  rm -f "$_pat"
}

# expect_ok_absent <desc> <pattern>... -- <cmd...>: exit 0 AND every
# literal pattern absent from captured stdout+stderr (grep -qF).
expect_ok_absent() {
  _desc=$1
  shift
  _pat=$(mktemp "$TMPBASE/avrmem_pat.XXXXXX")
  while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then
      break
    fi
    printf '%s\n' "$1" >>"$_pat"
    shift
  done
  if [ $# -eq 0 ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    rm -f "$_pat"
    return 0
  fi
  shift
  _out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
  if "$@" >"$_out" 2>&1; then
    _present=""
    while IFS= read -r _line; do
      if grep -qF -- "$_line" "$_out"; then
        _present="$_present [$_line]"
      fi
    done <"$_pat"
    if [ -z "$_present" ]; then
      echo "ok - $_desc"
      pass=$((pass + 1))
      rm -f "$_out"
    else
      echo "not ok - $_desc (unexpected output:$_present; output kept in $_out)"
      fail=$((fail + 1))
    fi
  else
    _status=$?
    echo "not ok - $_desc (exit $_status; output kept in $_out)"
    fail=$((fail + 1))
  fi
  rm -f "$_pat"
}

# expect_row_absent <desc> <row-pattern> <bad-pattern> -- <cmd...>:
# exit 0 AND no output line matching row-pattern (ERE) contains
# bad-pattern (literal, grep -qF). Row selection and badness check are
# deliberately split so the test pins "this row must not claim X"
# without prescribing the replacement rendering.
expect_row_absent() {
  _desc=$1
  _row=${2:-}
  _bad=${3:-}
  shift 3
  if [ -z "${_row:-}" ] || [ -z "${_bad:-}" ]; then
    echo "not ok - $_desc (harness error: need row-pattern and bad-pattern)"
    fail=$((fail + 1))
    return 0
  fi
  if [ "${1:-}" != "--" ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    return 0
  fi
  shift
  _out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
  if "$@" >"$_out" 2>&1; then
    if grep -E -- "$_row" "$_out" | grep -qF -- "$_bad"; then
      echo "not ok - $_desc (row matching [$_row] claims [$_bad]; output kept in $_out)"
      fail=$((fail + 1))
    else
      echo "ok - $_desc"
      pass=$((pass + 1))
      rm -f "$_out"
    fi
  else
    _status=$?
    echo "not ok - $_desc (exit $_status; output kept in $_out)"
    fail=$((fail + 1))
  fi
}

# ----------------------------------------------------------------------
# Valid cases: default + display modes, auto-device and explicit --device
# ----------------------------------------------------------------------
expect_ok "default (auto device)" -- "$AVRMEM" "$FIRMWARE"
expect_ok "default (explicit device)" -- "$AVRMEM" --device "$DEVICE" "$FIRMWARE"

expect_ok "--memory (auto device)" -- "$AVRMEM" --memory "$FIRMWARE"
expect_ok "--memory (explicit device)" -- "$AVRMEM" --device "$DEVICE" --memory "$FIRMWARE"

expect_ok "--sections (auto device)" -- "$AVRMEM" --sections "$FIRMWARE"
expect_ok "--sections (explicit device)" -- "$AVRMEM" --device "$DEVICE" --sections "$FIRMWARE"

expect_ok "--symbols (auto device)" -- "$AVRMEM" --symbols "$FIRMWARE"
expect_ok "--symbols (explicit device)" -- "$AVRMEM" --device "$DEVICE" --symbols "$FIRMWARE"

expect_ok_grep "--map shows FLASH/SRAM/EEPROM (auto device)" \
  FLASH SRAM EEPROM -- "$AVRMEM" --map "$FIRMWARE"
expect_ok_grep "--map shows FLASH/SRAM/EEPROM (explicit device)" \
  FLASH SRAM EEPROM -- "$AVRMEM" --device "$DEVICE" --map "$FIRMWARE"

# ----------------------------------------------------------------------
# Valid cases: per-symbol detail with output-content greps
# ----------------------------------------------------------------------
expect_ok_grep "--symbol main is FLASH with word address (auto device)" \
  FLASH "AVR word address:" -- "$AVRMEM" --symbol main "$FIRMWARE"
expect_ok_grep "--symbol main is FLASH with word address (explicit device)" \
  FLASH "AVR word address:" -- "$AVRMEM" --device "$DEVICE" --symbol main "$FIRMWARE"

expect_ok_grep "--symbol initialized_value is SRAM with LMA (auto device)" \
  SRAM "LMA:" -- "$AVRMEM" --symbol initialized_value "$FIRMWARE"
expect_ok_grep "--symbol initialized_value is SRAM with LMA (explicit device)" \
  SRAM "LMA:" -- "$AVRMEM" --device "$DEVICE" --symbol initialized_value "$FIRMWARE"

expect_ok_grep "--symbol counter_bss is SRAM zero-initialized (auto device)" \
  SRAM "ZERO" -- "$AVRMEM" --symbol counter_bss "$FIRMWARE"
expect_ok_grep "--symbol counter_bss is SRAM zero-initialized (explicit device)" \
  SRAM "ZERO" -- "$AVRMEM" --device "$DEVICE" --symbol counter_bss "$FIRMWARE"

# ----------------------------------------------------------------------
# Valid cases: combined options + short options
# ----------------------------------------------------------------------
expect_ok "combined --device --sections --symbols" -- \
  "$AVRMEM" --device "$DEVICE" --sections --symbols "$FIRMWARE"

expect_ok "-h (no ELF needed)" -- "$AVRMEM" -h
expect_ok "-m firmware" -- "$AVRMEM" -m "$FIRMWARE"
expect_ok "-c firmware" -- "$AVRMEM" -c "$FIRMWARE"
expect_ok "-s firmware" -- "$AVRMEM" -s "$FIRMWARE"
expect_ok "-p firmware" -- "$AVRMEM" -p "$FIRMWARE"
expect_ok "-d atmega328p firmware" -- "$AVRMEM" -d "$DEVICE" "$FIRMWARE"
expect_ok "-y main firmware" -- "$AVRMEM" -y main "$FIRMWARE"

# ----------------------------------------------------------------------
# Fixture case: stripped ELF (no .symtab) — sections work, symbols fail
# ----------------------------------------------------------------------
expect_ok "--sections stripped.elf" -- "$AVRMEM" --sections "$STRIPPED"
expect_fail "--symbol main stripped.elf (no symtab)" -- \
  "$AVRMEM" --symbol main "$STRIPPED"

# ----------------------------------------------------------------------
# Invalid cases: must exit non-zero
# ----------------------------------------------------------------------
expect_fail "no args" -- "$AVRMEM"
expect_fail "option after ELF rejected (order quirk)" -- \
  "$AVRMEM" "$FIRMWARE" --symbols
expect_fail "--device without value" -- "$AVRMEM" --device
expect_fail "--symbol without value" -- "$AVRMEM" --symbol
expect_fail "--unknown option" -- "$AVRMEM" --unknown "$FIRMWARE"
expect_fail "two ELF files" -- "$AVRMEM" "$FIRMWARE" "$STRIPPED"
expect_fail "duplicate --device" -- \
  "$AVRMEM" --device "$DEVICE" --device "$DEVICE" "$FIRMWARE"
expect_fail "duplicate --symbol" -- \
  "$AVRMEM" --symbol main --symbol main "$FIRMWARE"

# ----------------------------------------------------------------------
# Plan 2 Q1: error diagnostics with output-content greps
# (labels read from live bin/avrmem output, never guessed)
# ----------------------------------------------------------------------
expect_fail_grep "--sections invalid.bin" \
  "unable to open or parse" -- "$AVRMEM" --sections "$INVALID"
expect_fail "missing ELF file" -- "$AVRMEM" --sections "$MISSING"
expect_fail_grep "--device empty string" \
  "requires a device name" -- "$AVRMEM" --device "" "$FIRMWARE"

_sentinel="/tmp/avrmem_cli_pwned"
rm -f "$_sentinel"
expect_fail_grep "injection --device rejected" \
  "invalid device-probe arguments" -- \
  "$AVRMEM" --device 'x;touch /tmp/avrmem_cli_pwned' "$FIRMWARE"
if [ -e "$_sentinel" ]; then
  echo "not ok - injection left no sentinel file (found $_sentinel)"
  fail=$((fail + 1))
else
  echo "ok - injection left no sentinel file"
  pass=$((pass + 1))
fi

expect_fail_grep "invalid --device names the device" \
  "this_is_not_an_avr" -- \
  "$AVRMEM" --device this_is_not_an_avr "$FIRMWARE"
expect_fail_grep "auto-device on no_metadata.elf guides to --device" \
  -- --device -- "$AVRMEM" "$NOMETA"

# Direct redirect, never a pipe (pipes mask the exit code). The env
# prefix affects only the child: TMPBASE was captured at script top,
# so harness captures still land in the real TMPDIR.
expect_fail_grep "unwritable TMPDIR fails on mkstemp" \
  "mkstemp" -- env TMPDIR=/nonexistent-dir-xyz "$AVRMEM" "$FIRMWARE"

# ----------------------------------------------------------------------
# Plan 2 Q1: output-depth greps (device-agnostic wording)
# ----------------------------------------------------------------------
expect_ok_grep "--sections header + .bss SRAM row" \
  "Sections" ".bss" "SRAM" -- "$AVRMEM" --sections "$FIRMWARE"
expect_ok_grep "--symbols lists main" \
  "main" -- "$AVRMEM" --symbols "$FIRMWARE"
# FILE symbols (firmware.c in avr-readelf -s) are dropped by the
# useful-policy (OBJECT/FUNC/NOTYPE only, src/avr_elf.c).
expect_ok_absent "--symbols hides FILE symbols" \
  "firmware.c" -- "$AVRMEM" --symbols "$FIRMWARE"
expect_ok_grep "--map .bss zeroing + .noinit preserved" \
  ".bss initialization" "ZERO -> SRAM" ".noinit" \
  "Initialization: preserved" -- "$AVRMEM" --map "$FIRMWARE"
expect_ok "--help (no ELF needed)" -- "$AVRMEM" --help

# ----------------------------------------------------------------------
# Plan 2 Q1: short-option edges + empty --symbol + negative inputs
# ----------------------------------------------------------------------
expect_fail "duplicate short -d" -- \
  "$AVRMEM" -d a -d b "$FIRMWARE"
expect_fail "bare -d" -- "$AVRMEM" -d
expect_fail "bare -y" -- "$AVRMEM" -y
expect_fail "--symbol empty string" -- "$AVRMEM" --symbol "" "$FIRMWARE"
expect_fail "--sections libfixture.a (archive magic)" -- \
  "$AVRMEM" --sections "$LIBA"
if [ -r "$HOSTSO" ]; then
  expect_fail "--sections host.so (non-AVR ELF)" -- \
    "$AVRMEM" --sections "$HOSTSO"
else
  echo "ok - --sections host.so (SKIP: host.so missing, no host cc)"
  pass=$((pass + 1))
fi

# ----------------------------------------------------------------------
# Plan 2 Q1: exact-size greps, 328p-gated (toolchain-self-consistent)
# ----------------------------------------------------------------------
if [ "$DEVICE" = "atmega328p" ]; then
  expect_ok_grep "--memory reports 328p sizes" \
    "32768" "2048" "1024" "2304" -- "$AVRMEM" --memory "$FIRMWARE"
  expect_ok_grep "--sections .data row (328p addresses)" \
    ".data" "0x00800100" "0x000000ce" "SRAM" "AW" -- \
    "$AVRMEM" --sections "$FIRMWARE"
  expect_ok_grep "--map .data init mapping (328p addresses)" \
    ".data initialization" "FLASH 0x000000ce -> SRAM 0x00000100" \
    "ZERO -> SRAM 0x00000102" -- "$AVRMEM" --map "$FIRMWARE"
fi

# ----------------------------------------------------------------------
# Plan 2 Q1 A8: probe without avr-gcc via PATH shim (sh only, no avr-gcc;
# popen uses /bin/sh -c so PATH hides avr-gcc from the probe child).
# ----------------------------------------------------------------------
_shim=$(mktemp -d "$TMPBASE/avrmem_shim.XXXXXX")
ln -s "$(command -v sh)" "$_shim/sh"
_a8out=$(mktemp "$TMPBASE/avrmem_cli.XXXXXX")
if PATH="$_shim" "$AVRMEM" "$FIRMWARE" >"$_a8out" 2>&1; then
  echo "not ok - PATH-shim without avr-gcc fails (got exit 0; output kept in $_a8out)"
  fail=$((fail + 1))
elif grep -qF "unable to initialize" "$_a8out"; then
  echo "ok - PATH-shim without avr-gcc fails"
  pass=$((pass + 1))
  rm -f "$_a8out"
else
  echo "not ok - PATH-shim without avr-gcc fails (missing output; output kept in $_a8out)"
  fail=$((fail + 1))
fi
rm -rf "$_shim"

# ----------------------------------------------------------------------
# Plan 2 Q5a (FAILING-BY-DESIGN, RED is intentional): non-ALLOC
# presentation defect. --sections currently shows non-allocated VMA-0
# sections as resident FLASH. Each case below asserts the affected row
# does NOT claim FLASH residency; the exact replacement rendering is
# TBD in Q5b, so only the absence of the FLASH claim is pinned.
# ----------------------------------------------------------------------
# Defect: non-ALLOC VMA-0 .debug_info presented as resident FLASH.
# Expectation: row must not claim FLASH residency (replacement TBD Q5b).
# RED intentional until Q5b fix lands.
expect_row_absent "Q5a RED: .debug_info row must not claim FLASH" \
  "\.debug_info" "FLASH" -- "$AVRMEM" --sections "$FIRMWARE"
# Defect: non-ALLOC VMA-0 .comment presented as resident FLASH.
# Expectation: row must not claim FLASH residency (replacement TBD Q5b).
# RED intentional until Q5b fix lands.
expect_row_absent "Q5a RED: .comment row must not claim FLASH" \
  "\.comment" "FLASH" -- "$AVRMEM" --sections "$FIRMWARE"
# Defect: non-ALLOC VMA-0 .note.gnu.avr.deviceinfo presented as resident FLASH.
# Expectation: row must not claim FLASH residency (replacement TBD Q5b).
# RED intentional until Q5b fix lands.
expect_row_absent "Q5a RED: .note.gnu.avr.deviceinfo row must not claim FLASH" \
  "\.note\.gnu\.avr\.deviceinfo" "FLASH" -- "$AVRMEM" --sections "$FIRMWARE"
# Defect: non-ALLOC VMA-0 .stab presented as resident FLASH.
# Expectation: row must not claim FLASH residency (replacement TBD Q5b).
# RED intentional until Q5b fix lands.
expect_row_absent "Q5a RED: .stab row must not claim FLASH" \
  "\.stab[[:space:]]" "FLASH" -- "$AVRMEM" --sections "$FIRMWARE"
# Defect: non-ALLOC NULL section (idx 0) presented as resident FLASH.
# Expectation: row must not claim FLASH residency (replacement TBD Q5b).
# RED intentional until Q5b fix lands.
expect_row_absent "Q5a RED: NULL section idx 0 row must not claim FLASH" \
  "^0[[:space:]]" "FLASH" -- "$AVRMEM" --sections "$FIRMWARE"

echo "$pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
  exit 1
fi
exit 0
