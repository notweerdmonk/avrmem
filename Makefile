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


# ----------------------------------------------------------------------
# Rebuild
# ----------------------------------------------------------------------

.PHONY: rebuild
rebuild: clean all


# ----------------------------------------------------------------------
# Documentation helper
# ----------------------------------------------------------------------

.PHONY: docs
docs:
	@printf '%s\n' \
	  'Documentation is in docs/.' \
	  'The directory is configured as a Jekyll site.'


# ----------------------------------------------------------------------
# Help
# ----------------------------------------------------------------------

.PHONY: help
help:
	@printf '%s\n' \
	  'Targets:' \
	  '  make            Build bin/avrmem' \
	  '  make debug      Build a debug configuration' \
	  '  make clean      Remove bin/avrmem' \
	  '  make rebuild    Clean and rebuild' \
	  '  make docs       Show documentation information' \
	  '  make help       Show this help' \
	  '' \
	  'Debug variables:' \
	  '  make DEBUG=1' \
	  '  make DEBUG=yes' \
	  '  make DEBUG=true' \
	  '  make DEBUG=on'
