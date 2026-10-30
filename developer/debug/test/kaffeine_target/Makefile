TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= $(TEST_DIR)/build

# m68k-amigaos-gcc with libnix (-noixemul), and vasm for the assembler test
M68K_PREFIX ?= m68k-amigaos-
M68K_CC ?= $(M68K_PREFIX)gcc
# Every 68k program is built without peephole2: in m68k-amigaos-gcc 6.5.0b
# that pass turns the clearing of an unsigned short and of the int right
# after it into one clr.l of four bytes, and the low half of the int keeps
# what was in memory ("p->a = 0; p->b = 0;" on
# "struct s { char p[6]; unsigned short a; int b; }" gives clr.l (6,a0)).
M68K_CFLAGS ?= -O2 -fno-peephole2 -Wall -Wno-pointer-sign
M68K_LDFLAGS ?= -noixemul
VASM ?= vasmm68k_mot
PYTHON ?= python3

C_PROGRAMS := cpu_full_index exec_io_signal_budget dos_packet_zero dos_locks exec_timer_children exec_timer_stack gfx_gels_init gfx_screen_pens gfx_rastport_state gfx_classic_rastport gfx_copied_rastport intui_classic_draw exec_keyboard exec_timer_async exec_createproc gfx_askfont exec_process_stack gfx_window_clip gfx_classic_request gfx_requester_init gfx_intuitext_width gfx_blit_clip file_packet_reply dos_fileio dos_strings dos_readargs dos_lvo dos_exall dos_packets dos_handlers exec_memory exec_public exec_lists exec_syslists exec_memheader exec_timer exec_console exec_diskfont exec_libraries exec_library_paths gfx_window gfx_idcmp gfx_request gfx_menus gfx_bevel gfx_gadgets gfx_gadget_attrs gfx_gadget_setattrs gfx_gadget_listview gfx_gadget_window gfx_gadget_links gfx_gadget_keys gfx_classic_menus gfx_pubscreen gfx_classic_bool gfx_classic_string gfx_classic_prop gfx_mouse gfx_workbench utility_tags utility_dates locale math_ieee math_ffp c_stdio c_compute cpu_faults hooks
PROGRAMS := hello_short hello_long $(C_PROGRAMS) c_stdio_vec c_compute_fpu
C_PROGRAMS += exec_messages exec_message_lifetime exec_packets gfx_planar_read icon_objects gfx_display_info
C_PROGRAMS += gfx_owned_screen gfx_layer_topology gfx_layer_resources
PROGRAMS += gfx_owned_screen gfx_layer_topology gfx_layer_resources
C_PROGRAMS += gfx_private_layers gfx_private_regions gfx_bitmap_transfer gfx_bitmap_guard gfx_drawinfo gfx_window_bitmap gfx_bitmap_init
PROGRAMS += gfx_private_layers gfx_private_regions gfx_bitmap_transfer gfx_bitmap_guard gfx_drawinfo gfx_window_bitmap gfx_bitmap_init
C_PROGRAMS += gfx_appicon_icon gfx_screen_palette gfx_layerinfo_mode
PROGRAMS += gfx_appicon_icon gfx_screen_palette gfx_layerinfo_mode
C_PROGRAMS += gfx_window_defaults
PROGRAMS += gfx_window_defaults
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

# minigzip and zpipe of zlib 1.3.1, from the source the AROS build fetches
# for its Ports (the zlib of Chromium): set ZLIB_SRC to its zlib directory
# to build them. They leave out the SIMD paths of Chromium (CPU_NO_SIMD),
# take the zconf.h.in of the distribution as zconf.h and define AMIGA, so
# that a gzip header names the Amiga as its system. They keep the hash of
# canonical zlib (USE_ZLIB_RABIN_KARP_ROLLING_HASH), so their output is the
# one of canonical zlib; the hash of Chromium reads four bytes as a word and
# is validated for little endian processors only.
# Deflate shows the peephole2 bug above in "s->bi_buf = 0; s->bi_valid = 0;":
# malloc() memory is not cleared on AmigaOS, so with peephole2 every
# compressed stream started or ended with stray bits. The zlib programs keep
# -fno-peephole2 also when M68K_CFLAGS is given by hand.
# The battery compresses and decompresses zlib.h with them and compares the
# outputs with the references that "make zlib-refs" writes to $(ZLIB_REFS).
ZLIB_SRC ?=
ZLIB_BUILD := $(BUILD_DIR)/zlib-src
ZLIB_REFS := $(BUILD_DIR)/zlib-refs
ZLIB_LIB := adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback \
            inffast inflate inftrees trees uncompr zutil cpu_features
