TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AROS_ROOT := $(abspath $(TEST_DIR)/../../../..)
KAFFEINE_DIR := $(AROS_ROOT)/rom/kaffeine
BUILD_DIR ?= $(TEST_DIR)/build
RUNNER ?=
PYTHON ?= python3

# Keep diagnostics in our adapter and fixtures under -Werror. The pinned,
# unmodified third-party core has a GCC implicit-fallthrough diagnostic.
TEST_CPPFLAGS := -I$(TEST_DIR)/stubs -I$(KAFFEINE_DIR) -isystem $(KAFFEINE_DIR)/Moira
CXXFLAGS ?= -g
TEST_CXXFLAGS := -std=c++20 -Wall -Wextra -Wno-unused-parameter
CFLAGS ?= -g
DOS_CPPFLAGS := -I$(TEST_DIR)/dosstubs -I$(TEST_DIR)/stubs
DOS_CFLAGS := -std=gnu99 -Wall
DOS_LOADSEG_OBJ := $(BUILD_DIR)/dos_loadseg_aos.o
EXEC_LISTS_OBJ := $(BUILD_DIR)/kaffeine_lists.o
DOS_THUNK_CPPFLAGS := -I$(TEST_DIR)/dos_thunk_stubs $(TEST_CPPFLAGS)
DOS_THUNK_CFLAGS := -std=c99 -Wall -Wextra -Wno-unused-parameter
DOS_THUNK_OBJS := $(addprefix $(BUILD_DIR)/dos_thunk_,cpu.o dispatch.o thunks_dos.o cli.o format.o taglist.o segments.o)
DOS_THUNK_OBJS += $(BUILD_DIR)/dos_buffers_native.o $(BUILD_DIR)/file_boundary_unit.o
DOS_THUNK_OBJS += $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o
MEMHEADER_OBJS := $(addprefix $(BUILD_DIR)/memheader_,cpu.o dispatch.o memheader.o)
UTILITY_THUNK_OBJS := $(addprefix $(BUILD_DIR)/utility_thunk_,cpu.o dispatch.o thunks_utility.o taglist.o memory.o format.o)
CALLBACK_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(UTILITY_THUNK_OBJS) $(BUILD_DIR)/utility_thunk_exec_compat.o
HOSTED_JIT_OBJS := $(BUILD_DIR)/hosted_jit.o $(CALLBACK_OBJS)
LOCALE_THUNK_OBJS := $(addprefix $(BUILD_DIR)/locale_thunk_,cpu.o dispatch.o thunks_locale.o memory.o thunks_memory.o hook.o)
LOCALE_HOOK_OBJS := $(LOCALE_THUNK_OBJS) $(addprefix $(BUILD_DIR)/locale_thunk_,thunks_utility.o taglist.o format.o)
LOCALE_HOOK_OBJS += $(BUILD_DIR)/locale_format_native.o
MATHIEEE_THUNK_OBJS := $(addprefix $(BUILD_DIR)/mathieee_thunk_,cpu.o dispatch.o thunks_mathieee.o)
MATHFFP_THUNK_OBJS := $(addprefix $(BUILD_DIR)/mathffp_thunk_,cpu.o dispatch.o thunks_mathffp.o)
LIBRARY_PATHS_OBJS := $(addprefix $(BUILD_DIR)/library_paths_,cpu.o dispatch.o lookup.o)
# Expose strnlen() in production C, without restricting the C++ runtime's
# feature declarations (GCC 15's cstdlib needs quick_exit on macOS).
READARGS_CPPFLAGS := -I$(TEST_DIR)/readargs_stubs $(TEST_CPPFLAGS)
READARGS_C_CPPFLAGS := -D_POSIX_C_SOURCE=200809L $(READARGS_CPPFLAGS)
CLI_CPPFLAGS := -I$(TEST_DIR)/cli_stubs $(TEST_CPPFLAGS)
CLI_OBJS := $(addprefix $(BUILD_DIR)/cli_,cpu.o dispatch.o cli.o)
EXEC_COMPAT_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/exec_compat_,cpu.o dispatch.o exec_compat.o memory.o)
MEMORY_CPPFLAGS := -I$(TEST_DIR)/memory_stubs $(TEST_CPPFLAGS)
MEMORY_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/memory_,cpu.o dispatch.o memory.o thunks_memory.o exec_compat.o)
# Allows mutation tests to compile disposable copies, leaving rom/ untouched.
M68KLIB_SOURCE_DIR ?= $(KAFFEINE_DIR)
M68KLIB_CPPFLAGS := -I$(TEST_DIR)/m68klib_stubs -I$(M68KLIB_SOURCE_DIR) $(MEMORY_CPPFLAGS)
M68KLIB_OBJS := $(BUILD_DIR)/file_boundary_unit.o $(addprefix $(BUILD_DIR)/m68klib_,cpu.o dispatch.o thunks_memory.o memory.o m68klib.o moira.o library_thunks.o segments.o)
TASK_CPPFLAGS := -I$(TEST_DIR)/task_stubs $(TEST_CPPFLAGS)
IO_CPPFLAGS := -I$(TEST_DIR)/io_stubs $(TEST_CPPFLAGS)
IO_OBJS := $(addprefix $(BUILD_DIR)/io_,cpu.o dispatch.o exec_io.o thunks_timer.o thunks_console.o memory.o)
IO_OBJS += $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o
# Disposable bridge sources can be supplied for the mutation checks.
WORKBENCH_SOURCE_DIR ?= $(KAFFEINE_DIR)
WORKBENCH_CPPFLAGS := -I$(TEST_DIR)/workbench_stubs -I$(TEST_DIR)/gfx_stubs -I$(TEST_DIR)/io_stubs $(TEST_CPPFLAGS)
WORKBENCH_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/workbench_,cpu.o dispatch.o memory.o rastport.o taglist.o window.o exec_io.o workbench.o icon.o)
TASK_OBJS := $(BUILD_DIR)/file_boundary_unit.o $(addprefix $(BUILD_DIR)/task_,cpu.o dispatch.o shadow.o)
SCHED_CPPFLAGS := -I$(TEST_DIR)/sched_stubs $(TEST_CPPFLAGS)
SCHED_OBJS := $(addprefix $(BUILD_DIR)/sched_,cpu.o dispatch.o sched.o messages.o)
SCHED_OBJS += $(BUILD_DIR)/sched_jit.o
SCALING_OBJS := $(filter-out $(BUILD_DIR)/memory_memory.o,$(MEMORY_OBJS)) $(BUILD_DIR)/heap_scaling_memory.o
BENCH_OBJS := $(addprefix $(BUILD_DIR)/heap_bench_,memory.o cpu.o)
GFX_CPPFLAGS := -I$(TEST_DIR)/gfx_stubs $(TEST_CPPFLAGS)
# The state calls of caller-owned RastPorts check their storage with the
# production message boundary (Kaffeine_MessageMemory()).
GFX_THUNK_OBJS := $(BUILD_DIR)/gfx_generated_rp.o $(BUILD_DIR)/gfx_thunk_copy.o $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/gfx_thunk_,cpu.o dispatch.o thunks_gfx.o rastport.o fontattr.o taglist.o memory.o)
DISKFONT_CPPFLAGS := -I$(TEST_DIR)/memory_stubs $(GFX_CPPFLAGS)
DISKFONT_OBJS := $(BUILD_DIR)/gfx_thunk_copy.o $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/diskfont_,cpu.o dispatch.o thunks_diskfont.o thunks_gfx.o rastport.o fontattr.o taglist.o memory.o)
WINDOW_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/gfx_thunk_,cpu.o dispatch.o window.o requester.o copy.o format.o rastport.o memory.o taglist.o)
WINDOW_OBJS += $(BUILD_DIR)/gfx_thunk_thunks_memory.o
SCREEN_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/gfx_thunk_,cpu.o dispatch.o screens.o palette.o videocontrol.o rastport.o memory.o taglist.o fontattr.o)
# The layer bridge with the production run loop, window bridge and heap,
# the native hook entry of its private layers and the Region bridge of
# their clip Regions.
LAYER_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/gfx_thunk_,cpu.o dispatch.o window.o requester.o copy.o format.o rastport.o memory.o taglist.o layers.o hook.o regions.o)
LAYER_OBJS += $(BUILD_DIR)/layers_private_unused.o $(BUILD_DIR)/gfx_thunk_exec_compat.o
LAYER_OBJS += $(BUILD_DIR)/gfx_thunk_paint_hook.o $(BUILD_DIR)/gfx_thunk_paint_compile.o
MENU_CPPFLAGS := -I$(TEST_DIR)/menus_stubs $(GFX_CPPFLAGS)
MENU_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/gfx_thunk_,cpu.o dispatch.o window.o mirror.o copy.o rastport.o memory.o taglist.o)
MENU_OBJS += $(BUILD_DIR)/menus_bridge.o
GADGET_CPPFLAGS := -I$(TEST_DIR)/gadgets_stubs $(GFX_CPPFLAGS)
GADGET_OBJS := $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_native_unused.o $(addprefix $(BUILD_DIR)/gfx_thunk_,cpu.o dispatch.o window.o menus.o mirror.o copy.o rastport.o memory.o taglist.o)
GADGET_OBJS += $(BUILD_DIR)/gadgets_bridge.o $(BUILD_DIR)/classic_bridge.o
GUEST_BOUNDS_OBJS := $(addprefix $(BUILD_DIR)/guest_bounds_,cpu.o dispatch.o)
READARGS_PARSER_OBJ := $(BUILD_DIR)/dos_readargs_host.o
READARGS_OBJS := $(addprefix $(BUILD_DIR)/readargs_,cpu.o dispatch.o memory.o readargs.o) $(READARGS_PARSER_OBJ)
NDK_FD_DIR ?=
AROS_FD_DIR ?=
REG_CHECK_ARGS := $(if $(NDK_FD_DIR),--ndk-fd-dir "$(NDK_FD_DIR)",) $(if $(AROS_FD_DIR),--aros-fd-dir "$(AROS_FD_DIR)",)

