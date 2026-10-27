TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(TEST_DIR)/build

# m68k-amigaos-gcc with libnix (-noixemul), and vasm for the assembler test
M68K_PREFIX ?= m68k-amigaos-
M68K_CC ?= $(M68K_PREFIX)gcc
M68K_CFLAGS ?= -O2 -Wall -Wno-pointer-sign
M68K_LDFLAGS ?= -noixemul
VASM ?= vasmm68k_mot
PYTHON ?= python3

C_PROGRAMS := dos_fileio dos_strings dos_readargs dos_lvo exec_memory exec_public exec_lists exec_syslists exec_memheader exec_timer exec_console exec_diskfont exec_libraries exec_library_paths gfx_window gfx_idcmp gfx_request gfx_menus gfx_bevel gfx_gadgets gfx_gadget_attrs gfx_gadget_setattrs gfx_gadget_listview gfx_gadget_window gfx_gadget_links gfx_gadget_keys gfx_classic_menus gfx_pubscreen gfx_classic_bool gfx_classic_string gfx_classic_prop gfx_mouse gfx_workbench utility_tags utility_dates locale math_ieee math_ffp c_stdio c_compute cpu_faults hooks
PROGRAMS := hello_short hello_long $(C_PROGRAMS) c_stdio_vec c_compute_fpu
C_PROGRAMS += exec_messages exec_message_lifetime exec_packets gfx_planar_read icon_objects gfx_display_info
C_PROGRAMS += gfx_owned_screen
PROGRAMS += gfx_owned_screen
C_PROGRAMS += exec_guest_tasks exec_guest_task_window exec_guest_messages
PROGRAMS += exec_guest_tasks exec_guest_task_window exec_guest_messages
PROGRAMS += exec_messages exec_message_lifetime exec_packets gfx_planar_read icon_objects gfx_display_info
EXEC_LIBRARIES_DIR := $(BUILD_DIR)/exec_libraries-files
EXEC_LIBRARIES_LIBS := $(EXEC_LIBRARIES_DIR)/emu68test.library \
                       $(EXEC_LIBRARIES_DIR)/emu68test_l.library

# bzip2 1.0.8, from the source the AROS build fetches for its Ports: set
# BZIP2_SRC to the unpacked bzip2-1.0.8 directory to build it. The battery
# runs the six steps of its own self-test and compares the outputs with
# the reference files of the distribution.
BZIP2_SRC ?=
BZIP2_SOURCES := $(addprefix $(BZIP2_SRC)/,blocksort.c huffman.c crctable.c randtable.c \
                   compress.c decompress.c bzlib.c bzip2.c)
ifneq ($(BZIP2_SRC),)
PROGRAMS += bzip2
endif
PROGRAM_FILES := $(addprefix $(BUILD_DIR)/,$(PROGRAMS))

all: $(PROGRAM_FILES) $(EXEC_LIBRARIES_LIBS)

$(BUILD_DIR) $(EXEC_LIBRARIES_DIR):
	mkdir -p $@

# vasm writes the 16-bit relocation format as HUNK_DREL32 by default and
# HUNK_RELOC32 with -kick1hunks.
$(BUILD_DIR)/hello_short: $(TEST_DIR)/hello_seglist.s | $(BUILD_DIR)
	$(VASM) -quiet -Fhunkexe -nosym -o $@ $<

$(BUILD_DIR)/hello_long: $(TEST_DIR)/hello_seglist.s | $(BUILD_DIR)
	$(VASM) -quiet -Fhunkexe -nosym -kick1hunks -o $@ $<

$(addprefix $(BUILD_DIR)/,$(C_PROGRAMS)): $(BUILD_DIR)/%: $(TEST_DIR)/%.c $(TEST_DIR)/m68ktest.h | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $< $(EXTRA_LINK)

# Original bitmap font for the explicit-PROGDIR diskfont regression.
# Order-only dependency keeps the C source as the static pattern rule's $<.
$(BUILD_DIR)/exec_diskfont: | $(BUILD_DIR)/exec_diskfont-fixture.stamp
$(BUILD_DIR)/exec_diskfont-fixture.stamp: $(TEST_DIR)/make-bitmap-font.py | $(BUILD_DIR)
	$(PYTHON) $< $(BUILD_DIR)/exec_diskfont-files
	touch $@