ZLIB_SOURCES := $(addprefix $(ZLIB_SRC)/,$(addsuffix .c,$(ZLIB_LIB)))
ZLIB_FLAGS := -DCPU_NO_SIMD -DUSE_ZLIB_RABIN_KARP_ROLLING_HASH -DHAVE_UNISTD_H \
              -I$(ZLIB_BUILD) -I$(ZLIB_SRC)
ZLIB_M68K_FLAGS := -DAMIGA -fno-peephole2
ZLIB_GZ_MODES := 1 6 9 f h r
ifneq ($(ZLIB_SRC),)
PROGRAMS += minigzip zpipe
endif

# pngtest of libpng 1.6.58, from the source the AROS build fetches for its
# Ports, on the zlib above: set PNG_SRC to its libpng directory, with
# ZLIB_SRC, to build it. Its pnglibconf.h is the pnglibconf.h.prebuilt of
# the distribution. The battery runs it through the steps of the
# tests/pngtest-all of libpng, on the images that $(PNG_FILES) collects,
# and compares its output with the one of the same program built for the
# host, which "make png-refs" writes to $(PNG_REFS). The images are under
# EMU68/, the drawer of the program in SYS: of the guest, which is the
# current directory there.
PNG_SRC ?=
PNG_BUILD := $(BUILD_DIR)/png-src
PNG_FILES := $(BUILD_DIR)/pngtest-files
PNG_REFS := $(BUILD_DIR)/png-refs
PNG_LIB := png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil \
           pngset pngtrans pngwio pngwrite pngwtran pngwutil
PNG_SOURCES := $(addprefix $(PNG_SRC)/,$(addsuffix .c,$(PNG_LIB)))
PNG_FLAGS := -I$(PNG_BUILD) -I$(PNG_SRC)
# libpng turns on its SIMD paths by itself on some hosts
PNG_HOST_FLAGS := -DPNG_ARM_NEON_OPT=0 -DPNG_INTEL_SSE_OPT=0 -DPNG_MIPS_MSA_OPT=0 \
                  -DPNG_MIPS_MMI_OPT=0 -DPNG_POWERPC_VSX_OPT=0 \
                  -DPNG_LOONGARCH_LSX_OPT=0 -DPNG_RISCV_RVV_OPT=0
PNG_STRICT := EMU68/pngtest.png EMU68/png-3/cicp-display-p3_reencoded.png
PNG_RELAXED := EMU68/crashers/badcrc.png
# The Shell takes command lines of up to 512 bytes, so the 22 images that
# must fail go in three steps, in the order of tests/pngtest-all.
png_crashers = $(patsubst %,EMU68/crashers/%.png,$(1))
PNG_XFAIL1 := $(call png_crashers,badadler bad_iCCP empty_ancillary_chunks \
              huge_bKGD_chunk huge_cHRM_chunk huge_eXIf_chunk huge_gAMA_chunk)
PNG_XFAIL2 := $(call png_crashers,huge_hIST_chunk huge_iCCP_chunk huge_iTXt_chunk \
              huge_pCAL_chunk huge_pHYs_chunk huge_sCAL_chunk huge_sPLT_chunk \
              huge_sRGB_chunk)
PNG_XFAIL3 := $(call png_crashers,huge_sTER_chunk huge_tEXt_chunk huge_tIME_chunk \
              huge_zTXt_chunk huge_juNK_unsafe_to_copy huge_juNk_safe_to_copy \
              huge_IDAT)
