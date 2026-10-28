TEST_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AROS_ROOT := $(abspath $(TEST_DIR)/../../../..)
KAFFEINE_DIR := $(AROS_ROOT)/rom/kaffeine
HOST_TEST_DIR := $(TEST_DIR)/../kaffeine_host
BUILD_DIR ?= $(TEST_DIR)/build
AROS_CC ?= aarch64-aros-gcc
AROS_SYSROOT ?=
CFLAGS ?= -O2 -g
CPPFLAGS += $(if $(AROS_SYSROOT),--sysroot=$(AROS_SYSROOT),) -I$(KAFFEINE_DIR) -I$(HOST_TEST_DIR)
PROBE_OBJS := $(addprefix $(BUILD_DIR)/,jit_probe.o hosted_jit.o cpu.o dispatch.o)

.PHONY: all clean
all: $(BUILD_DIR)/jit_probe

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/jit_probe.o: $(TEST_DIR)/jit_probe.c | $(BUILD_DIR)
	$(AROS_CC) $(CPPFLAGS) $(CFLAGS) -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/hosted_jit.o: $(HOST_TEST_DIR)/hosted_jit.c | $(BUILD_DIR)
	$(AROS_CC) $(CPPFLAGS) $(CFLAGS) -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/cpu.o: $(KAFFEINE_DIR)/kaffeine_cpu.c | $(BUILD_DIR)
	$(AROS_CC) $(CPPFLAGS) $(CFLAGS) -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/dispatch.o: $(KAFFEINE_DIR)/kaffeine_dispatch.c | $(BUILD_DIR)
	$(AROS_CC) $(CPPFLAGS) $(CFLAGS) -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror -MMD -MP -c -o $@ $<

$(BUILD_DIR)/jit_probe: $(PROBE_OBJS)
	$(AROS_CC) $(CPPFLAGS) $(LDFLAGS) -o $@ $^ -lposixc

clean:
	rm -f $(PROBE_OBJS) $(PROBE_OBJS:.o=.d) $(BUILD_DIR)/jit_probe

-include $(PROBE_OBJS:.o=.d)
