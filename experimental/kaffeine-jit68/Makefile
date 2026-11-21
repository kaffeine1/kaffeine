# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Host build and checks of the kaffeine-jit68 engine, for an AArch64 host
# (macOS or Linux). It gives the options of the host run described in
# README.md, which also says what a run of this Makefile gave.

CC    ?= cc
CXX   ?= c++
BUILD ?= build
# Moira, the interpreter the engine is compared with: a submodule of this repository
MOIRA ?= ../../rom/kaffeine/Moira

COMMON  = -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -g
# the host can run the code the translator writes: an AArch64 Mac or Linux
DEFS    = -DKJ68_TEST_SEAMS -DKJ68_PLAT_RUNS=1 -I host/include -I include -I engine
SAN     = -fsanitize=address,undefined -fno-sanitize-recover=undefined
ASANFL  = $(COMMON) -O1 -fno-omit-frame-pointer $(SAN) $(DEFS)
O2FL    = $(COMMON) -O2 $(DEFS)
# the comparison with Moira is C++, as Moira is; Moira's own warnings are not this directory's
CXXFL   = -std=c++20 -O2 -g -w $(DEFS) -I $(MOIRA)

HEADERS = include/m68kemu_engine.h engine/kj68_intern.h engine/kj68_platform.h host/include/exec/types.h
NAMES   = kj68_engine kj68_translate kj68_platform_host kj68_host_tests
ENGINE_NAMES = kj68_engine kj68_translate kj68_platform_host
# how many instructions "make check" compares with Moira
COMPARE ?= 2000000

# The translator keeps a temporary in x18, which is an ordinary caller-saved
# register in the AROS ABI. Darwin does not keep x18 across a context
# switch: what translated code leaves there is lost when its thread is
# preempted. So on a Mac these checks build the translator from a copy with
# that one temporary in x23, a register the trampolines save and the
# translator does not use. On Linux they build the file as it is.
ifeq ($(shell uname -s),Darwin)
TRANSLATE = $(BUILD)/darwin/kj68_translate.c
$(TRANSLATE): engine/kj68_translate.c
	@mkdir -p $(@D)
	sed -e 's/^#define R_SRC     18 /#define R_SRC     23 /' $< > $@
	grep -q '^#define R_SRC     23 ' $@
else
TRANSLATE = engine/kj68_translate.c
endif

vpath %.c engine host

.PHONY: all check clean

all: $(BUILD)/asan/kj68_host_tests $(BUILD)/o2/kj68_host_tests $(BUILD)/o2/kj68_moira_compare

$(BUILD)/asan/kj68_translate.o: $(TRANSLATE) $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASANFL) -c $< -o $@

$(BUILD)/o2/kj68_translate.o: $(TRANSLATE) $(HEADERS)
	@mkdir -p $(@D)
	$(CC) $(O2FL) -c $< -o $@

$(BUILD)/o2/kj68_moira_compare.o: host/kj68_moira_compare.cpp $(HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFL) -c $< -o $@

$(BUILD)/o2/kj68_moira_compare: $(ENGINE_NAMES:%=$(BUILD)/o2/%.o) $(BUILD)/o2/kj68_moira_compare.o
	$(CXX) -o $@ $^

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
	$(BUILD)/o2/kj68_moira_compare $(COMPARE)

clean:
	rm -rf $(BUILD)
