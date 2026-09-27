# ----------------------------------------------------------------------
# avrmem
#
# Build configuration for the AVR ELF memory/symbol explorer.
#
# Project layout:
#
#   src/      C source files
#   include/  public/internal headers
#   bin/      generated executable
#   docs/     Jekyll documentation
#
# Normal build:
#
#   make
#
# Debug build:
#
#   make debug
#   make DEBUG=1
#   make DEBUG=yes
# ----------------------------------------------------------------------

CC ?= gcc

TARGET := bin/avrmem

SOURCES := \
  src/avr_device_probe.c \
  src/avr_sfr.c \
  src/avr_elf.c \
  src/avr_device.c \
  src/avrmem.c

CPPFLAGS := -Iinclude

CFLAGS := \
  -std=c11 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -O2

LDFLAGS :=
LDLIBS :=


# ----------------------------------------------------------------------
# Debug configuration
# ----------------------------------------------------------------------
#
# Any of these enables the debug configuration:
#
#   make debug
#   make DEBUG=1
#   make DEBUG=yes
#   make DEBUG=true
#   make DEBUG=on
#
# Case-insensitive variants are accepted.
#
# Debug configuration:
#
#   -O0
#   -g3
#   -DDEBUG
# ----------------------------------------------------------------------

DEBUG_VALUE := $(strip $(DEBUG))

ifneq ($(filter 1 yes YES y Y true TRUE on ON debug DEBUG,$(DEBUG_VALUE)),)
  CFLAGS := \
    -std=c11 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -O0 \
    -g3 \
    -DDEBUG
endif


# ----------------------------------------------------------------------
# Default target
# ----------------------------------------------------------------------

.PHONY: all
all: $(TARGET)


# ----------------------------------------------------------------------
# Debug target
# ----------------------------------------------------------------------

.PHONY: debug
debug:
	$(MAKE) DEBUG=1 all


# ----------------------------------------------------------------------
# Executable
# ----------------------------------------------------------------------

$(TARGET): $(SOURCES) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -o $@


# ----------------------------------------------------------------------
# Output directory
# ----------------------------------------------------------------------

.PHONY: bin
bin:
	mkdir -p $@


# ----------------------------------------------------------------------
# Clean
# ----------------------------------------------------------------------

.PHONY: clean
clean:
	-rm -f $(TARGET)
	-rm -f $(TEST_BIN_DIR)/test_*
	-rm -rf tests/fixtures/out
	-rm -rf docs/html


# ----------------------------------------------------------------------
# Rebuild
# ----------------------------------------------------------------------

.PHONY: rebuild
rebuild: clean all


# ----------------------------------------------------------------------
# Tests (greatest.h, see tests/vendor/README.md)
# ----------------------------------------------------------------------
#
#   make test      Build and run the full suite (unit tiers always run;
#                  toolchain-gated tiers SKIP when avr-gcc is absent)
#   make check     Alias for `make test`
#   make test-asan Same suite with AddressSanitizer + UBSan (test binaries)
#   make vendor-sync
#                  Update the greatest.h submodule (network) and refresh
#                  the offline fallback copy (only sync entry point)
#
# Test sources are picked up automatically: tests/test_*.c -> tests/bin/*.
# Each runner links only the modules it exercises (explicit rules below).
# greatest.h comes from the submodule when initialized, else from the
# bundled offline copy — see tests/vendor/README.md. Normal builds never
# touch the network; only `make vendor-sync` does.
# ----------------------------------------------------------------------

TEST_BIN_DIR := tests/bin

TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_RUNNERS := $(patsubst tests/%.c,$(TEST_BIN_DIR)/%,$(TEST_SOURCES))

GREATEST_INCLUDE := $(shell tests/vendor/resolve_greatest.sh)

# Sanitizer flags apply to test binaries only (set by `make test-asan`).
TEST_SAN :=

# Extra env for running test binaries (set by `make test-asan`).
#
# allocator_may_return_null=1 makes ASan emulate glibc on huge
# allocations (return NULL so the parser's malloc-failure path runs)
# instead of aborting the runner. Needed by the directory-open case in
# test_elf_negative, which deliberately triggers a LONG_MAX-sized
# allocation that real malloc refuses gracefully.
TEST_ENV :=

$(TEST_BIN_DIR):
	mkdir -p $@

$(TEST_BIN_DIR)/test_sfr: tests/test_sfr.c src/avr_sfr.c | $(TEST_BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SAN) -I$(GREATEST_INCLUDE) tests/test_sfr.c src/avr_sfr.c -o $@

# NOTE: test_device links avr_device_probe.c too — avr_device_init
# references the probe layer, so the linker needs it even though no test
# calls init (fixtures are hand-built, toolchain-free).
$(TEST_BIN_DIR)/test_device: tests/test_device.c src/avr_device.c src/avr_sfr.c src/avr_device_probe.c | $(TEST_BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SAN) -I$(GREATEST_INCLUDE) tests/test_device.c src/avr_device.c src/avr_sfr.c src/avr_device_probe.c -o $@