MESSAGE_CPPFLAGS := -DAROS_FAST_BSTR=1 -I$(TEST_DIR)/message_stubs -I$(TEST_DIR)/dos_thunk_stubs -I$(TEST_DIR)/gfx_stubs -I$(TEST_DIR)/io_stubs $(TEST_CPPFLAGS)
MESSAGE_OBJS := $(addprefix $(BUILD_DIR)/message_boundary_,cpu.o dispatch.o memory.o messages.o exec_compat.o filehandle.o handlers.o format.o shadow.o)
MESSAGE_OBJS += $(BUILD_DIR)/message_entries.o
ICON_CPPFLAGS := -I$(TEST_DIR)/icon_stubs $(MESSAGE_CPPFLAGS)
ICON_OBJS := $(MESSAGE_OBJS) $(BUILD_DIR)/icon_boundary_icon.o

TESTS = test_kaffeine test_moira_exec test_hello_thunks test_dispatch test_hunk_loading
TESTS += test_exec_lists
TESTS += test_locks
TESTS += test_dos_thunks test_exec_messages test_dos_packets
TESTS += test_dos_packet_thunks test_dos_handlers
TESTS += test_icon test_display test_fbgfx_refresh
TESTS += test_memheader test_memheader_moira
TESTS += test_utility_thunks test_locale_thunks test_mathieee_thunks
TESTS += test_mathffp_thunks
TESTS += test_library_paths
TESTS += test_readargs
TESTS += test_cli test_exec_slots test_exec_semaphores test_exec_public test_exec_tasks test_gfx_thunks
TESTS += test_exec_io
TESTS += test_console
TESTS += test_diskfont
TESTS += test_window
TESTS += test_workbench
TESTS += test_menus
TESTS += test_gadgets
TESTS += test_guest_bounds
TESTS += test_callbacks
TESTS += test_locale_hooks
TESTS += test_exec_memory
TESTS += test_heap_scaling
TESTS += test_screens
TESTS += test_layers
TESTS += test_layers_private
TESTS += test_regions
TESTS += test_m68klib
TESTS += test_hosted_jit
TESTS += test_process_tags
TESTS += test_guest_tasks
TEST_BINS := $(addprefix $(BUILD_DIR)/,$(TESTS))

