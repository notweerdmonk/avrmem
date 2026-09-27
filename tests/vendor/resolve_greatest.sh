#!/bin/sh
# resolve_greatest.sh — pick the greatest.h include dir (POSIX sh).
#
# Inspection-only: never calls git, never touches the network. Resolves
# paths relative to this script's own directory (dirname "$0"), never
# the caller's CWD, so all of these work from any directory:
#
#   tests/vendor/resolve_greatest.sh
#   /abs/path/tests/vendor/resolve_greatest.sh
#   sh tests/vendor/resolve_greatest.sh
#
# Order: (1) tests/vendor/greatest/greatest.h (git submodule) sane
# (exists, non-empty, defines GREATEST_VERSION_MAJOR) wins; a version
# other than v1.5.0 warns on stderr but is still used (explicit-sync
# policy: sync lives only in `make vendor-sync`). (2) Else the offline
# fallback tests/vendor/greatest.h + stderr note. (3) Else stderr error
# + exit 1.
#
# Success prints exactly one line (the include dir, in the same form as
# invoked: relative in, relative out) on stdout; every diagnostic goes
# to stderr.
set -eu

VENDOR_DIR=$(dirname "$0")
SUB_H="$VENDOR_DIR/greatest/greatest.h"
FB_H="$VENDOR_DIR/greatest.h"

if [ -s "$SUB_H" ] && grep -q "GREATEST_VERSION_MAJOR" "$SUB_H" 2>/dev/null; then
  _major=$(sed -n 's/^#define[[:space:]][[:space:]]*GREATEST_VERSION_MAJOR[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' "$SUB_H")
  _minor=$(sed -n 's/^#define[[:space:]][[:space:]]*GREATEST_VERSION_MINOR[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' "$SUB_H")
  _patch=$(sed -n 's/^#define[[:space:]][[:space:]]*GREATEST_VERSION_PATCH[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' "$SUB_H")
  if grep -q "GREATEST_VERSION_MAJOR 1$" "$SUB_H" 2>/dev/null \
      && [ "${_minor:-?}" = "5" ] && [ "${_patch:-?}" = "0" ]; then
    : # pinned v1.5.0: silent
  else
    printf '%s\n' "resolve_greatest.sh: warning: $SUB_H is v${_major:-?}.${_minor:-?}.${_patch:-?} (expected v1.5.0); using it anyway (run make vendor-sync to re-pin)" >&2
  fi
  printf '%s\n' "$VENDOR_DIR/greatest"
  exit 0
fi

if [ -s "$FB_H" ] && grep -q "GREATEST_VERSION_MAJOR" "$FB_H" 2>/dev/null; then
  printf '%s\n' "resolve_greatest.sh: note: using offline fallback $FB_H (submodule missing or invalid)" >&2
  printf '%s\n' "$VENDOR_DIR"
  exit 0
fi

printf '%s\n' "resolve_greatest.sh: error: no usable greatest.h (checked $SUB_H and $FB_H)" >&2
exit 1
