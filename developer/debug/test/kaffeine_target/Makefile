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

C_PROGRAMS := cpu_full_index exec_io_signal_budget dos_packet_zero dos_locks exec_timer_children exec_timer_stack gfx_gels_init gfx_screen_pens gfx_rastport_state gfx_classic_rastport gfx_copied_rastport intui_classic_draw exec_keyboard exec_timer_async exec_createproc gfx_askfont exec_process_stack gfx_window_clip gfx_classic_request gfx_requester_init gfx_intuitext_width gfx_blit_clip file_packet_reply dos_fileio dos_strings dos_readargs dos_lvo dos_exall dos_packets dos_handlers exec_memory exec_public exec_lists exec_syslists exec_memheader exec_timer exec_console exec_diskfont exec_libraries exec_library_paths gfx_window gfx_idcmp gfx_request gfx_menus gfx_bevel gfx_gadgets gfx_gadget_attrs gfx_gadget_setattrs gfx_gadget_listview gfx_gadget_window gfx_gadget_links gfx_gadget_keys gfx_classic_menus gfx_pubscreen gfx_classic_bool gfx_classic_string gfx_classic_prop gfx_mouse gfx_workbench utility_tags utility_dates locale math_ieee math_ffp c_stdio c_compute c_time cpu_faults hooks
PROGRAMS := hello_short hello_long $(C_PROGRAMS) c_stdio_vec c_compute_fpu
C_PROGRAMS += cpu_dbcc
PROGRAMS += cpu_dbcc
C_PROGRAMS += exec_messages exec_message_lifetime exec_packets gfx_planar_read icon_objects gfx_display_info
C_PROGRAMS += gfx_owned_screen gfx_layer_topology gfx_layer_resources
PROGRAMS += gfx_owned_screen gfx_layer_topology gfx_layer_resources
C_PROGRAMS += gfx_private_layers gfx_private_regions gfx_bitmap_transfer gfx_bitmap_guard gfx_drawinfo gfx_window_bitmap gfx_bitmap_init
PROGRAMS += gfx_private_layers gfx_private_regions gfx_bitmap_transfer gfx_bitmap_guard gfx_drawinfo gfx_window_bitmap gfx_bitmap_init
C_PROGRAMS += gfx_appicon_icon gfx_screen_palette gfx_layerinfo_mode
PROGRAMS += gfx_appicon_icon gfx_screen_palette gfx_layerinfo_mode
C_PROGRAMS += gfx_window_defaults
PROGRAMS += gfx_window_defaults
C_PROGRAMS += gfx_p96
PROGRAMS += gfx_p96
C_PROGRAMS += exec_guest_tasks exec_guest_task_window exec_guest_messages
PROGRAMS += exec_guest_tasks exec_guest_task_window exec_guest_messages
PROGRAMS += exec_messages exec_message_lifetime exec_packets gfx_planar_read icon_objects gfx_display_info
EXEC_LIBRARIES_DIR := $(BUILD_DIR)/exec_libraries-files
EXEC_LIBRARIES_LIBS := $(EXEC_LIBRARIES_DIR)/emu68test.library \
                       $(EXEC_LIBRARIES_DIR)/emu68test_l.library

# bzip2 1.0.8, from the source the AROS build fetches for its Ports: set
# BZIP2_SRC to the unpacked bzip2-1.0.8 directory to build it, with
# bzip2_amiga.c for fchmod() and fchown(). The battery runs it through the
# six steps of its own self-test, -9, -s, and three bad inputs on the files
# that $(BZIP2_FILES) collects under EMU68/, and compares its output with
# the one of the same program built for the host, which "make bzip2-refs"
# writes to $(BZIP2_REFS).
BZIP2_SRC ?=
BZIP2_FILES := $(BUILD_DIR)/bzip2-files
BZIP2_REFS := $(BUILD_DIR)/bzip2-refs
BZIP2_SOURCES := $(addprefix $(BZIP2_SRC)/,blocksort.c huffman.c crctable.c randtable.c \
                   compress.c decompress.c bzlib.c bzip2.c)
BZIP2_STEPS := c1 c2 c3 c9 cs d1 d2 d3 bad_crc bad_cut bad_magic
BZIP2_ARGS_c1 := -c -1 EMU68/sample1.ref
BZIP2_ARGS_c2 := -c -2 EMU68/sample2.ref
BZIP2_ARGS_c3 := -c -3 EMU68/sample3.ref
BZIP2_ARGS_c9 := -c -9 EMU68/sample2.ref
BZIP2_ARGS_cs := -c -s EMU68/sample2.ref
BZIP2_ARGS_d1 := -dc EMU68/sample1.bz2
BZIP2_ARGS_d2 := -dc EMU68/sample2.bz2
BZIP2_ARGS_d3 := -dcs EMU68/sample3.bz2
BZIP2_ARGS_bad_crc := -dc EMU68/sample1.bz2 EMU68/bad-crc.bz2
BZIP2_ARGS_bad_cut := -dcq EMU68/sample1.bz2 EMU68/bad-cut.bz2
BZIP2_ARGS_bad_magic := -dc EMU68/sample1.bz2 EMU68/sample1.ref
$(foreach s,bad_crc bad_cut bad_magic,$(eval BZIP2_RC_$(s) := 2))
ifneq ($(BZIP2_SRC),)
PROGRAMS += bzip2
endif