all: $(TEST_BINS)

$(TESTS): %: $(BUILD_DIR)/%

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%: $(TEST_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(EXTRA_OBJS) $(LDLIBS)

# The seglist tests link the DOS HUNK loader, built from its AROS source.
$(DOS_LOADSEG_OBJ): $(TEST_DIR)/dos_loadseg_aos.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(DOS_CPPFLAGS) $(CFLAGS) $(DOS_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_hunk_loading: $(DOS_LOADSEG_OBJ)
$(BUILD_DIR)/test_hunk_loading: EXTRA_OBJS = $(DOS_LOADSEG_OBJ)

# Compile the production list thunks as C, then exercise them through dispatch.
$(EXEC_LISTS_OBJ): $(KAFFEINE_DIR)/kaffeine_lists.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=gnu99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_exec_lists: $(EXEC_LISTS_OBJ)
$(BUILD_DIR)/test_exec_lists: EXTRA_OBJS = $(EXEC_LISTS_OBJ)

# Compile the production DOS thunks and dispatcher as C, then link the C++ test.
PROCESS_TAG_OBJS := $(addprefix $(BUILD_DIR)/process_tags_,cpu.o dispatch.o taglist.o process_tags.o)
$(BUILD_DIR)/process_tags_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(TEST_DIR)/process_tags_stubs $(DOS_THUNK_CPPFLAGS) $(CFLAGS) $(DOS_THUNK_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_process_tags: $(TEST_DIR)/test_process_tags.cpp $(PROCESS_TAG_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -I$(TEST_DIR)/process_tags_stubs $(DOS_THUNK_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(PROCESS_TAG_OBJS) $(LDLIBS)
-include $(PROCESS_TAG_OBJS:.o=.d)

$(BUILD_DIR)/dos_thunk_thunks_dos.o: DOS_THUNK_TRACE_CPPFLAGS := -I$(TEST_DIR)/dos_trace_stubs
$(BUILD_DIR)/dos_thunk_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(DOS_THUNK_TRACE_CPPFLAGS) $(CPPFLAGS) $(DOS_THUNK_CPPFLAGS) $(CFLAGS) $(DOS_THUNK_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/dos_buffers_native.o: $(TEST_DIR)/dos_buffers_native.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(DOS_THUNK_CPPFLAGS) $(CFLAGS) $(DOS_THUNK_CFLAGS) -funsigned-char -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_dos_thunks: $(TEST_DIR)/test_dos_thunks.cpp $(DOS_THUNK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(DOS_THUNK_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(DOS_THUNK_OBJS) $(LDLIBS)

# Both MemHeader tests link the production allocator and dispatch as C objects.
$(BUILD_DIR)/memheader_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_memheader $(BUILD_DIR)/test_memheader_moira: $(MEMHEADER_OBJS)
$(BUILD_DIR)/test_memheader $(BUILD_DIR)/test_memheader_moira: EXTRA_OBJS = $(MEMHEADER_OBJS)

# Utility thunks with the production heap and injectable native allocation.
$(BUILD_DIR)/utility_thunk_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/utility_arithmetic_generated.h: $(TEST_DIR)/utility_arithmetic_extract.py $(KAFFEINE_DIR)/kaffeine_thunks_gen.c $(AROS_ROOT)/rom/utility/utility.conf | $(BUILD_DIR)
	$(PYTHON) $(TEST_DIR)/utility_arithmetic_extract.py $(KAFFEINE_DIR) $(AROS_ROOT)/rom/utility/utility.conf $@

$(BUILD_DIR)/test_utility_thunks: $(TEST_DIR)/test_utility_thunks.cpp $(BUILD_DIR)/utility_arithmetic_generated.h $(UTILITY_THUNK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -I$(BUILD_DIR) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(UTILITY_THUNK_OBJS) $(LDLIBS)

$(BUILD_DIR)/test_callbacks: $(TEST_DIR)/test_callbacks.cpp $(CALLBACK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(CALLBACK_OBJS) $(LDLIBS)

# The actual loader, heap, callback engine and memory thunks, compiled as C.
$(BUILD_DIR)/m68klib_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(M68KLIB_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/m68klib_memory.o: $(M68KLIB_SOURCE_DIR)/kaffeine_memory.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(M68KLIB_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/m68klib_m68klib.o: $(M68KLIB_SOURCE_DIR)/kaffeine_m68klib.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(M68KLIB_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

# Compile only the relevant thunk bodies, extracted verbatim from production.
$(BUILD_DIR)/m68klib_library_thunks.c: $(TEST_DIR)/m68klib_extract_thunks.py $(M68KLIB_SOURCE_DIR)/kaffeine_thunks.c $(M68KLIB_SOURCE_DIR)/kaffeine_thunks_dos.c $(KAFFEINE_DIR)/kaffeine_thunks_dos.h | $(BUILD_DIR)
	$(PYTHON) $< $(M68KLIB_SOURCE_DIR)/kaffeine_thunks.c $(M68KLIB_SOURCE_DIR)/kaffeine_thunks_dos.c $(KAFFEINE_DIR)/kaffeine_thunks_dos.h $@

$(BUILD_DIR)/m68klib_library_thunks.o: $(BUILD_DIR)/m68klib_library_thunks.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(M68KLIB_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/m68klib_moira.o: $(TEST_DIR)/m68klib_moira.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(M68KLIB_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_m68klib: $(TEST_DIR)/test_m68klib.cpp $(M68KLIB_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(M68KLIB_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(M68KLIB_OBJS) $(LDLIBS)

# Native library lookup and OpenLibrary wrappers, extracted from production.
$(BUILD_DIR)/library_paths_lookup.c: $(TEST_DIR)/library_paths_extract.py $(KAFFEINE_DIR)/kaffeine_thunks.c $(KAFFEINE_DIR)/kaffeine_thunks_gen.c $(KAFFEINE_DIR)/kaffeine_memory.c | $(BUILD_DIR)
	$(PYTHON) $< $(KAFFEINE_DIR) $@

$(BUILD_DIR)/library_paths_lookup.o: $(BUILD_DIR)/library_paths_lookup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/library_paths_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_library_paths: $(TEST_DIR)/test_library_paths.cpp $(LIBRARY_PATHS_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(LIBRARY_PATHS_OBJS) $(LDLIBS)

# Locale thunks with the production heap and controlled native locale stubs.
$(BUILD_DIR)/locale_thunk_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_locale_thunks: $(TEST_DIR)/test_locale_thunks.cpp $(LOCALE_THUNK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(LOCALE_THUNK_OBJS) $(LDLIBS)

$(BUILD_DIR)/test_locale_hooks: $(TEST_DIR)/test_locale_hooks.cpp $(LOCALE_HOOK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(LOCALE_HOOK_OBJS) $(LDLIBS)

$(BUILD_DIR)/locale_format_native.o: $(TEST_DIR)/locale_format_native.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

# The mathieeedoubbas and mathieeedoubtrans thunks, compiled as C and called through LVOs.
$(BUILD_DIR)/mathieee_thunk_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_mathieee_thunks: $(MATHIEEE_THUNK_OBJS)
$(BUILD_DIR)/test_mathieee_thunks: EXTRA_OBJS = $(MATHIEEE_THUNK_OBJS)
$(BUILD_DIR)/test_mathieee_thunks: LDLIBS += -lm
# This oracle compares runtime libm bits. GCC 15 on macOS folds sin(-0)
# to +0 in the finite-angle loop unless these calls remain actual calls.
$(BUILD_DIR)/test_mathieee_thunks: TEST_CXXFLAGS += -fno-builtin-sin -fno-builtin-cos

# Integer FFP thunks, real CPU accessors and production dispatcher, without a native library.
$(BUILD_DIR)/mathffp_thunk_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_mathffp_thunks: $(MATHFFP_THUNK_OBJS)
$(BUILD_DIR)/test_mathffp_thunks: EXTRA_OBJS = $(MATHFFP_THUNK_OBJS)
$(BUILD_DIR)/test_mathffp_thunks: LDLIBS += -lm

# The ReadArgs thunks run the native parser from rom/dos, built for the host,
# with the production heap, dispatch and thunks compiled as C.
$(READARGS_PARSER_OBJ): $(TEST_DIR)/dos_readargs_host.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(READARGS_C_CPPFLAGS) $(CFLAGS) -std=gnu99 -Wall -Wno-pointer-sign -MMD -MP -c -o $@ $<

$(BUILD_DIR)/readargs_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(READARGS_C_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_readargs: $(TEST_DIR)/test_readargs.cpp $(READARGS_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(READARGS_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(READARGS_OBJS) $(LDLIBS)

# The CLI snapshot and safe Exec slot overrides share the real dispatcher.
$(BUILD_DIR)/cli_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_cli: $(TEST_DIR)/test_cli.cpp $(CLI_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(CLI_OBJS) $(LDLIBS)

$(BUILD_DIR)/exec_compat_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_exec_slots $(BUILD_DIR)/test_exec_semaphores $(BUILD_DIR)/test_exec_public: $(EXEC_COMPAT_OBJS)
$(BUILD_DIR)/test_exec_slots $(BUILD_DIR)/test_exec_semaphores $(BUILD_DIR)/test_exec_public: EXTRA_OBJS = $(EXEC_COMPAT_OBJS)

# The exec I/O handlers and device thunks, with the real dispatcher and
# heap, over a native Exec that the test defines and records.
$(BUILD_DIR)/io_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(IO_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_exec_io: $(TEST_DIR)/test_exec_io.cpp $(IO_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(IO_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(IO_OBJS) $(LDLIBS)

$(BUILD_DIR)/test_console: $(TEST_DIR)/test_console.cpp $(IO_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(IO_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(IO_OBJS) $(LDLIBS)

# The task tests use the real shadow engine, including ExecBase initialization.
$(BUILD_DIR)/task_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TASK_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_exec_tasks: $(TEST_DIR)/test_exec_tasks.cpp $(KAFFEINE_DIR)/kaffeine_moira.cpp $(KAFFEINE_DIR)/Moira/Moira/Moira.cpp $(KAFFEINE_DIR)/Moira/Moira/MoiraDebugger.cpp $(TASK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(TASK_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(TASK_OBJS) $(LDLIBS)

# AddTask() children: the production scheduler, run loop and dispatcher as
# C, over a recording native Exec, with mock CPUs and with real Moira CPUs.
$(BUILD_DIR)/sched_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(SCHED_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_guest_tasks: $(TEST_DIR)/test_guest_tasks.cpp $(KAFFEINE_DIR)/kaffeine_moira.cpp $(KAFFEINE_DIR)/Moira/Moira/Moira.cpp $(KAFFEINE_DIR)/Moira/Moira/MoiraDebugger.cpp $(SCHED_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(SCHED_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(SCHED_OBJS) $(LDLIBS)

# Compile the heap and memory thunks as C and count their native allocations.
$(BUILD_DIR)/memory_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_exec_memory: $(TEST_DIR)/test_exec_memory.cpp $(MEMORY_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(MEMORY_OBJS) $(LDLIBS)

# Instrument only the heap's basic blocks, including native record walks.
# trace-pc is supported by GCC and clang and needs no production test hooks.
$(BUILD_DIR)/heap_scaling_memory.o: $(TEST_DIR)/heap_scaling_memory.c $(KAFFEINE_DIR)/kaffeine_memory.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize-coverage=trace-pc -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_heap_scaling: $(TEST_DIR)/test_heap_scaling.cpp $(SCALING_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(SCALING_OBJS) $(LDLIBS)

# Optional timing probe, built without coverage at the caller's chosen flags.
$(BUILD_DIR)/heap_bench_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CFLAGS) -std=c99 -MMD -MP -c -o $@ $<

$(BUILD_DIR)/bench_exec_memory: $(TEST_DIR)/bench_exec_memory.cpp $(BENCH_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(BENCH_OBJS) $(LDLIBS)

bench_exec_memory: $(BUILD_DIR)/bench_exec_memory

# The hosted AArch64 translation probe (docs/kaffeine/hosted-jit-probe.md)
# with the production loop, dispatcher, callback primitive and thunks, and
# Moira as the reference. Translated code runs on AArch64 Darwin or Linux;
# elsewhere only the encoder and translation checks run.
$(BUILD_DIR)/hosted_jit.o: $(KAFFEINE_DIR)/kaffeine_jit.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_hosted_jit: $(TEST_DIR)/test_hosted_jit.cpp $(HOSTED_JIT_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MEMORY_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(HOSTED_JIT_OBJS) $(LDLIBS)

# Timings of the same programs under Moira and the probe; build with the
# optimization to compare, in a build directory of its own.
bench-hosted-jit: $(BUILD_DIR)/test_hosted_jit
	$(RUNNER) $< --bench

$(BUILD_DIR)/gfx_thunk_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/gfx_generated_rp.c: $(TEST_DIR)/gfx_generated_extract.py $(KAFFEINE_DIR)/kaffeine_thunks_gen.c | $(BUILD_DIR)
	$(PYTHON) $(TEST_DIR)/gfx_generated_extract.py $(KAFFEINE_DIR)/kaffeine_thunks_gen.c $@

$(BUILD_DIR)/gfx_generated_rp.o: $(BUILD_DIR)/gfx_generated_rp.c
	$(CC) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_gfx_thunks: $(TEST_DIR)/test_gfx_thunks.cpp $(GFX_THUNK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(GFX_THUNK_OBJS) $(LDLIBS)

# Diskfont and graphics share the production font bridge, dispatcher and
# heap, with counted native allocations and recording library collaborators.
$(BUILD_DIR)/diskfont_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(DISKFONT_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_diskfont: $(TEST_DIR)/test_diskfont.cpp $(DISKFONT_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(DISKFONT_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(DISKFONT_OBJS) $(LDLIBS)

$(BUILD_DIR)/test_window: $(TEST_DIR)/test_window.cpp $(WINDOW_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(WINDOW_OBJS) $(LDLIBS)

# Link the real Workbench bridge, window hooks, Exec I/O, dispatcher and heap.
$(BUILD_DIR)/workbench_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WORKBENCH_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/workbench_workbench.o: $(WORKBENCH_SOURCE_DIR)/kaffeine_workbench.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WORKBENCH_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/workbench_window.o: $(WORKBENCH_SOURCE_DIR)/kaffeine_window.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WORKBENCH_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/workbench_exec_io.o: $(WORKBENCH_SOURCE_DIR)/kaffeine_exec_io.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WORKBENCH_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_workbench: $(TEST_DIR)/test_workbench.cpp $(WORKBENCH_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(WORKBENCH_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(WORKBENCH_OBJS) $(LDLIBS)

$(BUILD_DIR)/test_screens: $(TEST_DIR)/test_screens.cpp $(SCREEN_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(SCREEN_OBJS) $(LDLIBS)

$(BUILD_DIR)/test_layers: $(TEST_DIR)/test_layers.cpp $(LAYER_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(LAYER_OBJS) $(LDLIBS)

# Poisoned private layer collaborators; test_layers_private overrides them.
$(BUILD_DIR)/layers_private_unused.o: $(TEST_DIR)/layers_private_unused.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_layers_private: $(TEST_DIR)/test_layers_private.cpp $(LAYER_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(LAYER_OBJS) $(LDLIBS)

# Regions and clip Regions of private layers over recording native graphics
# Regions and layers, with every generated thunk poisoned.
$(BUILD_DIR)/test_regions: $(TEST_DIR)/test_regions.cpp $(LAYER_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GFX_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(LAYER_OBJS) $(LDLIBS)

# Only the menu bridge and its test use this counted AllocVec stub.
$(BUILD_DIR)/menus_bridge.o: $(KAFFEINE_DIR)/kaffeine_menus.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MENU_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_menus: $(TEST_DIR)/test_menus.cpp $(MENU_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MENU_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(MENU_OBJS) $(LDLIBS)

# Only these bridges and their test use the counted AllocVec stub.
$(BUILD_DIR)/gadgets_bridge.o: $(KAFFEINE_DIR)/kaffeine_gadgets.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GADGET_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/classic_bridge.o: $(KAFFEINE_DIR)/kaffeine_classic.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(GADGET_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_gadgets: $(TEST_DIR)/test_gadgets.cpp $(GADGET_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(GADGET_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -MMD -MP $(LDFLAGS) -o $@ $< $(GADGET_OBJS) $(LDLIBS)

# A backend exception must be caught before reaching these C frames.
$(BUILD_DIR)/guest_bounds_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -fno-unwind-tables -fno-asynchronous-unwind-tables -MMD -MP -c -o $@ $<

$(BUILD_DIR)/test_guest_bounds: $(GUEST_BOUNDS_OBJS)
$(BUILD_DIR)/test_guest_bounds: EXTRA_OBJS = $(GUEST_BOUNDS_OBJS)

check-thunk-registrations:
	$(PYTHON) $(TEST_DIR)/test_thunk_registrations.py --cc "$(CC)" $(REG_CHECK_ARGS)

test: all check-thunk-registrations
	@set -e; for test in $(TEST_BINS); do $(RUNNER) "$$test"; done

# Runs only the DOS seglist load, copy and rebase checks.
diagnose-seglist: $(BUILD_DIR)/test_hunk_loading
	$(RUNNER) $< --diagnose-seglist

clean:
	rm -f $(PROCESS_TAG_OBJS) $(PROCESS_TAG_OBJS:.o=.d)
	rm -f $(LOCK_OBJS) $(LOCK_OBJS:.o=.d)
	rm -f $(MESSAGE_OBJS) $(MESSAGE_OBJS:.o=.d) $(BUILD_DIR)/message_boundary_sched.o $(BUILD_DIR)/message_boundary_sched.d $(BUILD_DIR)/message_entries.c $(BUILD_DIR)/message_messages.o $(BUILD_DIR)/message_messages.d $(BUILD_DIR)/message_native_unused.o $(BUILD_DIR)/message_native_unused.d $(BUILD_DIR)/file_boundary_unit.o $(BUILD_DIR)/file_boundary_unit.d
	rm -f $(DISKFONT_OBJS) $(DISKFONT_OBJS:.o=.d)
	rm -f $(LIBRARY_PATHS_OBJS) $(LIBRARY_PATHS_OBJS:.o=.d) $(BUILD_DIR)/library_paths_lookup.c
	rm -f $(MATHFFP_THUNK_OBJS) $(MATHFFP_THUNK_OBJS:.o=.d)
	rm -f $(WORKBENCH_OBJS) $(WORKBENCH_OBJS:.o=.d)
	rm -f $(M68KLIB_OBJS) $(M68KLIB_OBJS:.o=.d) $(BUILD_DIR)/m68klib_library_thunks.c
	rm -f $(SCREEN_OBJS) $(SCREEN_OBJS:.o=.d)
	rm -f $(LAYER_OBJS) $(LAYER_OBJS:.o=.d)
	rm -f $(LOCALE_HOOK_OBJS) $(LOCALE_HOOK_OBJS:.o=.d)
	rm -f $(CALLBACK_OBJS) $(CALLBACK_OBJS:.o=.d)
	rm -f $(BUILD_DIR)/hosted_jit.o $(BUILD_DIR)/hosted_jit.d
	rm -f $(SCALING_OBJS) $(SCALING_OBJS:.o=.d) $(BENCH_OBJS) $(BENCH_OBJS:.o=.d) $(BUILD_DIR)/bench_exec_memory $(BUILD_DIR)/bench_exec_memory.d
	rm -f $(GUEST_BOUNDS_OBJS) $(GUEST_BOUNDS_OBJS:.o=.d)
	rm -f $(SCHED_OBJS) $(SCHED_OBJS:.o=.d)
	rm -f $(TEST_BINS) $(addsuffix .d,$(TEST_BINS)) $(DOS_LOADSEG_OBJ) $(DOS_LOADSEG_OBJ:.o=.d) $(EXEC_LISTS_OBJ) $(EXEC_LISTS_OBJ:.o=.d) $(DOS_THUNK_OBJS) $(DOS_THUNK_OBJS:.o=.d) $(MEMHEADER_OBJS) $(MEMHEADER_OBJS:.o=.d) $(UTILITY_THUNK_OBJS) $(UTILITY_THUNK_OBJS:.o=.d) $(LOCALE_THUNK_OBJS) $(LOCALE_THUNK_OBJS:.o=.d) $(MATHIEEE_THUNK_OBJS) $(MATHIEEE_THUNK_OBJS:.o=.d) $(READARGS_OBJS) $(READARGS_OBJS:.o=.d) $(CLI_OBJS) $(CLI_OBJS:.o=.d) $(EXEC_COMPAT_OBJS) $(EXEC_COMPAT_OBJS:.o=.d) $(GFX_THUNK_OBJS) $(GFX_THUNK_OBJS:.o=.d) $(MEMORY_OBJS) $(MEMORY_OBJS:.o=.d) $(TASK_OBJS) $(TASK_OBJS:.o=.d) $(IO_OBJS) $(IO_OBJS:.o=.d)

-include $(addsuffix .d,$(TEST_BINS)) $(DOS_LOADSEG_OBJ:.o=.d) $(EXEC_LISTS_OBJ:.o=.d) $(DOS_THUNK_OBJS:.o=.d) $(MEMHEADER_OBJS:.o=.d) $(UTILITY_THUNK_OBJS:.o=.d) $(LOCALE_THUNK_OBJS:.o=.d) $(MATHIEEE_THUNK_OBJS:.o=.d) $(READARGS_OBJS:.o=.d) $(CLI_OBJS:.o=.d) $(EXEC_COMPAT_OBJS:.o=.d) $(GFX_THUNK_OBJS:.o=.d) $(MEMORY_OBJS:.o=.d) $(TASK_OBJS:.o=.d) $(IO_OBJS:.o=.d)
-include $(GUEST_BOUNDS_OBJS:.o=.d)
-include $(SCHED_OBJS:.o=.d)
-include $(MATHFFP_THUNK_OBJS:.o=.d)
-include $(LIBRARY_PATHS_OBJS:.o=.d)
-include $(DISKFONT_OBJS:.o=.d)
-include $(WINDOW_OBJS:.o=.d) $(MENU_OBJS:.o=.d) $(GADGET_OBJS:.o=.d)
-include $(SCREEN_OBJS:.o=.d)
-include $(LAYER_OBJS:.o=.d)
-include $(WORKBENCH_OBJS:.o=.d)
-include $(M68KLIB_OBJS:.o=.d)
-include $(CALLBACK_OBJS:.o=.d)
-include $(BUILD_DIR)/hosted_jit.d
-include $(LOCALE_HOOK_OBJS:.o=.d)
-include $(SCALING_OBJS:.o=.d) $(BENCH_OBJS:.o=.d) $(BUILD_DIR)/bench_exec_memory.d

.PHONY: all test clean diagnose-seglist check-thunk-registrations bench_exec_memory bench-hosted-jit $(TESTS)

$(BUILD_DIR)/message_messages.o: $(KAFFEINE_DIR)/kaffeine_messages.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/message_native_unused.o: $(TEST_DIR)/message_native_unused.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/file_boundary_unit.o: $(TEST_DIR)/file_boundary_unit.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/message_boundary_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/message_entries.c: $(TEST_DIR)/messages_extract.py $(KAFFEINE_DIR)/kaffeine_thunks.c $(KAFFEINE_DIR)/kaffeine_thunks_dos.c $(KAFFEINE_DIR)/kaffeine_window.c $(KAFFEINE_DIR)/kaffeine_exec_io.c | $(BUILD_DIR)
	$(PYTHON) $< $(KAFFEINE_DIR) $@
$(BUILD_DIR)/message_entries.o: $(BUILD_DIR)/message_entries.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/test_exec_messages: $(TEST_DIR)/test_exec_messages.cpp $(MESSAGE_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(MESSAGE_OBJS) $(LDLIBS)
$(BUILD_DIR)/test_dos_packets: $(TEST_DIR)/test_dos_packets.cpp $(MESSAGE_OBJS) $(BUILD_DIR)/message_boundary_sched.o | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(MESSAGE_OBJS) $(BUILD_DIR)/message_boundary_sched.o $(LDLIBS)
$(BUILD_DIR)/test_dos_packet_thunks: $(TEST_DIR)/test_dos_packet_thunks.cpp $(MESSAGE_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(MESSAGE_OBJS) $(LDLIBS)
$(BUILD_DIR)/test_dos_handlers: $(TEST_DIR)/test_dos_handlers.cpp $(MESSAGE_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(MESSAGE_OBJS) $(LDLIBS)
-include $(MESSAGE_OBJS:.o=.d) $(BUILD_DIR)/message_boundary_sched.d $(BUILD_DIR)/message_messages.d
.PHONY: test_exec_messages test_dos_packets

$(BUILD_DIR)/icon_boundary_icon.o: $(KAFFEINE_DIR)/kaffeine_icon.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ICON_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/test_icon: $(TEST_DIR)/test_icon.cpp $(ICON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICON_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(ICON_OBJS) $(LDLIBS)
-include $(BUILD_DIR)/icon_boundary_icon.d

DISPLAY_OBJS := $(MESSAGE_OBJS) $(BUILD_DIR)/display_boundary_display.o
$(BUILD_DIR)/display_boundary_display.o: $(KAFFEINE_DIR)/kaffeine_display.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/test_display: $(TEST_DIR)/test_display.cpp $(DISPLAY_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(MESSAGE_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(DISPLAY_OBJS) $(LDLIBS)
-include $(BUILD_DIR)/display_boundary_display.d

$(BUILD_DIR)/test_fbgfx_refresh: CPPFLAGS += -I$(TEST_DIR)/../../../../arch/aarch64-raspi/hidd/fbgfx
$(BUILD_DIR)/test_fbgfx_refresh: $(TEST_DIR)/../../../../arch/aarch64-raspi/hidd/fbgfx/fbgfx_refresh.h

# Production FileLock metadata, native-reference oracles and the real heap.
LOCK_CPPFLAGS := -DAROS_FAST_BSTR=1 -I$(TEST_DIR)/locks_stubs $(TEST_CPPFLAGS)
LOCK_OBJS := $(addprefix $(BUILD_DIR)/locks_,cpu.o dispatch.o memory.o locks.o)
$(BUILD_DIR)/locks_%.o: $(KAFFEINE_DIR)/kaffeine_%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(LOCK_CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<
$(BUILD_DIR)/test_locks: $(TEST_DIR)/test_locks.cpp $(LOCK_OBJS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(LOCK_CPPFLAGS) $(CXXFLAGS) $(TEST_CXXFLAGS) -Werror -MMD -MP $(LDFLAGS) -o $@ $< $(LOCK_OBJS) $(LDLIBS)
-include $(LOCK_OBJS:.o=.d)