# Original byte fixture for the valid read-only DOS packet regression.
$(BUILD_DIR)/exec_packets: | $(BUILD_DIR)/exec_packets-files/putmsg-read.bin
$(BUILD_DIR)/exec_packets-files/putmsg-read.bin: $(TEST_DIR)/exec_packets-files/putmsg-read.bin | $(BUILD_DIR)
	mkdir -p $(@D)
	cp $< $@

# Optional native companion, built separately by the AROS target reviewer.
# It must run as native code, so never use M68K_CC for this target.
NATIVE_CC ?= aarch64-aros-gcc
$(BUILD_DIR)/exec_public_native: $(TEST_DIR)/exec_public.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -DKAFFEINE_NATIVE_PORT_FIXTURE -o $@ $<

# The floating point printf() of libnix is in libm.a. It calls __fixdfsi()
# and __floatsidf(), which the empty libgcc of the toolchain lacks.
STDIO_FLOAT := $(TEST_DIR)/libgcc_ieee.c -lm
$(BUILD_DIR)/c_stdio: $(TEST_DIR)/libgcc_ieee.c
$(BUILD_DIR)/c_stdio: EXTRA_LINK = $(STDIO_FLOAT)

# c_stdio with its own malloc() family on AllocVec() and FreeVec()
$(BUILD_DIR)/c_stdio_vec: $(TEST_DIR)/c_stdio.c $(TEST_DIR)/libgcc_ieee.c | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) -DMALLOC_ON_ALLOCVEC $(M68K_LDFLAGS) -o $@ $< $(STDIO_FLOAT)

$(BUILD_DIR)/bzip2: $(BZIP2_SOURCES) $(TEST_DIR)/bzip2_amiga.c $(TEST_DIR)/libgcc_ieee.c | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $^ -lm

# c_compute for the 68020 with 68881 instructions; fsqrt without the libm
# fallback that sets errno
$(BUILD_DIR)/c_compute_fpu: $(TEST_DIR)/c_compute.c $(TEST_DIR)/m68ktest.h | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) -m68020 -m68881 -fno-math-errno $(M68K_LDFLAGS) -o $@ $<

# 68k test libraries for exec_libraries, installed next to it with
# --files exec_libraries $(BUILD_DIR)/exec_libraries-files
$(BUILD_DIR)/exec_libraries: $(EXEC_LIBRARIES_LIBS)

$(EXEC_LIBRARIES_DIR)/emu68test.library: $(TEST_DIR)/emu68test_lib.c | $(EXEC_LIBRARIES_DIR)
	$(M68K_CC) $(M68K_CFLAGS) -fno-toplevel-reorder -nostartfiles -nostdlib $(M68K_LDFLAGS) -o $@ $<

$(EXEC_LIBRARIES_DIR)/emu68test_l.library: $(TEST_DIR)/emu68test_lib.c | $(EXEC_LIBRARIES_DIR)
	$(M68K_CC) $(M68K_CFLAGS) -fno-toplevel-reorder -DLONG_TABLE -nostartfiles -nostdlib $(M68K_LDFLAGS) -o $@ $<

# bzip2 too, also when it was built with a BZIP2_SRC that this make lacks
clean:
	rm -f $(PROGRAM_FILES) $(EXEC_LIBRARIES_LIBS) $(BUILD_DIR)/bzip2 $(BUILD_DIR)/exec_public_native

# Offline result/parser tests: no cross compiler, mtools or QEMU needed.
check-runner:
	$(PYTHON) -m unittest discover -s $(TEST_DIR) -p 'test_run_raspi_qemu.py' -v

.PHONY: all clean check-runner

# Native AArch64 framebuffer check: never build this with M68K_CC.
$(BUILD_DIR)/gfx_framebuffer_palette_native: $(TEST_DIR)/gfx_framebuffer_palette_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Werror -o $@ $<

# Native observer after the owned-screen leave probe, in the same boot.
$(BUILD_DIR)/gfx_screen_cleanup_native: $(TEST_DIR)/gfx_screen_cleanup_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Werror -o $@ $<