# zstd of Zstandard 1.5.7, from the source the AROS build fetches for its
# Ports: set ZSTD_SRC to its zstd-1.5.7 directory to build it, without
# threads, legacy formats, the benchmark or the dictionary builder. The
# battery runs it on the files that $(ZSTD_FILES) collects under EMU68/, and
# compares its output with the one of the same program built for the host,
# which "make zstd-refs" writes to $(ZSTD_REFS).
ZSTD_SRC ?=
ZSTD_FILES := $(BUILD_DIR)/zstd-files
ZSTD_REFS := $(BUILD_DIR)/zstd-refs
ZSTD_SOURCES := $(wildcard $(patsubst %,$(ZSTD_SRC)/lib/%/*.c,common compress decompress)) \
                $(addprefix $(ZSTD_SRC)/programs/,zstdcli.c util.c timefn.c fileio.c fileio_asyncio.c)
ZSTD_FLAGS := -DXXH_NAMESPACE=ZSTD_ -DZSTD_DISABLE_ASM -DZSTD_NO_INTRINSICS \
              -DZSTD_NOBENCH -DZSTD_NODICT -DZSTD_NOTRACE -DZSTD_LEGACY_SUPPORT=0 \
              -I$(ZSTD_SRC)/lib -I$(ZSTD_SRC)/lib/common
ZSTD_GOOD := $(addprefix EMU68/,block-128k.zst empty-block.zst rle-first-block.zst zeroSeq_2B.zst)
ZSTD_BAD := $(addprefix EMU68/,off0.bin.zst truncated_huff_state.zst zeroSeq_extraneous.zst)
ZSTD_INPUTS := $(addprefix EMU68/,PR-3517-block-splitter-corruption-test http \
               huffman-compressed-larger large-literal-and-match-lengths)
ZSTD_PACK := c1 c3 c9 c19
ZSTD_STEPS := good bad list $(ZSTD_PACK) unpack
ZSTD_ARGS_good := -dc $(ZSTD_GOOD)
ZSTD_ARGS_bad := -dc $(ZSTD_BAD) EMU68/zeroSeq_2B.zst
ZSTD_RC_bad := 1
ZSTD_ARGS_list := -l $(ZSTD_GOOD)
$(foreach l,1 3 9 19,$(eval ZSTD_ARGS_c$(l) := -c -$(l) $(ZSTD_INPUTS)))
ZSTD_ARGS_unpack := -dc
ifneq ($(ZSTD_SRC),)
PROGRAMS += zstd
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
PNG_RC_badpal := 1
ifneq ($(PNG_SRC),)
ifneq ($(ZLIB_SRC),)
PROGRAMS += pngtest
endif
endif

# xmlwf of expat 2.8.2, from the source the AROS build fetches for its
# Ports: set EXPAT_SRC to its expat directory to build it. xmlwf.c is built
# with xmlwf_stdout.h in front, so that "-d -" writes what xmlwf parses to
# stdout. The battery runs it through seven steps on XML documents of this
# tree, which $(XMLWF_FILES) collects under EMU68/ as $(PNG_FILES) does, and
# compares its output with the one of the same program built for the host,
# which "make xmlwf-refs" writes to $(XMLWF_REFS).
EXPAT_SRC ?=
EXPAT_BUILD := $(BUILD_DIR)/expat-src
XMLWF_FILES := $(BUILD_DIR)/xmlwf-files
XMLWF_REFS := $(BUILD_DIR)/xmlwf-refs
EXPAT_SOURCES := $(addprefix $(EXPAT_SRC)/,lib/xmlparse.c lib/xmlrole.c lib/xmltok.c \
                 xmlwf/xmlfile.c xmlwf/codepage.c xmlwf/readfilemap.c)
EXPAT_FLAGS := -I$(EXPAT_BUILD) -I$(EXPAT_SRC)/lib -include $(TEST_DIR)/xmlwf_stdout.h
TREE_DIR := $(abspath $(TEST_DIR)/../../../..)
XMLWF_DOCS := $(addprefix $(TREE_DIR)/,arch/all-pc/udis86/docs/x86/optable.xml \
              arch/all-pc/udis86/docs/x86/avx.xml \
              workbench/classes/zune/betterstring/include/xml/muimaster.xml \
              images/IconSets/Gorilla/Icons/Scalable/AROS/Prefs/Locale.svg \
              workbench/network/WirelessManager/wpa_supplicant/wpa_gui-qt4/icons/laptop.svg \
              arch/all-android/bootstrap/app/AndroidManifest.xml)
# The arguments of the steps. xmlwf_stdin reads AndroidManifest.xml from its
# input, and xmlwf_errors ends with 2, as xmlwf does when a document is not
# well formed.
XMLWF_VERSION := -v
XMLWF_CANON := -d - EMU68/optable.xml
XMLWF_NS := -n -d - EMU68/laptop.svg
XMLWF_META := -m -d - EMU68/muimaster.xml
XMLWF_COPY := -c -d - EMU68/Locale.svg
XMLWF_STDIN := -d -
XMLWF_ERRORS := -k -s EMU68/avx.xml EMU68/Locale.svg EMU68/AndroidManifest.xml
XMLWF_RC_errors := 2
ifneq ($(EXPAT_SRC),)
PROGRAMS += xmlwf
endif

# xz of XZ Utils 5.8.3, from the source the AROS build fetches for its
# Ports: set XZ_SRC to its xz directory to build it, with xz_config.h as its
# config.h and, for the m68k, xz_libnix.c for the POSIX calls that libnix
# lacks. The battery runs it through the steps of tests/test_files.sh and
# tests/test_compress.sh of XZ Utils, on the files that $(XZ_FILES)
# collects under EMU68/, and compares its output with the one of the same
# program built for the host, which "make xz-refs" writes to $(XZ_REFS).
XZ_SRC ?=
XZ_BUILD := $(BUILD_DIR)/xz-src
XZ_FILES := $(BUILD_DIR)/xz-files
XZ_REFS := $(BUILD_DIR)/xz-refs
XZ_LIBLZMA := $(filter-out %_small.c %_tablegen.c %/crc_clmul_consts_gen.c %/outqueue.c %_mt.c, \
              $(wildcard $(patsubst %,$(XZ_SRC)/src/liblzma/%/*.c,check common delta lz lzma \
              rangecoder simple)))
XZ_SOURCES := $(XZ_LIBLZMA) $(wildcard $(XZ_SRC)/src/xz/*.c) \
              $(patsubst %,$(XZ_SRC)/src/common/tuklib_%.c,cpucores physmem exit mbstr_fw \
              mbstr_nonprint mbstr_width mbstr_wrap open_stdxxx progname)
XZ_FLAGS := -DHAVE_CONFIG_H -I$(XZ_BUILD) -I$(XZ_SRC)/src/common -I$(XZ_SRC)/src/liblzma/api \
            $(patsubst %,-I$(XZ_SRC)/src/liblzma/%,common check lz rangecoder lzma delta simple)
# The steps. The good files must decompress and the bad and unsupported ones
# must fail, as in tests/test_files.sh; the 512 bytes of a command line take
# eleven of their names or fewer. Each bad step ends with a good file, which
# xz decompresses after the errors of the others, so that no step is without
# output. The steps of tests/test_compress.sh compress the three files that
# create_compress_files writes, with the presets -1 and -2 and with each
# filter before LZMA2, and xz_unpack decompresses the references of all
# thirteen from its input. -1 and -2 take 9 and 17 MiB; -3 takes 32 MiB, all
# the memory that kaffeine gives the guest.
xz_files = $(patsubst $(XZ_SRC)/tests/files/%,EMU68/%,$(sort $(wildcard $(XZ_SRC)/tests/files/$(1))))
XZ_GOOD := $(call xz_files,good-*)
XZ_BAD := $(call xz_files,bad-*) $(call xz_files,unsupported-*)
XZ_MEM := --memlimit-compress=48MiB --memlimit-decompress=5MiB --no-adjust --threads=1
XZ_HELLO := EMU68/good-1-check-crc32.xz
XZ_INPUTS := $(patsubst %,EMU68/compress_generated_%,abc random text)
XZ_LZMA2 := lzma2:dict=64KiB,nice=32,mode=fast
XZ_BCJ := x86 powerpc ia64 arm armthumb arm64 sparc riscv
XZ_PACK := p1 p2 delta1 delta4 delta256 $(XZ_BCJ)
XZ_STEPS := good1 good2 good3 bad1 bad2 bad3 bad4 bad5 bad6 quiet list $(XZ_PACK) unpack
XZ_ARGS_good1 := -dc $(wordlist 1,11,$(XZ_GOOD))
XZ_ARGS_good2 := -dc $(wordlist 12,22,$(XZ_GOOD))
XZ_ARGS_good3 := -dc $(wordlist 23,33,$(XZ_GOOD))
XZ_ARGS_bad1 := -dc $(wordlist 1,11,$(XZ_BAD)) $(XZ_HELLO)
XZ_ARGS_bad2 := -dc $(wordlist 12,22,$(XZ_BAD)) $(XZ_HELLO)
XZ_ARGS_bad3 := -dc $(wordlist 23,33,$(XZ_BAD)) $(XZ_HELLO)
XZ_ARGS_bad4 := -dc $(wordlist 34,43,$(XZ_BAD)) $(XZ_HELLO)
XZ_ARGS_bad5 := -dc $(wordlist 44,53,$(XZ_BAD)) $(XZ_HELLO)
XZ_ARGS_bad6 := -dc $(wordlist 54,62,$(XZ_BAD)) $(XZ_HELLO)
$(foreach n,1 2 3 4 5 6,$(eval XZ_RC_bad$(n) := 1))
XZ_ARGS_quiet := -dcqQ EMU68/unsupported-check.xz
XZ_ARGS_list := -lvv $(patsubst %,EMU68/good-%.xz,0cat-empty 0catpad-empty 1-check-none \
                1-check-crc32 1-check-crc64 1-check-sha256 1-3delta-lzma2 1-arm64-lzma2-1 \
                1-block_header-1 2-lzma2)
$(foreach p,1 2,$(eval XZ_ARGS_p$(p) := -c -$(p) $(XZ_MEM) $(XZ_INPUTS)))
$(foreach d,1 4 256,$(eval XZ_ARGS_delta$(d) := -c $(XZ_MEM) --filters=delta:dist=$(d)--$(XZ_LZMA2) $(XZ_INPUTS)))
$(foreach f,$(XZ_BCJ),$(eval XZ_ARGS_$(f) := -c $(XZ_MEM) --filters=$(f)--$(XZ_LZMA2) $(XZ_INPUTS)))
XZ_ARGS_unpack := -dc $(XZ_MEM)
ifneq ($(XZ_SRC),)
PROGRAMS += xz
endif

# programs/test/selftest of mbed TLS 3.6.7, from the source the AROS build
# fetches for its Ports: set MBEDTLS_SRC to its mbedtls directory to build
# it as mbedtls_selftest, for the 68020, with mbedtls_config.h as its
# configuration. Its output must be the one of the same program built for
# the host, which "make mbedtls-refs" writes to $(MBEDTLS_REFS).
MBEDTLS_SRC ?=
MBEDTLS_REFS := $(BUILD_DIR)/mbedtls-refs
MBEDTLS_SOURCES := $(wildcard $(MBEDTLS_SRC)/library/*.c)
MBEDTLS_FLAGS := -DMBEDTLS_CONFIG_FILE='"$(TEST_DIR)/mbedtls_config.h"' \
                 -I$(MBEDTLS_SRC)/include -I$(MBEDTLS_SRC)/library
ifneq ($(MBEDTLS_SRC),)
PROGRAMS += mbedtls_selftest
endif

# lua of Lua 5.4.7: set LUA_SRC to its lua-5.4.7 directory (with
# lua-5.4.7-tests next to it or in $(LUA_TESTS_SRC)) to build it, with
# lua_config.h in front of each file and, for the m68k, lua_libnix.c for
# the floating-point snprintf(), strtod(), double comparison and division
# cases where libnix differs from ISO C, with the exact %e, %f and %g of
# libnix_fmt.c. The battery runs it on the twelve deterministic scripts of
# lua-5.4.7-tests that need no C test library, a standard-input step and an
# unhandled-error step, which $(LUA_FILES) collects under EMU68/, and
# compares its output with the one of the same program built for the host,
# which "make lua-refs" writes to $(LUA_REFS).
LUA_SRC ?=
LUA_TESTS_SRC ?= $(firstword $(wildcard $(LUA_SRC)-tests $(LUA_SRC)/lua-5.4.7-tests $(LUA_SRC)/tests))
LUA_FILES := $(BUILD_DIR)/lua-files
LUA_REFS := $(BUILD_DIR)/lua-refs
LUA_SOURCES := $(filter-out %/luac.c,$(sort $(wildcard $(LUA_SRC)/src/*.c)))
LUA_FLAGS := -I$(LUA_SRC)/src -include $(TEST_DIR)/lua_config.h
LUA_TESTS := strings nextvar closure calls events vararg pm utf8 bitwise literals goto errors
LUA_STEPS := $(LUA_TESTS) stdin bad
LUA_INIT := -e "_port=true _soft=true"
$(foreach s,$(LUA_TESTS),$(eval LUA_ARGS_$(s) := $(LUA_INIT) EMU68/$(s).lua))
LUA_ARGS_stdin := -
LUA_ARGS_bad := EMU68/bad.lua
LUA_RC_bad := 1
ifneq ($(LUA_SRC),)
PROGRAMS += lua
endif

# unzip of Info-ZIP UnZip 6.0: set UNZIP_SRC to an unpacked unzip60
# directory to build the Unix port
# (unix/unix.c) with libnix, with unzip_config.h included in front and, for
# the m68k, unzip_libnix.c so timestamps stay in UTC0 without opening
# locale.library. The battery runs it through nine steps on the deterministic
# archives that make-unzip-files.py writes to $(UNZIP_FILES) under EMU68/,
# and compares its output with the one of the same program built for the
# host, which "make unzip-refs" writes to $(UNZIP_REFS) under TZ=UTC0.
UNZIP_SRC ?=
UNZIP_FILES := $(BUILD_DIR)/unzip-files
UNZIP_REFS := $(BUILD_DIR)/unzip-refs
UNZIP_CORE := unzip crc32 crypt envargs explode extract fileio globals \
              inflate list match process ttyio ubz2err unreduce unshrink \
              zipinfo unix/unix
UNZIP_SOURCES := $(addprefix $(UNZIP_SRC)/,$(addsuffix .c,$(UNZIP_CORE)))
UNZIP_FLAGS := -I$(UNZIP_SRC) -include $(TEST_DIR)/unzip_config.h
UNZIP_STEPS := list verbose zipinfo test pipe extract overwrite bad_trunc bad_crc
UNZIP_ARGS_list := -l EMU68/tree.zip
UNZIP_ARGS_verbose := -v EMU68/tree.zip
UNZIP_ARGS_zipinfo := -Z EMU68/tree.zip
UNZIP_ARGS_test := -t EMU68/tree.zip
UNZIP_ARGS_pipe := -p EMU68/tree.zip sub/nested/deflated.txt
UNZIP_ARGS_extract := -o EMU68/tree.zip -d T:unzip_tree
UNZIP_ARGS_overwrite := -o EMU68/tree.zip -d T:unzip_tree
UNZIP_ARGS_bad_trunc := -l EMU68/bad_trunc.zip
UNZIP_ARGS_bad_crc := -p EMU68/bad_crc.zip
UNZIP_RC_bad_trunc := 9
UNZIP_RC_bad_crc := 2
ifneq ($(UNZIP_SRC),)
PROGRAMS += unzip
endif

# awk of the one true awk (20260426), from an unpacked awk tree:
# set AWK_SRC to its directory to build it, with awk_config.h in front of
# each file, awkgram.tab.c and awkgram.tab.h generated from awkgram.y by
# $(YACC), proctab.c generated from awkgram.tab.h by maketab built for the
# host, and, for the m68k, awk_libnix.c for environ, pclose(), locale
# initialization, snprintf() and strtod(), with the exact %e, %f and %g of
# libnix_fmt.c. The battery runs it on six
# deterministic p.* programs from testdir on test.countries, eight
# deterministic t.* programs from testdir on test.data, six programs of my
# own (printf, regex, getline, redir, strings and system) and a fatal-error
# step (bad), which $(AWK_FILES) collects under EMU68/, and compares its
# output with the one of the same program built for the host, which
# "make awk-refs" writes to $(AWK_REFS).
AWK_SRC ?=
ifeq ($(origin YACC),default)
YACC := bison -d -y
endif
YACC ?= bison -d -y
AWK_BUILD := $(BUILD_DIR)/awk-src
AWK_FILES := $(BUILD_DIR)/awk-files
AWK_REFS := $(BUILD_DIR)/awk-refs
AWK_SOURCES := $(addprefix $(AWK_SRC)/,b.c main.c parse.c tran.c lib.c run.c lex.c)
AWK_GEN_SOURCES := $(AWK_BUILD)/awkgram.tab.c $(AWK_BUILD)/proctab.c
AWK_FLAGS := -I$(AWK_BUILD) -I$(AWK_SRC) -include $(TEST_DIR)/awk_config.h
AWK_P_TESTS := p.5 p.24 p.25 p.36 p.43 p.table
AWK_T_TESTS := t.array t.builtins t.fun5 t.getline1 t.printf t.roff t.split4 t.sub2
AWK_P_STEPS := p5 p24 p25 p36 p43 ptable
AWK_T_STEPS := tarray tbuiltins tfun5 tgetline1 tprintf troff tsplit4 tsub2
AWK_OWN_STEPS := printf regex getline redir strings system bad
AWK_STEPS := $(AWK_P_STEPS) $(AWK_T_STEPS) $(AWK_OWN_STEPS)
AWK_ARGS_p5 := -f EMU68/p.5 EMU68/test.countries EMU68/test.countries
AWK_ARGS_p24 := -f EMU68/p.24 EMU68/test.countries EMU68/test.countries
AWK_ARGS_p25 := -f EMU68/p.25 EMU68/test.countries EMU68/test.countries
AWK_ARGS_p36 := -f EMU68/p.36 EMU68/test.countries EMU68/test.countries
AWK_ARGS_p43 := -f EMU68/p.43 EMU68/test.countries EMU68/test.countries
AWK_ARGS_ptable := -f EMU68/p.table EMU68/test.countries EMU68/test.countries
AWK_ARGS_tarray := -f EMU68/t.array EMU68/test.data
AWK_ARGS_tbuiltins := -f EMU68/t.builtins EMU68/test.data
AWK_ARGS_tfun5 := -f EMU68/t.fun5 EMU68/test.data
AWK_ARGS_tgetline1 := -f EMU68/t.getline1 EMU68/test.data
AWK_ARGS_tprintf := -f EMU68/t.printf EMU68/test.data
AWK_ARGS_troff := -f EMU68/t.roff EMU68/test.data
AWK_ARGS_tsplit4 := -f EMU68/t.split4 EMU68/test.data
AWK_ARGS_tsub2 := -f EMU68/t.sub2 EMU68/test.data
AWK_ARGS_printf := -f EMU68/awk_printf.awk
AWK_ARGS_regex := -f EMU68/awk_regex.awk EMU68/test.countries
AWK_ARGS_getline := -f EMU68/awk_getline.awk
AWK_ARGS_redir := -f EMU68/awk_redir.awk
AWK_ARGS_strings := -f EMU68/awk_strings.awk
AWK_ARGS_system := -f EMU68/awk_system.awk
AWK_ARGS_bad := -f EMU68/awk_bad.awk EMU68/test.countries
AWK_RC_bad := 2
ifneq ($(AWK_SRC),)
PROGRAMS += awk
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

# Original native foreign-Hook controller; separate from the 68k battery.
$(BUILD_DIR)/paint_fixture_native: $(TEST_DIR)/paint_fixture_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Wextra -Werror -Wno-pointer-sign -o $@ $<

# Portable emitter has no dependency on AROS headers or a cross compiler.
$(BUILD_DIR)/paint_fixture_emit: $(TEST_DIR)/paint_fixture_native.c | $(BUILD_DIR)
	$(HOST_CC) -std=c99 -O1 -Wall -Wextra -Werror -pedantic -DPAINT_FIXTURE_EMIT_ONLY -o $@ $<

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

HOST_CC ?= cc

# The files of the bzip2 steps, installed with --files next to each step:
# the three sample pairs of the distribution, sample1.bz2 with a byte of its
# block CRC changed, and its first 16 KiB as a truncated stream.
$(BZIP2_FILES).stamp: $(addprefix $(BZIP2_SRC)/,sample1.ref sample2.ref sample3.ref \
                      sample1.bz2 sample2.bz2 sample3.bz2) | $(BUILD_DIR)
	rm -rf $(BZIP2_FILES)
	mkdir -p $(BZIP2_FILES)
	cp $^ $(BZIP2_FILES)/
	cp $(BZIP2_SRC)/sample1.bz2 $(BZIP2_FILES)/bad-crc.bz2
	printf '\000' | dd of=$(BZIP2_FILES)/bad-crc.bz2 bs=1 seek=10 count=1 conv=notrunc 2>/dev/null
	head -c 16384 $(BZIP2_SRC)/sample1.bz2 > $(BZIP2_FILES)/bad-cut.bz2
	touch $@

$(BUILD_DIR)/bzip2: $(BZIP2_SOURCES) $(TEST_DIR)/bzip2_amiga.c $(TEST_DIR)/libgcc_ieee.c \
                   $(TEST_DIR)/libgcc_ieee_ports.c | $(BUILD_DIR) $(BZIP2_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) $(M68K_LDFLAGS) -o $@ $^ -lm

# The bzip2 references: the outputs of the steps through bzip2 built for the
# host, run where EMU68 names $(BZIP2_FILES). Each bad step decompresses
# sample1.bz2 first so that its output is not empty, and ends with 2;
# bad_cut runs with -q so that bzip2 does not call perror() with errno 0.
$(BZIP2_REFS)/stamp: $(BZIP2_SOURCES) $(BZIP2_FILES).stamp
	rm -rf $(@D)
	mkdir -p '$(@D)/SYS:EMU68'
	$(HOST_CC) -O2 -o $(@D)/bzip2 $(BZIP2_SOURCES)
	ln -s $(BZIP2_FILES) $(@D)/EMU68
	cd $(@D) && $(foreach s,$(BZIP2_STEPS),cp bzip2 SYS:EMU68/bzip2_$(s) && \
	    { SYS:EMU68/bzip2_$(s) $(BZIP2_ARGS_$(s)) > bzip2_$(s).out 2> bzip2_$(s).err; \
	    test $$? = $(if $(BZIP2_RC_$(s)),$(BZIP2_RC_$(s)),0); } &&) true
	cd $(@D) && cmp -s bzip2_c1.out EMU68/sample1.bz2 && \
	    cmp -s bzip2_c2.out EMU68/sample2.bz2 && \
	    cmp -s bzip2_c3.out EMU68/sample3.bz2 && \
	    cmp -s bzip2_d1.out EMU68/sample1.ref && \
	    cmp -s bzip2_d2.out EMU68/sample2.ref && \
	    cmp -s bzip2_d3.out EMU68/sample3.ref
	touch $@

bzip2-refs: $(BZIP2_REFS)/stamp

# The files of the zstd steps, installed with --files next to each step:
# the archives of tests/golden-decompression and
# tests/golden-decompression-errors, and the first 16 KiB of each file of
# tests/golden-compression.
$(ZSTD_FILES).stamp: $(wildcard $(ZSTD_SRC)/tests/golden-decompression/*.zst \
                     $(ZSTD_SRC)/tests/golden-decompression-errors/*.zst \
                     $(ZSTD_SRC)/tests/golden-compression/*) | $(BUILD_DIR)
	rm -rf $(ZSTD_FILES)
	mkdir -p $(ZSTD_FILES)
	cp $(ZSTD_SRC)/tests/golden-decompression/*.zst \
	   $(ZSTD_SRC)/tests/golden-decompression-errors/*.zst $(ZSTD_FILES)/
	for f in $(ZSTD_SRC)/tests/golden-compression/*; do \
	    head -c 16384 "$$f" > $(ZSTD_FILES)/$$(basename "$$f") || exit 1; done
	touch $@

# -lm brings the double helpers of the sizes and ratios that zstd prints
$(BUILD_DIR)/zstd: $(ZSTD_SOURCES) | $(BUILD_DIR) $(ZSTD_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) $(ZSTD_FLAGS) $(M68K_LDFLAGS) -o $@ $(ZSTD_SOURCES) -lm

# The zstd references: the outputs of the steps through zstd built for the
# host, run where EMU68 names $(ZSTD_FILES). zstd_bad ends with 1, and
# zstd_unpack reads zstd_unpack.in, the outputs of the four compression
# steps one after the other.
$(ZSTD_REFS)/stamp: $(ZSTD_SOURCES) $(ZSTD_FILES).stamp
	rm -rf $(@D)
	mkdir -p '$(@D)/SYS:EMU68'
	$(HOST_CC) -O2 $(ZSTD_FLAGS) -o $(@D)/zstd $(ZSTD_SOURCES)
	ln -s $(ZSTD_FILES) $(@D)/EMU68
	cd $(@D) && $(foreach s,$(filter-out unpack,$(ZSTD_STEPS)),cp zstd SYS:EMU68/zstd_$(s) && \
	    { SYS:EMU68/zstd_$(s) $(ZSTD_ARGS_$(s)) > zstd_$(s).out 2> zstd_$(s).err; \
	    test $$? = $(if $(ZSTD_RC_$(s)),$(ZSTD_RC_$(s)),0); } &&) true
	cd $(@D) && cat $(patsubst %,zstd_%.out,$(ZSTD_PACK)) > zstd_unpack.in
	cd $(@D) && cp zstd SYS:EMU68/zstd_unpack && \
	    SYS:EMU68/zstd_unpack $(ZSTD_ARGS_unpack) < zstd_unpack.in > zstd_unpack.out 2> zstd_unpack.err
	cd $(@D) && cat $(ZSTD_INPUTS) $(ZSTD_INPUTS) $(ZSTD_INPUTS) $(ZSTD_INPUTS) | cmp -s - zstd_unpack.out
	touch $@

zstd-refs: $(ZSTD_REFS)/stamp

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
	cd $(@D) && { ./pngtest -m $(PNG_BADPAL) > png_badpal.out 2> png_badpal.err; test $$? = $(PNG_RC_badpal); }
	rm -f $(@D)/pngout.png
	touch $@

png-refs: $(PNG_REFS)/stamp

# What the configure of expat writes, without the sources of entropy that
# AmigaOS lacks: with XML_POOR_ENTROPY expat salts its hash tables from the
# time, which leaves its output as it is. BYTEORDER is left out, so that
# the host build finds its own; the m68k build gives 4321.
$(EXPAT_BUILD)/expat_config.h:
	mkdir -p $(@D)
	printf '#define XML_DTD 1\n#define XML_NS 1\n#define XML_GE 1\n#define XML_CONTEXT_BYTES 1024\n#define XML_POOR_ENTROPY 1\n' > $@

# The documents of the steps, installed with --files next to each step
$(XMLWF_FILES).stamp: $(XMLWF_DOCS) | $(BUILD_DIR)
	rm -rf $(XMLWF_FILES)
	mkdir -p $(XMLWF_FILES)
	cp $(XMLWF_DOCS) $(XMLWF_FILES)/
	touch $@

# -lm brings strtof() and the float helpers of the amplification limits
$(BUILD_DIR)/xmlwf: $(EXPAT_SRC)/xmlwf/xmlwf.c $(EXPAT_SOURCES) $(EXPAT_BUILD)/expat_config.h \
                    $(TEST_DIR)/xmlwf_stdout.h | $(BUILD_DIR) $(XMLWF_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) -DBYTEORDER=4321 $(EXPAT_FLAGS) $(M68K_LDFLAGS) \
	    -o $@ $< $(EXPAT_SOURCES) -lm

# The xmlwf references: the outputs of the seven steps through xmlwf built
# for the host, run where EMU68 names $(XMLWF_FILES). -v prints the name
# that xmlwf runs under, so that step runs as xmlwf_version, as in the
# guest.
$(XMLWF_REFS)/stamp: $(EXPAT_SRC)/xmlwf/xmlwf.c $(EXPAT_SOURCES) $(EXPAT_BUILD)/expat_config.h \
                     $(TEST_DIR)/xmlwf_stdout.h $(XMLWF_FILES).stamp
	mkdir -p $(@D)
	$(HOST_CC) -O2 $(EXPAT_FLAGS) -o $(@D)/xmlwf $(EXPAT_SRC)/xmlwf/xmlwf.c $(EXPAT_SOURCES)
	rm -f $(@D)/EMU68
	ln -s $(XMLWF_FILES) $(@D)/EMU68
	cd $(@D) && cp xmlwf xmlwf_version && ./xmlwf_version $(XMLWF_VERSION) > xmlwf_version.out
	cd $(@D) && ./xmlwf $(XMLWF_CANON) > xmlwf_canon.out
	cd $(@D) && ./xmlwf $(XMLWF_NS) > xmlwf_ns.out
	cd $(@D) && ./xmlwf $(XMLWF_META) > xmlwf_meta.out
	cd $(@D) && ./xmlwf $(XMLWF_COPY) > xmlwf_copy.out
	cd $(@D) && ./xmlwf $(XMLWF_STDIN) < EMU68/AndroidManifest.xml > xmlwf_stdin.out
	cd $(@D) && { ./xmlwf $(XMLWF_ERRORS) > xmlwf_errors.out; test $$? = $(XMLWF_RC_errors); }
	touch $@

xmlwf-refs: $(XMLWF_REFS)/stamp

$(XZ_BUILD)/config.h: $(TEST_DIR)/xz_config.h
	mkdir -p $(@D)
	cp $< $@

# The files of the steps, installed with --files next to each step: those of
# tests/files, and the first 16 KiB of the three that create_compress_files,
# built for the host, writes for tests/test_compress.sh. xz compresses about
# 1 KB a second under kaffeine: with the whole files, 772 KB, a step took 13
# to 15 minutes a run.
$(XZ_FILES).stamp: $(XZ_SRC)/tests/create_compress_files.c $(XZ_BUILD)/config.h | $(BUILD_DIR)
	rm -rf $(XZ_FILES)
	mkdir -p $(XZ_FILES)
	cp $(addprefix $(XZ_SRC)/tests/files/,*.xz *.lzma *.lz) $(XZ_FILES)/
	$(HOST_CC) -O2 $(XZ_FLAGS) -o $(XZ_BUILD)/create_compress_files $<
	cd $(XZ_BUILD) && for f in abc random text; do \
	    ./create_compress_files compress_generated_$$f && \
	    head -c 16384 compress_generated_$$f > $(XZ_FILES)/compress_generated_$$f || exit 1; done
	touch $@

# -lm brings the double helpers of the sizes and ratios that xz prints
$(BUILD_DIR)/xz: $(XZ_SOURCES) $(TEST_DIR)/xz_libnix.c $(XZ_BUILD)/config.h | $(BUILD_DIR) \
                 $(XZ_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) -DWORDS_BIGENDIAN $(XZ_FLAGS) $(M68K_LDFLAGS) \
	    -o $@ $(XZ_SOURCES) $(TEST_DIR)/xz_libnix.c -lm

# The xz references: the outputs of the steps through xz built for the host,
# run where EMU68 names $(XZ_FILES). xz begins its messages with its name as
# it was started, SYS:EMU68/NAME in the guest, so each step runs as that here
# too, and writes its error output to NAME.err. The bad steps end with 1.
# xz_unpack reads xz_unpack.in, the outputs of the thirteen compression
# steps one after the other.
$(XZ_REFS)/stamp: $(XZ_SOURCES) $(XZ_BUILD)/config.h $(XZ_FILES).stamp
	rm -rf $(@D)
	mkdir -p '$(@D)/SYS:EMU68'
	$(HOST_CC) -O2 $(XZ_FLAGS) -o $(@D)/xz $(XZ_SOURCES)
	ln -s $(XZ_FILES) $(@D)/EMU68
	cd $(@D) && $(foreach s,$(filter-out unpack,$(XZ_STEPS)),cp xz SYS:EMU68/xz_$(s) && \
	    { SYS:EMU68/xz_$(s) $(XZ_ARGS_$(s)) > xz_$(s).out 2> xz_$(s).err; \
	    test $$? = $(if $(XZ_RC_$(s)),$(XZ_RC_$(s)),0); } &&) true
	cd $(@D) && cat $(patsubst %,xz_%.out,$(XZ_PACK)) > xz_unpack.in
	cd $(@D) && cp xz SYS:EMU68/xz_unpack && \
	    SYS:EMU68/xz_unpack $(XZ_ARGS_unpack) < xz_unpack.in > xz_unpack.out 2> xz_unpack.err
	touch $@

xz-refs: $(XZ_REFS)/stamp

# The modules that mbedtls_config.h leaves out compile to nothing
$(BUILD_DIR)/mbedtls_selftest: $(MBEDTLS_SRC)/programs/test/selftest.c $(MBEDTLS_SOURCES) \
                               $(TEST_DIR)/mbedtls_config.h | $(BUILD_DIR)
	$(M68K_CC) $(M68K_CFLAGS) -m68020 $(MBEDTLS_FLAGS) $(M68K_LDFLAGS) -o $@ $< $(MBEDTLS_SOURCES)

$(MBEDTLS_REFS)/stamp: $(MBEDTLS_SRC)/programs/test/selftest.c $(MBEDTLS_SOURCES) \
                       $(TEST_DIR)/mbedtls_config.h
	rm -rf $(@D)
	mkdir -p $(@D)
	$(HOST_CC) -O2 $(MBEDTLS_FLAGS) -o $(@D)/mbedtls_selftest $< $(MBEDTLS_SOURCES)
	$(@D)/mbedtls_selftest > $(@D)/mbedtls_selftest.out
	touch $@

mbedtls-refs: $(MBEDTLS_REFS)/stamp

$(LUA_FILES).stamp: $(patsubst %,$(LUA_TESTS_SRC)/%.lua,$(LUA_TESTS) bwcoercion) \
                    $(TEST_DIR)/lua_stdin.lua $(TEST_DIR)/lua_bad.lua | $(BUILD_DIR)
	rm -rf $(LUA_FILES)
	mkdir -p $(LUA_FILES)
	cp $(patsubst %,$(LUA_TESTS_SRC)/%.lua,$(LUA_TESTS) bwcoercion) $(LUA_FILES)/
	cp $(TEST_DIR)/lua_stdin.lua $(LUA_FILES)/stdin.lua
	cp $(TEST_DIR)/lua_bad.lua $(LUA_FILES)/bad.lua
	touch $@

# -lm brings the IEEE double helpers and the floating-point printf() of libnix
$(BUILD_DIR)/lua: $(LUA_SOURCES) $(TEST_DIR)/lua_libnix.c $(TEST_DIR)/lua_config.h \
                  $(TEST_DIR)/libnix_fmt.c $(TEST_DIR)/libnix_fmt.h | $(BUILD_DIR) \
                  $(LUA_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) $(LUA_FLAGS) $(M68K_LDFLAGS) \
	    -o $@ $(LUA_SOURCES) $(TEST_DIR)/lua_libnix.c $(TEST_DIR)/libnix_fmt.c -lm

# The lua references: the outputs of the steps through lua built for the
# host, run where EMU68 names $(LUA_FILES). lua begins its error messages
# with its name as it was started, SYS:EMU68/NAME in the guest, so each step
# runs as that here too, and writes its error output to NAME.err. lua_stdin
# reads EMU68/stdin.lua from its input, and lua_bad ends with 1.
$(LUA_REFS)/stamp: $(LUA_SOURCES) $(TEST_DIR)/lua_config.h $(LUA_FILES).stamp
	rm -rf $(@D)
	mkdir -p '$(@D)/SYS:EMU68'
	$(HOST_CC) -O2 -Wno-deprecated-declarations $(LUA_FLAGS) -o $(@D)/lua $(LUA_SOURCES) -lm
	ln -s $(LUA_FILES) $(@D)/EMU68
	cd $(@D) && $(foreach s,$(filter-out stdin,$(LUA_STEPS)),cp lua SYS:EMU68/lua_$(s) && \
	    { TZ=UTC0 SYS:EMU68/lua_$(s) $(LUA_ARGS_$(s)) > lua_$(s).out 2> lua_$(s).err; \
	    test $$? = $(if $(LUA_RC_$(s)),$(LUA_RC_$(s)),0); } &&) true
	cd $(@D) && cp lua SYS:EMU68/lua_stdin && \
	    TZ=UTC0 SYS:EMU68/lua_stdin $(LUA_ARGS_stdin) < EMU68/stdin.lua > lua_stdin.out 2> lua_stdin.err
	touch $@

lua-refs: $(LUA_REFS)/stamp

$(UNZIP_FILES).stamp: $(TEST_DIR)/make-unzip-files.py | $(BUILD_DIR)
	rm -rf $(UNZIP_FILES)
	$(PYTHON) $(TEST_DIR)/make-unzip-files.py $(UNZIP_FILES)
	touch $@

$(BUILD_DIR)/unzip: $(UNZIP_SOURCES) $(TEST_DIR)/unzip_libnix.c $(TEST_DIR)/unzip_config.h | $(BUILD_DIR) \
                    $(UNZIP_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) $(UNZIP_FLAGS) $(M68K_LDFLAGS) \
	    -o $@ $(UNZIP_SOURCES) $(TEST_DIR)/unzip_libnix.c

# The unzip references: the outputs of the steps through unzip built for
# the host, run under TZ=UTC0 where EMU68 names $(UNZIP_FILES).
# unzip_bad_trunc ends with 9 (PK_NOZIP) and unzip_bad_crc ends with 2
# (PK_ERR). The extracted tree T:unzip_tree is removed after the steps run.
$(UNZIP_REFS)/stamp: $(UNZIP_SOURCES) $(TEST_DIR)/unzip_config.h $(UNZIP_FILES).stamp
	rm -rf $(@D)
	mkdir -p '$(@D)/SYS:EMU68'
	$(HOST_CC) -O2 -Wno-deprecated-non-prototype -Wno-format-security $(UNZIP_FLAGS) \
	    -o $(@D)/unzip $(UNZIP_SOURCES)
	ln -s $(UNZIP_FILES) $(@D)/EMU68
	cd $(@D) && $(foreach s,$(UNZIP_STEPS),cp unzip SYS:EMU68/unzip_$(s) && \
	    { TZ=UTC0 SYS:EMU68/unzip_$(s) $(UNZIP_ARGS_$(s)) > unzip_$(s).out 2> unzip_$(s).err; \
	    test $$? = $(if $(UNZIP_RC_$(s)),$(UNZIP_RC_$(s)),0); } &&) \
	    rm -rf 'T:unzip_tree'
	touch $@

unzip-refs: $(UNZIP_REFS)/stamp

# A multi-target pattern rule runs Yacc once for both outputs, including
# under parallel builds, and works with GNU make 3.81 as well.
$(AWK_BUILD)/%.tab.c $(AWK_BUILD)/%.tab.h: $(AWK_SRC)/%.y
	mkdir -p $(@D)
	cd $(@D) && $(YACC) -b $* $(abspath $<)
	test -f $(AWK_BUILD)/$*.tab.c || mv $(AWK_BUILD)/y.tab.c $(AWK_BUILD)/$*.tab.c
	test -f $(AWK_BUILD)/$*.tab.h || mv $(AWK_BUILD)/y.tab.h $(AWK_BUILD)/$*.tab.h

$(AWK_BUILD)/proctab.c: $(AWK_SRC)/maketab.c $(AWK_BUILD)/awkgram.tab.h $(AWK_SRC)/awk.h $(AWK_SRC)/proto.h
	mkdir -p $(@D)
	$(HOST_CC) -O2 -I$(AWK_BUILD) -I$(AWK_SRC) -o $(AWK_BUILD)/maketab $<
	$(AWK_BUILD)/maketab $(AWK_BUILD)/awkgram.tab.h > $@

# The scripts and input files of the steps, installed with --files next to each step
$(AWK_FILES).stamp: $(addprefix $(AWK_SRC)/testdir/,test.countries test.data $(AWK_P_TESTS) $(AWK_T_TESTS)) \
                    $(patsubst %,$(TEST_DIR)/awk_%.awk,$(AWK_OWN_STEPS)) | $(BUILD_DIR)
	rm -rf $(AWK_FILES)
	mkdir -p $(AWK_FILES)
	cp $(addprefix $(AWK_SRC)/testdir/,test.countries test.data $(AWK_P_TESTS) $(AWK_T_TESTS)) $(AWK_FILES)/
	cp $(patsubst %,$(TEST_DIR)/awk_%.awk,$(AWK_OWN_STEPS)) $(AWK_FILES)/
	touch $@

# -lm brings the IEEE double helpers and the floating-point printf() of libnix
$(BUILD_DIR)/awk: $(AWK_SOURCES) $(AWK_GEN_SOURCES) $(TEST_DIR)/awk_libnix.c $(TEST_DIR)/awk_config.h \
                  $(TEST_DIR)/libnix_fmt.c $(TEST_DIR)/libnix_fmt.h | $(BUILD_DIR) $(AWK_FILES).stamp
	$(M68K_CC) $(M68K_CFLAGS) $(AWK_FLAGS) $(M68K_LDFLAGS) \
	    -o $@ $(AWK_GEN_SOURCES) $(AWK_SOURCES) $(TEST_DIR)/awk_libnix.c $(TEST_DIR)/libnix_fmt.c -lm

# The awk references: the outputs of the steps through awk built for the
# host, run where EMU68 names $(AWK_FILES). awk begins its error messages
# with its name as it was started, SYS:EMU68/NAME in the guest, so each step
# runs as that here too, and writes its error output to NAME.err. awk_bad
# ends with 2.
$(AWK_REFS)/stamp: $(AWK_SOURCES) $(AWK_GEN_SOURCES) $(TEST_DIR)/awk_config.h $(AWK_FILES).stamp
	rm -rf $(@D)
	mkdir -p '$(@D)/SYS:EMU68'
	$(HOST_CC) -O2 $(AWK_FLAGS) -o $(@D)/awk $(AWK_GEN_SOURCES) $(AWK_SOURCES) -lm
	ln -s $(AWK_FILES) $(@D)/EMU68
	cd $(@D) && $(foreach s,$(AWK_STEPS),cp awk SYS:EMU68/awk_$(s) && \
	    { SYS:EMU68/awk_$(s) $(AWK_ARGS_$(s)) > awk_$(s).out 2> awk_$(s).err; \
	    test $$? = $(if $(AWK_RC_$(s)),$(AWK_RC_$(s)),0); } &&) rm -f $(@D)/T:awk_redir.txt
	touch $@

awk-refs: $(AWK_REFS)/stamp

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

# bzip2, zstd, the JPEG programs, the zlib ones, pngtest, xmlwf, xz,
# mbedtls_selftest, lua, unzip and awk too, also when they were built
# with a BZIP2_SRC, ZSTD_SRC, JPEG_SRC, ZLIB_SRC, PNG_SRC, EXPAT_SRC,
# XZ_SRC, MBEDTLS_SRC, LUA_SRC, UNZIP_SRC or AWK_SRC that this make lacks
clean:
	rm -f $(PROGRAM_FILES) $(EXEC_LIBRARIES_LIBS) $(BUILD_DIR)/bzip2 $(BZIP2_FILES).stamp $(BUILD_DIR)/zstd $(ZSTD_FILES).stamp $(BUILD_DIR)/exec_public_native
	rm -f $(BUILD_DIR)/cjpeg $(BUILD_DIR)/djpeg $(BUILD_DIR)/jpegtran
	rm -f $(BUILD_DIR)/minigzip $(BUILD_DIR)/zpipe $(BUILD_DIR)/pngtest $(PNG_FILES).stamp
	rm -f $(BUILD_DIR)/xmlwf $(XMLWF_FILES).stamp $(BUILD_DIR)/xz $(XZ_FILES).stamp
	rm -f $(BUILD_DIR)/mbedtls_selftest $(BUILD_DIR)/lua $(LUA_FILES).stamp $(BUILD_DIR)/unzip $(UNZIP_FILES).stamp $(BUILD_DIR)/awk $(AWK_FILES).stamp
	rm -rf $(BZIP2_FILES) $(BZIP2_REFS) $(ZSTD_FILES) $(ZSTD_REFS) $(JPEG_BUILD) $(ZLIB_BUILD) $(ZLIB_REFS) $(PNG_BUILD) $(PNG_FILES) $(PNG_REFS)
	rm -rf $(EXPAT_BUILD) $(XMLWF_FILES) $(XMLWF_REFS) $(XZ_BUILD) $(XZ_FILES) $(XZ_REFS)
	rm -rf $(MBEDTLS_REFS) $(LUA_FILES) $(LUA_REFS) $(UNZIP_FILES) $(UNZIP_REFS) $(AWK_BUILD) $(AWK_FILES) $(AWK_REFS)

# Offline result/parser tests: no cross compiler, mtools or QEMU needed.
check-runner:
	$(PYTHON) -m unittest discover -s $(TEST_DIR) -p 'test_run_raspi_qemu.py' -v

# libnix_fmt.c built for the host and compared with the host snprintf() and
# strtod()
$(BUILD_DIR)/test_libnix_fmt: $(TEST_DIR)/test_libnix_fmt.c $(TEST_DIR)/libnix_fmt.c \
                              $(TEST_DIR)/libnix_fmt.h | $(BUILD_DIR)
	$(HOST_CC) -O2 -Wall -Wno-format-nonliteral -I$(TEST_DIR) -o $@ \
	    $(TEST_DIR)/test_libnix_fmt.c $(TEST_DIR)/libnix_fmt.c -lm

check-libnix-fmt: $(BUILD_DIR)/test_libnix_fmt
	$(BUILD_DIR)/test_libnix_fmt

.PHONY: all clean check-runner check-libnix-fmt bzip2-refs zstd-refs zlib-refs png-refs xmlwf-refs xz-refs mbedtls-refs lua-refs unzip-refs awk-refs

# Native AArch64 framebuffer check: never build this with M68K_CC.
$(BUILD_DIR)/gfx_framebuffer_palette_native: $(TEST_DIR)/gfx_framebuffer_palette_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Werror -o $@ $<

# Native observer after the owned-screen leave probe, in the same boot.
$(BUILD_DIR)/gfx_screen_cleanup_native: $(TEST_DIR)/gfx_screen_cleanup_native.c | $(BUILD_DIR)
	$(NATIVE_CC) -O2 -Wall -Werror -o $@ $<

# The bytecode probe intentionally uses 68020 addressing and MOVE from CCR.
$(BUILD_DIR)/cpu_full_index: M68K_CFLAGS += -m68020