# NOTE: test_elf_negative carries its own guarded _POSIX_C_SOURCE for
# mkstemp, so plain $(CFLAGS) suffice.
$(TEST_BIN_DIR)/test_elf_negative: tests/test_elf_negative.c src/avr_elf.c src/avr_device.c src/avr_sfr.c src/avr_device_probe.c | $(TEST_BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SAN) -I$(GREATEST_INCLUDE) tests/test_elf_negative.c src/avr_elf.c src/avr_device.c src/avr_sfr.c src/avr_device_probe.c -o $@

# NOTE: gated integration suite — reads FIXTURE_DIR (default
# tests/fixtures/out) and SKIPs (exit 0) when firmware.elf is absent.
$(TEST_BIN_DIR)/test_integration: tests/test_integration.c src/avr_elf.c src/avr_device.c src/avr_sfr.c src/avr_device_probe.c | $(TEST_BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SAN) -I$(GREATEST_INCLUDE) tests/test_integration.c src/avr_elf.c src/avr_device.c src/avr_sfr.c src/avr_device_probe.c -o $@

.PHONY: test
test: all $(TEST_RUNNERS)
	@if [ -x tests/vendor/test_resolve.sh ]; then tests/vendor/test_resolve.sh || exit 1; fi
	@if [ -x tests/fixtures/gen.sh ]; then tests/fixtures/gen.sh; fi
	@if [ -z "$(TEST_RUNNERS)" ]; then printf '%s\n' 'No test runners found.'; fi
	@for t in $(TEST_RUNNERS); do printf '== %s\n' "$$t"; $(TEST_ENV) ./$$t || exit 1; done
	@if [ -x tests/cli_tests.sh ]; then tests/cli_tests.sh; fi
	@printf '%s\n' 'All tests passed.'

.PHONY: check
check: test

.PHONY: test-asan
test-asan: TEST_SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
test-asan: TEST_ENV := ASAN_OPTIONS=allocator_may_return_null=1
test-asan: clean test


# ----------------------------------------------------------------------
# Vendor sync (greatest.h submodule — the ONLY network-touching target)
# ----------------------------------------------------------------------
#
# Updates the pinned submodule, then refreshes the bundled offline
# fallback from it so the two can never silently diverge. Run the full
# suite afterwards and commit everything (gitlink + fallback + docs).
# ----------------------------------------------------------------------

.PHONY: vendor-sync
vendor-sync:
	git submodule update --init --depth 1 tests/vendor/greatest && \
	grep -q 'GREATEST_VERSION_MAJOR 1' tests/vendor/greatest/greatest.h && \
	cp tests/vendor/greatest/greatest.h tests/vendor/greatest.h && \
	printf '%s\n' 'vendor-sync: fallback refreshed from submodule.' \
	  'Now run: make clean && make test && make test-asan'


# ----------------------------------------------------------------------
# Doxygen API documentation (output: docs/html/, gitignored)
# ----------------------------------------------------------------------
#
#   make doxygen    Generate API docs with doxygen (needs doxygen + dot)
#   make clean-docs Remove the generated docs only (`clean` does too)
#
# The Doxyfile covers README.md, include/, src/, and tests/ (minus
# tests/vendor). docs/index.md links docs/html/ for Jekyll passthrough;
# generate before serving or building the site locally.
# ----------------------------------------------------------------------

.PHONY: doxygen
doxygen:
	@if ! command -v doxygen >/dev/null 2>&1; then \
	  printf '%s\n' 'error: doxygen not found in PATH' >&2; \
	  exit 1; \
	fi
	doxygen Doxyfile

.PHONY: clean-docs
clean-docs:
	-rm -rf docs/html


# ----------------------------------------------------------------------
# Documentation helper
# ----------------------------------------------------------------------

.PHONY: docs
docs:
	@printf '%s\n' \
	  'Documentation is in docs/.' \
	  'The directory is configured as a Jekyll site.' \
	  'API docs: run `make doxygen` (output: docs/html/),' \
	  '`make clean-docs` removes them.'


# ----------------------------------------------------------------------
# Help
# ----------------------------------------------------------------------

.PHONY: help
help:
	@printf '%s\n' \
	  'Targets:' \
  '  make            Build bin/avrmem' \
  '  make debug      Build a debug configuration' \
  '  make test       Build and run the test suite' \
  '  make check      Alias for `make test`' \
  '  make test-asan  Run the test suite with ASan/UBSan' \
  '  make vendor-sync Update greatest.h submodule + offline fallback' \
  '  make doxygen    Generate Doxygen API docs into docs/html/' \
  '  make clean-docs Remove generated Doxygen docs' \
  '  make clean      Remove build, test, and generated-docs artifacts' \
  '  make rebuild    Clean and rebuild' \
	  '  make docs       Show documentation information' \
	  '  make help       Show this help' \
	  '' \
	  'Debug variables:' \
	  '  make DEBUG=1' \
	  '  make DEBUG=yes' \
	  '  make DEBUG=true' \
	  '  make DEBUG=on'
