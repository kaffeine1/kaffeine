# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Host build and checks of the kaffeine-jit68 engine, for an AArch64 host
# (macOS or Linux). It gives the options of the host run described in
# README.md, which also says what a run of this Makefile gave.

CC    ?= cc
BUILD ?= build

COMMON  = -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -g
# the host can run the code the translator writes: an AArch64 Mac or Linux
DEFS    = -DKJ68_TEST_SEAMS -DKJ68_PLAT_RUNS=1 -I host/include -I include -I engine
SAN     = -fsanitize=address,undefined -fno-sanitize-recover=undefined
ASANFL  = $(COMMON) -O1 -fno-omit-frame-pointer $(SAN) $(DEFS)
O2FL    = $(COMMON) -O2 $(DEFS)

HEADERS = include/m68kemu_engine.h engine/kj68_intern.h engine/kj68_platform.h host/include/exec/types.h
NAMES   = kj68_engine kj68_translate kj68_platform_host kj68_host_tests

vpath %.c engine host

.PHONY: all check clean

all: $(BUILD)/asan/kj68_host_tests $(BUILD)/o2/kj68_host_tests

$(BUILD)/asan/%.o: %.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASANFL) -c $< -o $@

$(BUILD)/o2/%.o: %.c $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(O2FL) -c $< -o $@

$(BUILD)/asan/kj68_host_tests: $(NAMES:%=$(BUILD)/asan/%.o)
	$(CC) $(SAN) -o $@ $^

$(BUILD)/o2/kj68_host_tests: $(NAMES:%=$(BUILD)/o2/%.o)
	$(CC) -o $@ $^

check: all
	ASAN_OPTIONS=halt_on_error=1:abort_on_error=0:detect_leaks=0 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(BUILD)/asan/kj68_host_tests
	$(BUILD)/o2/kj68_host_tests

clean:
	rm -rf $(BUILD)
