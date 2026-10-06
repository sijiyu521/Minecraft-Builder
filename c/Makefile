# Makefile — Minecraft Builder, C edition
#
# Zero third-party dependencies. On Windows the only external library is
# winhttp, which ships with the OS; the code falls back to spawning curl.exe.
#
#   mingw32-make            build build/mb.exe
#   mingw32-make run        build and launch the TUI
#   mingw32-make test       build and run the golden-value test suite
#   mingw32-make clean      remove build/
#
# Use mingw32-make: plain `make` is not part of the winlibs toolchain.

CC       ?= gcc
# GNU make ships with a built-in `CC = cc`, and on Windows there is rarely a
# `cc`, so `?=` alone would silently keep the broken default. Detect that case.
ifeq ($(origin CC),default)
  CC := gcc
endif

CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
CPPFLAGS := -Iinclude

BUILD    := build
BIN      := $(BUILD)/mb.exe
TESTBIN  := $(BUILD)/mb_tests.exe

SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

# main.o is excluded from the test binary so the tests own the entry point.
TESTOBJ  := $(filter-out $(BUILD)/main.o,$(OBJ)) $(BUILD)/tests/test_core.o

# ---------------------------------------------------------------------------
# Platform quirks
# ---------------------------------------------------------------------------
# 1. winlibs/MinGW-w64 installed in a path that contains a space — the usual
#    "C:\Program Files (x86)\..." location — cannot link *anything*. GCC's
#    *endfile spec injects default-manifest.o as an unquoted absolute path
#    full of spaces, ld splits that argument, and the link dies with
#    "ld.exe: cannot find C:/Program". tools/no-manifest.specs overrides
#    *endfile to drop that one object; a console application does not need an
#    embedded application manifest. The alternative fix is to reinstall the
#    toolchain somewhere space-free such as C:\mingw64, after which this
#    work-around is simply unnecessary. The spec file must be given as an
#    absolute path — GCC does not resolve a relative one.
# 2. make runs recipe lines without a shell here (there is no sh.exe in the
#    winlibs distribution), so directories are created through cmd explicitly
#    instead of relying on cmd's builtins being reachable.
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
  SPECS  := -specs="$(CURDIR)/tools/no-manifest.specs"
  LDLIBS := -lwinhttp
  winpath = $(subst /,\,$(1))
  MKDIR   = cmd /c if not exist "$(call winpath,$(1))" mkdir "$(call winpath,$(1))"
  RMDIR   = cmd /c if exist "$(call winpath,$(1))" rmdir /S /Q "$(call winpath,$(1))"
else
  SPECS  :=
  LDLIBS :=
  MKDIR   = mkdir -p "$(1)"
  RMDIR   = rm -rf "$(1)"
endif

.PHONY: all run test clean

all: $(BIN)

$(BUILD)/%.o: src/%.c include/mb.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c include/mb.h | $(BUILD)/tests
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	$(CC) $(SPECS) $(OBJ) -o $@ $(LDLIBS)

$(TESTBIN): $(TESTOBJ)
	$(CC) $(SPECS) $(TESTOBJ) -o $@ $(LDLIBS)

$(BUILD):
	@$(call MKDIR,$(BUILD))

$(BUILD)/tests:
	@$(call MKDIR,$(BUILD)/tests)

run: $(BIN)
	$(BIN)

test: $(TESTBIN)
	$(TESTBIN)

clean:
	@$(call RMDIR,$(BUILD))
