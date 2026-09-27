#!/bin/sh
# test_resolve.sh — hermetic self-test for resolve_greatest.sh (POSIX sh).
#
# No toolchain, no network, no git: builds fake vendor trees under
# mktemp -d, copies the REAL shipped resolver into each tree (so this
# test tracks the script it ships with), and asserts stdout dir +
# exit code (+ stderr empty/non-empty/keyword) per leg. TAP-ish
# ok/not-ok lines, `<N> passed, <M> failed` summary, exit 1 on failure.
#
# Legs: submodule exact v1.5.0 (silent, wins over a present fallback);
# drifted v9.9.9 (still submodule + stderr warning); empty submodule
# dir (fallback + note); empty submodule header file (fallback + note);
# header without the macro (fallback + note); neither source (exit 1);
# CWD-independence via absolute path, direct and `sh <path>` forms;
# relative path in, relative path out.
set -eu

SELF_DIR=$(dirname "$0")
RESOLVER_SRC="$SELF_DIR/resolve_greatest.sh"
TMPBASE="${TMPDIR:-/tmp}"

pass=0
fail=0

if [ ! -r "$RESOLVER_SRC" ]; then
  echo "not ok - shipped resolver missing: $RESOLVER_SRC"
  echo "0 passed, 1 failed"
  exit 1
fi

T=$(mktemp -d "$TMPBASE/resolve_test.XXXXXX")
case $T in
  /*) ;;
  *) T="$PWD/$T" ;;
esac
trap 'rm -rf "$T"' EXIT INT TERM

# new_tree <name>: fake VENDOR_DIR holding a copy of the REAL resolver.
new_tree() {
  _d="$T/$1"
  mkdir -p "$_d/greatest"
  cp "$RESOLVER_SRC" "$_d/resolve_greatest.sh"
  chmod +x "$_d/resolve_greatest.sh"
  printf '%s\n' "$_d"
}

# write_header <path> <major> <minor> <patch>: minimal greatest-like header.
write_header() {
  printf '#define GREATEST_VERSION_MAJOR %s\n#define GREATEST_VERSION_MINOR %s\n#define GREATEST_VERSION_PATCH %s\n' "$2" "$3" "$4" >"$1"
}

# in_dir <dir> -- <cmd...>: run cmd with CWD=dir (subshell, POSIX).
# Lets expect_resolve assert foreign-CWD behavior without sh -c quoting.
in_dir() {
  _dir=$1
  shift
  if [ "${1:-}" != "--" ]; then
    echo "not ok - (harness error: missing -- separator in in_dir)"
    fail=$((fail + 1))
    return 0
  fi
  shift
  (cd "$_dir" && "$@")
}

# expect_resolve <desc> <want_stdout|-> <want_exit> <want_stderr> [want_grep] -- <cmd...>
# want_stderr: empty | nonempty | any. want_grep "-" (default) skips the
# stderr keyword check (literal grep -qF).
expect_resolve() {
  _desc=$1
  _want=$2
  _wx=$3
  _we=$4
  shift 4
  _wg="-"
  if [ "${1:-}" != "--" ]; then
    _wg=$1
    shift
  fi
  if [ "${1:-}" != "--" ]; then
    echo "not ok - $_desc (harness error: missing -- separator)"
    fail=$((fail + 1))
    return 0
  fi
  shift
  _o=$(mktemp "$TMPBASE/resolve_out.XXXXXX")
  _e=$(mktemp "$TMPBASE/resolve_err.XXXXXX")
  _st=0
  if "$@" >"$_o" 2>"$_e"; then
    _st=0
  else
    _st=$?
  fi
  _got=$(cat "$_o")
  _goterr=$(cat "$_e")
  rm -f "$_o" "$_e"
  _bad=""
  if [ "$_st" != "$_wx" ]; then
    _bad="$_bad [exit $_st, want $_wx]"
  fi
  if [ "$_want" != "-" ] && [ "$_got" != "$_want" ]; then
    _bad="$_bad [stdout '$_got', want '$_want']"
  fi
  case $_we in
    empty)
      if [ -n "$_goterr" ]; then
        _bad="$_bad [stderr not empty: '$_goterr']"
      fi
      ;;
    nonempty)
      if [ -z "$_goterr" ]; then
        _bad="$_bad [stderr empty, want diagnostic]"
      fi
      ;;
    any) ;;
    *)
      _bad="$_bad [harness error: bad stderr mode '$_we']"
      ;;
  esac
  if [ "$_wg" != "-" ]; then
    if ! printf '%s\n' "$_goterr" | grep -qF -- "$_wg"; then
      _bad="$_bad [stderr missing '$_wg']"
    fi
  fi
  if [ -z "$_bad" ]; then
    echo "ok - $_desc"
    pass=$((pass + 1))
  else
    echo "not ok -$_bad ($_desc)"
    fail=$((fail + 1))
  fi
}

EXACT=$(new_tree exact)
write_header "$EXACT/greatest/greatest.h" 1 5 0
write_header "$EXACT/greatest.h" 1 5 0
expect_resolve "submodule v1.5.0 exact (silent, wins over fallback)" \
  "$EXACT/greatest" 0 empty -- "$EXACT/resolve_greatest.sh"

DRIFT=$(new_tree drift)
write_header "$DRIFT/greatest/greatest.h" 9 9 9
write_header "$DRIFT/greatest.h" 1 5 0
expect_resolve "submodule v9.9.9 drifted (still used + warning)" \
  "$DRIFT/greatest" 0 nonempty warning -- "$DRIFT/resolve_greatest.sh"

EMPTYD=$(new_tree empty_subdir)
write_header "$EMPTYD/greatest.h" 1 5 0
expect_resolve "empty submodule dir (fallback + note)" \
  "$EMPTYD" 0 nonempty fallback -- "$EMPTYD/resolve_greatest.sh"

CORRUPT=$(new_tree corrupt_empty)
: >"$CORRUPT/greatest/greatest.h"
write_header "$CORRUPT/greatest.h" 1 5 0
expect_resolve "empty submodule header file (fallback + note)" \
  "$CORRUPT" 0 nonempty fallback -- "$CORRUPT/resolve_greatest.sh"

NOMACRO=$(new_tree no_macro)
printf '/* not a greatest header */\nint x;\n' >"$NOMACRO/greatest/greatest.h"
write_header "$NOMACRO/greatest.h" 1 5 0
expect_resolve "macro-less submodule header (fallback + note)" \
  "$NOMACRO" 0 nonempty fallback -- "$NOMACRO/resolve_greatest.sh"

NONE=$(new_tree neither)
expect_resolve "neither source (exit 1)" \
  "-" 1 nonempty error -- "$NONE/resolve_greatest.sh"

ELSEWHERE="$T/elsewhere"
mkdir -p "$ELSEWHERE"
expect_resolve "CWD-independence (absolute path, foreign CWD)" \
  "$EXACT/greatest" 0 empty -- \
  in_dir "$ELSEWHERE" -- "$EXACT/resolve_greatest.sh"
expect_resolve "CWD-independence (sh <abs path>, foreign CWD)" \
  "$EXACT/greatest" 0 empty -- \
  in_dir "$ELSEWHERE" -- sh "$EXACT/resolve_greatest.sh"
expect_resolve "relative path in, relative path out" \
  "exact/greatest" 0 empty -- \
  in_dir "$T" -- exact/resolve_greatest.sh

echo "$pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
  exit 1
fi
exit 0