PNG_BADPAL := $(patsubst %,EMU68/badpal/%.png,regression-palette-8 small-palette-1 \
              small-palette-2 small-palette-4 small-palette-8 test-palette-1 \
              test-palette-2 test-palette-4 test-palette-8)
ifneq ($(PNG_SRC),)
ifneq ($(ZLIB_SRC),)
PROGRAMS += pngtest
endif
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
# This payload needs the native parent and is not a standalone battery case.
$(BUILD_DIR)/hunk_process_child: $(TEST_DIR)/hunk_process_child.c | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $<

$(BUILD_DIR)/hunk_process_native: $(TEST_DIR)/hunk_process_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Wextra -Werror -Wno-pointer-sign -o $@ $<

# The native controller emits its original 68k HUNKs itself.
$(BUILD_DIR)/domain_fixture_native: $(TEST_DIR)/domain_fixture_native.c $(TEST_DIR)/domain_fixture_hunks.c $(TEST_DIR)/domain_fixture_hunks.h | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Wextra -Werror -Wno-pointer-sign -o $@ $(TEST_DIR)/domain_fixture_native.c $(TEST_DIR)/domain_fixture_hunks.c

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

$(ZLIB_BUILD)/zconf.h: $(ZLIB_SRC)/zconf.h.in
	mkdir -p $(@D)
	cp $< $@

$(BUILD_DIR)/minigzip: $(ZLIB_SRC)/test/minigzip.c $(ZLIB_SOURCES) $(ZLIB_BUILD)/zconf.h | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) $(ZLIB_M68K_FLAGS) $(ZLIB_FLAGS) $(M68K_LDFLAGS) -o $@ $< $(ZLIB_SOURCES)

$(BUILD_DIR)/zpipe: $(ZLIB_SRC)/examples/zpipe.c $(ZLIB_SOURCES) $(ZLIB_BUILD)/zconf.h | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) $(ZLIB_M68K_FLAGS) $(ZLIB_FLAGS) $(M68K_LDFLAGS) -o $@ $< $(ZLIB_SOURCES)

# The zlib references: zlib.h through the same two programs built for the
# host from the same sources, without AMIGA. The tenth byte of a gzip
# header names the system that wrote it, so the references get the 1 of
# the Amiga there, as the m68k build writes.
HOST_CC ?= cc
$(ZLIB_REFS)/stamp: $(ZLIB_SRC)/test/minigzip.c $(ZLIB_SRC)/examples/zpipe.c $(ZLIB_SOURCES) \
                    $(ZLIB_BUILD)/zconf.h
	mkdir -p $(@D)
	$(HOST_CC) -O2 $(ZLIB_FLAGS) -o $(@D)/minigzip $(ZLIB_SRC)/test/minigzip.c $(ZLIB_SOURCES)
	$(HOST_CC) -O2 $(ZLIB_FLAGS) -o $(@D)/zpipe $(ZLIB_SRC)/examples/zpipe.c $(ZLIB_SOURCES)
	for m in $(ZLIB_GZ_MODES); do \
	    $(@D)/minigzip -$$m < $(ZLIB_SRC)/zlib.h > $(@D)/zlib.h-$$m.gz || exit 1; \
	    printf '\001' | dd of=$(@D)/zlib.h-$$m.gz bs=1 seek=9 count=1 conv=notrunc 2>/dev/null || exit 1; \
	done
	$(@D)/zpipe < $(ZLIB_SRC)/zlib.h > $(@D)/zlib.h.zz
	touch $@

zlib-refs: $(ZLIB_REFS)/stamp

$(PNG_BUILD)/pnglibconf.h: $(PNG_SRC)/scripts/pnglibconf.h.prebuilt
	mkdir -p $(@D)
	cp $< $@

# The images of the steps, installed with --files next to each step
$(PNG_FILES).stamp: $(PNG_SRC)/pngtest.png | $(BUILD_DIR)
	rm -rf $(PNG_FILES)
	mkdir -p $(PNG_FILES)
	cp $(PNG_SRC)/pngtest.png $(PNG_FILES)/
	cp -R $(addprefix $(PNG_SRC)/contrib/testpngs/,png-3 crashers badpal) $(PNG_FILES)/
	touch $@

