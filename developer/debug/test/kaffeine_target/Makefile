TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(TEST_DIR)/build

# m68k-amigaos-gcc with libnix (-noixemul), and vasm for the assembler test
M68K_PREFIX ?= m68k-amigaos-
M68K_CC ?= $(M68K_PREFIX)gcc
M68K_CFLAGS ?= -O2 -Wall -Wno-pointer-sign
M68K_LDFLAGS ?= -noixemul
VASM ?= vasmm68k_mot
PYTHON ?= python3

C_PROGRAMS := dos_fileio dos_strings dos_readargs dos_lvo dos_exall dos_packets exec_memory exec_public exec_lists exec_syslists exec_memheader exec_timer exec_console exec_diskfont exec_libraries exec_library_paths gfx_window gfx_idcmp gfx_request gfx_menus gfx_bevel gfx_gadgets gfx_gadget_attrs gfx_gadget_setattrs gfx_gadget_listview gfx_gadget_window gfx_gadget_links gfx_gadget_keys gfx_classic_menus gfx_pubscreen gfx_classic_bool gfx_classic_string gfx_classic_prop gfx_mouse gfx_workbench utility_tags utility_dates locale math_ieee math_ffp c_stdio c_compute cpu_faults hooks
PROGRAMS := hello_short hello_long $(C_PROGRAMS) c_stdio_vec c_compute_fpu
C_PROGRAMS += exec_messages exec_message_lifetime exec_packets gfx_planar_read icon_objects gfx_display_info
C_PROGRAMS += gfx_owned_screen gfx_layer_topology gfx_layer_resources
PROGRAMS += gfx_owned_screen gfx_layer_topology gfx_layer_resources
C_PROGRAMS += gfx_private_layers gfx_private_regions gfx_bitmap_transfer gfx_bitmap_guard gfx_drawinfo gfx_window_bitmap
PROGRAMS += gfx_private_layers gfx_private_regions gfx_bitmap_transfer gfx_bitmap_guard gfx_drawinfo gfx_window_bitmap
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

# cjpeg, djpeg and jpegtran of the IJG JPEG library 9f, from the source the
# AROS build fetches for its Ports: set JPEG_SRC to the unpacked jpeg-9f
# directory to build them. The sources are copied to $(JPEG_BUILD) with
# jpeg_jconfig.h as their jconfig.h, since the jconfig.h of the tree is the
# one of the AROS host and a quoted include finds it first. The battery runs
# the seven steps of the library's own "make test" and compares the outputs
# with its reference files.
JPEG_SRC ?=
JPEG_BUILD := $(BUILD_DIR)/jpeg-src
JPEG_LIB := jaricom jcomapi jutils jerror jmemmgr jmemnobs \
            jcapimin jcapistd jcarith jctrans jcparam jdatadst jcinit \
            jcmaster jcmarker jcmainct jcprepct jccoefct jccolor jcsample \
            jchuff jcdctmgr jfdctfst jfdctflt jfdctint \
            jdapimin jdapistd jdarith jdtrans jdatasrc jdmaster jdinput \
            jdmarker jdhuff jdmainct jdcoefct jdpostct jddctmgr jidctfst \
            jidctflt jidctint jdsample jdcolor jquant1 jquant2 jdmerge
JPEG_CJPEG := cjpeg rdppm rdgif rdtarga rdrle rdbmp rdswitch cdjpeg
JPEG_DJPEG := djpeg wrppm wrgif wrtarga wrrle wrbmp rdcolmap cdjpeg
JPEG_JPEGTRAN := jpegtran rdswitch cdjpeg transupp
jpeg_c = $(addprefix $(JPEG_BUILD)/,$(addsuffix .c,$(1)))
ifneq ($(JPEG_SRC),)
PROGRAMS += cjpeg djpeg jpegtran
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

$(BUILD_DIR)/bzip2: $(BZIP2_SOURCES) $(TEST_DIR)/bzip2_amiga.c $(TEST_DIR)/libgcc_ieee.c \
                   $(TEST_DIR)/libgcc_ieee_ports.c | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $^ -lm

$(JPEG_BUILD)/stamp: $(wildcard $(JPEG_SRC)/*.c $(JPEG_SRC)/*.h) $(TEST_DIR)/jpeg_jconfig.h | $(BUILD_DIR)
	rm -rf $(JPEG_BUILD)
	mkdir -p $(JPEG_BUILD)
	cp $(JPEG_SRC)/*.c $(JPEG_SRC)/*.h $(JPEG_BUILD)/
	cp $(TEST_DIR)/jpeg_jconfig.h $(JPEG_BUILD)/jconfig.h
	touch $@

JPEG_FLOAT := $(TEST_DIR)/libgcc_ieee.c $(TEST_DIR)/libgcc_ieee_ports.c
$(BUILD_DIR)/cjpeg: $(JPEG_BUILD)/stamp $(JPEG_FLOAT)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $(call jpeg_c,$(JPEG_CJPEG) $(JPEG_LIB)) $(JPEG_FLOAT) -lm

$(BUILD_DIR)/djpeg: $(JPEG_BUILD)/stamp $(JPEG_FLOAT)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $(call jpeg_c,$(JPEG_DJPEG) $(JPEG_LIB)) $(JPEG_FLOAT) -lm

$(BUILD_DIR)/jpegtran: $(JPEG_BUILD)/stamp $(JPEG_FLOAT)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $(call jpeg_c,$(JPEG_JPEGTRAN) $(JPEG_LIB)) $(JPEG_FLOAT) -lm

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

# bzip2 and the JPEG programs too, also when they were built with a
# BZIP2_SRC or JPEG_SRC that this make lacks
clean:
	rm -f $(PROGRAM_FILES) $(EXEC_LIBRARIES_LIBS) $(BUILD_DIR)/bzip2 $(BUILD_DIR)/exec_public_native
	rm -f $(BUILD_DIR)/cjpeg $(BUILD_DIR)/djpeg $(BUILD_DIR)/jpegtran
	rm -rf $(JPEG_BUILD)

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