$(BUILD_DIR)/pngtest: $(PNG_SRC)/pngtest.c $(PNG_SOURCES) $(ZLIB_SOURCES) $(PNG_BUILD)/pnglibconf.h \
                      $(ZLIB_BUILD)/zconf.h | $(BUILD_DIR) $(PNG_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) $(ZLIB_M68K_FLAGS) $(ZLIB_FLAGS) $(PNG_FLAGS) $(M68K_LDFLAGS) \
	    -o $@ $< $(PNG_SOURCES) $(ZLIB_SOURCES) -lm

# The pngtest references: the outputs of the six steps through pngtest
# built for the host, run where EMU68 names $(PNG_FILES). The badpal step
# ends with 1, as tests/pngtest-all expects of those images.
$(PNG_REFS)/stamp: $(PNG_SRC)/pngtest.c $(PNG_SOURCES) $(ZLIB_SOURCES) $(PNG_BUILD)/pnglibconf.h \
                   $(ZLIB_BUILD)/zconf.h $(PNG_FILES).stamp
	mkdir -p $(@D)
	$(HOST_CC) -O2 $(ZLIB_FLAGS) $(PNG_FLAGS) $(PNG_HOST_FLAGS) -o $(@D)/pngtest \
	    $(PNG_SRC)/pngtest.c $(PNG_SOURCES) $(ZLIB_SOURCES)
	rm -f $(@D)/EMU68
	ln -s $(PNG_FILES) $(@D)/EMU68
	cd $(@D) && ./pngtest --strict $(PNG_STRICT) > png_strict.out
	cd $(@D) && ./pngtest --relaxed $(PNG_RELAXED) > png_relaxed.out
	cd $(@D) && ./pngtest --xfail $(PNG_XFAIL1) > png_xfail1.out 2> png_xfail1.err
	cd $(@D) && ./pngtest --xfail $(PNG_XFAIL2) > png_xfail2.out 2> png_xfail2.err
	cd $(@D) && ./pngtest --xfail $(PNG_XFAIL3) > png_xfail3.out 2> png_xfail3.err
	cd $(@D) && { ./pngtest -m $(PNG_BADPAL) > png_badpal.out 2> png_badpal.err; test $$? = 1; }
	rm -f $(@D)/pngout.png
	touch $@

png-refs: $(PNG_REFS)/stamp

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

# bzip2, the JPEG programs, the zlib ones and pngtest too, also when they
# were built with a BZIP2_SRC, JPEG_SRC, ZLIB_SRC or PNG_SRC that this make
# lacks
clean:
	rm -f $(PROGRAM_FILES) $(EXEC_LIBRARIES_LIBS) $(BUILD_DIR)/bzip2 $(BUILD_DIR)/exec_public_native
	rm -f $(BUILD_DIR)/cjpeg $(BUILD_DIR)/djpeg $(BUILD_DIR)/jpegtran
	rm -f $(BUILD_DIR)/minigzip $(BUILD_DIR)/zpipe $(BUILD_DIR)/pngtest $(PNG_FILES).stamp
	rm -rf $(JPEG_BUILD) $(ZLIB_BUILD) $(ZLIB_REFS) $(PNG_BUILD) $(PNG_FILES) $(PNG_REFS)

# Offline result/parser tests: no cross compiler, mtools or QEMU needed.
check-runner:
	$(PYTHON) -m unittest discover -s $(TEST_DIR) -p 'test_run_raspi_qemu.py' -v

.PHONY: all clean check-runner zlib-refs png-refs

# Native AArch64 framebuffer check: never build this with M68K_CC.
$(BUILD_DIR)/gfx_framebuffer_palette_native: $(TEST_DIR)/gfx_framebuffer_palette_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Werror -o $@ $<

# Native observer after the owned-screen leave probe, in the same boot.
$(BUILD_DIR)/gfx_screen_cleanup_native: $(TEST_DIR)/gfx_screen_cleanup_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Werror -o $@ $<

# The bytecode probe intentionally uses 68020 addressing and MOVE from CCR.
$(BUILD_DIR)/cpu_full_index: M68K_CFLAGS += -m68020
