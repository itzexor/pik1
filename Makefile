CC       ?= gcc
CFLAGS   := -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE \
             -ffunction-sections -fdata-sections \
             -Isrc/app -Isrc/common -Isrc/link -Isrc/services \
             -Isrc/io -Isrc/vendor
LDFLAGS  := -Wl,--gc-sections

STATIC   := -static
BUILD    := build
TEST_BUILD := $(BUILD)/tests

COBS_SRCS   := src/vendor/nanocobs/cobs.c
COBS_HDRS   := src/vendor/nanocobs/cobs.h
COMMON_SRCS := src/common/util.c src/common/logging.c
COMMON_HDRS := src/common/util.h src/common/logging.h src/common/product.h
LINK_SRCS   := src/link/frame.c src/link/link.c src/link/session.c
LINK_HDRS   := src/link/frame.h src/link/link.h src/link/session.h \
               src/link/pik_proto.h
SERVICE_SRCS := src/services/control.c src/services/serialmux.c \
                src/services/tunnel.c
SERVICE_HDRS := src/services/control.h src/services/serialmux.h \
                src/services/tunnel.h
IO_SRCS := src/io/fd.c src/io/usb_host.c src/io/usb_gadget.c \
           src/io/usb_gadget_configfs.c
IO_HDRS := src/io/fd.h src/io/usb.h
APP_SRCS := src/app/pik1d.c src/app/commands.c
APP_HDRS := src/app/commands.h

BASE_SRCS   := $(COMMON_SRCS) $(LINK_SRCS) $(IO_SRCS)
BASE_HDRS   := $(COMMON_HDRS) $(LINK_HDRS) $(IO_HDRS)
PIK1D_SRCS  := $(APP_SRCS) $(SERVICE_SRCS) $(COBS_SRCS) $(BASE_SRCS)
PIK1D_HDRS  := $(APP_HDRS) $(SERVICE_HDRS) $(COBS_HDRS) $(BASE_HDRS)
UTILITY_SCRIPTS := scripts/wifi-reset.sh

# Install
SUDO            ?= sudo

K1_DIR          ?= /usr/data/pik1
K1_INIT_DIR     := /etc/init.d
K1_DISABLE_SVCS := S50nginx_service S50unslung S50webcam \
                   S55klipper_mcu S55klipper_service \
                   S56moonraker_service
K1_SCREEN_SVCS  := S99guppyscreen S99grumpyscreen

# Screen support. The K1 screen UIs reach Moonraker on the Pi through the
# TCP tunnel, so the tunnel and the screen services travel together: pass
# SCREEN=0 (to install on both ends) to disable the screen services on the
# K1 and omit the TCP tunnel channel from both daemons.
SCREEN ?= 1
ifeq ($(SCREEN),0)
K1_DISABLE_SVCS += $(K1_SCREEN_SVCS)
K1_TCP_SPEC     :=
PI_TCP_SPEC     :=
else
K1_TCP_SPEC     := listen:$$TCP_ADDR:$$TCP_PORT
PI_TCP_SPEC     := forward:127.0.0.1:7125
endif

PI_DIR          ?= /opt/pik1
PI_SYSTEMD_DIR  ?= /etc/systemd/system

# Cross toolchains
TOOLCHAIN_DIR  := $(CURDIR)/.toolchain
MUSL_CC_BASE   := https://musl.cc

MIPSEL_TRIPLE  := mipsel-linux-musl
AARCH64_TRIPLE := aarch64-linux-musl

MIPSEL_CC    ?= $(TOOLCHAIN_DIR)/$(MIPSEL_TRIPLE)-cross/bin/$(MIPSEL_TRIPLE)-gcc
MIPSEL_STRIP ?= $(TOOLCHAIN_DIR)/$(MIPSEL_TRIPLE)-cross/bin/$(MIPSEL_TRIPLE)-strip
AARCH64_CC   ?= $(TOOLCHAIN_DIR)/$(AARCH64_TRIPLE)-cross/bin/$(AARCH64_TRIPLE)-gcc
AARCH64_STRIP ?= $(TOOLCHAIN_DIR)/$(AARCH64_TRIPLE)-cross/bin/$(AARCH64_TRIPLE)-strip

.PHONY: all native mipsel aarch64 test test-unit test-cli test-scripts \
        toolchain clean distclean \
        render-k1-init install-k1 uninstall-k1 install-pi uninstall-pi FORCE

all: native

native: $(BUILD)/pik1d

test: test-unit test-cli test-scripts

test-unit: $(TEST_BUILD)/test_util $(TEST_BUILD)/test_cobs $(TEST_BUILD)/test_frame \
      $(TEST_BUILD)/test_logging $(TEST_BUILD)/test_control_names \
      $(TEST_BUILD)/test_control_protocol $(TEST_BUILD)/test_commands \
      $(TEST_BUILD)/test_serialmux_protocol $(TEST_BUILD)/test_tunnel_protocol
	$(TEST_BUILD)/test_util
	$(TEST_BUILD)/test_cobs
	$(TEST_BUILD)/test_frame
	$(TEST_BUILD)/test_logging
	$(TEST_BUILD)/test_control_names
	$(TEST_BUILD)/test_control_protocol
	PIK1_CONTROL_SOCK=$(abspath $(TEST_BUILD))/commands.sock $(TEST_BUILD)/test_commands
	$(TEST_BUILD)/test_serialmux_protocol
	$(TEST_BUILD)/test_tunnel_protocol

test-cli: native
	PIK1D=$(BUILD)/pik1d bash tests/test_cli.sh

test-scripts:
	bash tests/test_wifi_reset.sh

# Native builds
$(BUILD)/pik1d: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PIK1D_SRCS)

$(TEST_BUILD)/test_util: tests/test_util.c src/common/util.c src/common/util.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_util.c src/common/util.c

$(TEST_BUILD)/test_frame: tests/test_frame.c src/link/frame.c src/link/frame.h $(COBS_SRCS) $(COBS_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_frame.c src/link/frame.c $(COBS_SRCS)

$(TEST_BUILD)/test_cobs: tests/test_cobs.c $(COBS_SRCS) $(COBS_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_cobs.c $(COBS_SRCS)

$(TEST_BUILD)/test_logging: tests/test_logging.c src/common/logging.c src/common/logging.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_logging.c src/common/logging.c

$(TEST_BUILD)/test_control_names: tests/test_control_names.c $(SERVICE_SRCS) $(SERVICE_HDRS) $(COBS_SRCS) $(BASE_SRCS) $(BASE_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_control_names.c $(SERVICE_SRCS) $(COBS_SRCS) $(BASE_SRCS)

$(TEST_BUILD)/test_control_protocol: tests/test_control_protocol.c tests/test_harness.h tests/test_session_harness.h $(SERVICE_SRCS) $(SERVICE_HDRS) $(COBS_SRCS) $(BASE_SRCS) $(BASE_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_control_protocol.c $(SERVICE_SRCS) $(COBS_SRCS) $(BASE_SRCS)

$(TEST_BUILD)/test_commands: tests/test_commands.c tests/test_harness.h src/app/commands.c src/app/commands.h src/common/logging.c src/common/logging.h src/io/fd.c src/io/fd.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_commands.c src/app/commands.c src/common/logging.c src/io/fd.c

$(TEST_BUILD)/test_serialmux_protocol: tests/test_serialmux_protocol.c tests/test_harness.h tests/test_session_harness.h $(SERVICE_SRCS) $(SERVICE_HDRS) $(COBS_SRCS) $(BASE_SRCS) $(BASE_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_serialmux_protocol.c $(SERVICE_SRCS) $(COBS_SRCS) $(BASE_SRCS)

$(TEST_BUILD)/test_tunnel_protocol: tests/test_tunnel_protocol.c tests/test_harness.h tests/test_session_harness.h $(SERVICE_SRCS) $(SERVICE_HDRS) $(COBS_SRCS) $(BASE_SRCS) $(BASE_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_tunnel_protocol.c $(SERVICE_SRCS) $(COBS_SRCS) $(BASE_SRCS)

# MIPSEL
mipsel: $(BUILD)/pik1d.mipsel

$(BUILD)/pik1d.mipsel: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(MIPSEL_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(PIK1D_SRCS)
	-$(MIPSEL_STRIP) $@

# AARCH64
aarch64: $(BUILD)/pik1d.aarch64

$(BUILD)/pik1d.aarch64: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(AARCH64_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(PIK1D_SRCS)
	-$(AARCH64_STRIP) $@

$(BUILD):
	mkdir -p $@

$(TEST_BUILD):
	mkdir -p $@

$(BUILD)/S99pik1: files/k1/S99pik1.in FORCE | $(BUILD)
	sed -e 's|@INSTALL_DIR@|$(K1_DIR)|g' -e 's|@TCP_SPEC@|$(K1_TCP_SPEC)|g' $< > $@

$(BUILD)/shutdown_command.sh: files/k1/shutdown_command.sh.in FORCE | $(BUILD)
	sed 's|@INSTALL_DIR@|$(K1_DIR)|g' $< > $@

FORCE:

render-k1-init: $(BUILD)/S99pik1 $(BUILD)/shutdown_command.sh

# Toolchain download
define fetch_toolchain
$(TOOLCHAIN_DIR)/$(1)-cross/bin/$(1)-gcc:
	mkdir -p $(TOOLCHAIN_DIR)
	curl -fL --progress-bar $(MUSL_CC_BASE)/$(1)-cross.tgz | tar -xz -C $(TOOLCHAIN_DIR)
endef
$(eval $(call fetch_toolchain,$(MIPSEL_TRIPLE)))
$(eval $(call fetch_toolchain,$(AARCH64_TRIPLE)))

toolchain: \
	$(TOOLCHAIN_DIR)/$(MIPSEL_TRIPLE)-cross/bin/$(MIPSEL_TRIPLE)-gcc \
	$(TOOLCHAIN_DIR)/$(AARCH64_TRIPLE)-cross/bin/$(AARCH64_TRIPLE)-gcc

# Install targets
install-k1: $(BUILD)/S99pik1 $(BUILD)/shutdown_command.sh $(UTILITY_SCRIPTS)
	@test -x $(BUILD)/pik1d.mipsel || \
		{ echo "Missing $(BUILD)/pik1d.mipsel; run 'make mipsel' first"; exit 1; }
	install -d $(K1_DIR) $(K1_DIR)/scripts
	install -m 755 $(BUILD)/pik1d.mipsel    $(K1_DIR)/pik1d
	install -m 755 $(BUILD)/shutdown_command.sh $(K1_DIR)/shutdown_command.sh
	install -m 755 $(UTILITY_SCRIPTS) $(K1_DIR)/scripts/
	install -m 755 $(BUILD)/S99pik1 $(K1_INIT_DIR)/S99pik1
	@for svc in $(K1_DISABLE_SVCS); do \
		if [ -f $(K1_INIT_DIR)/$$svc ]; then \
			echo "Disabling $$svc"; \
			mv $(K1_INIT_DIR)/$$svc $(K1_INIT_DIR)/_$$svc; \
		fi; \
	done

uninstall-k1:
	rm -f $(K1_INIT_DIR)/S99pik1
	rm -f $(K1_DIR)/pik1d $(K1_DIR)/shutdown_command.sh
	rm -f $(K1_DIR)/scripts/wifi-reset.sh
	-rmdir $(K1_DIR)/scripts
	@for svc in $(sort $(K1_DISABLE_SVCS) $(K1_SCREEN_SVCS)); do \
		if [ -f $(K1_INIT_DIR)/_$$svc ]; then \
			echo "Restoring $$svc"; \
			mv $(K1_INIT_DIR)/_$$svc $(K1_INIT_DIR)/$$svc; \
		fi; \
	done

install-pi: $(UTILITY_SCRIPTS)
	@test -x $(BUILD)/pik1d.aarch64 || \
		{ echo "Missing $(BUILD)/pik1d.aarch64; run 'make aarch64' first"; exit 1; }
	$(SUDO) install -d $(PI_DIR) $(PI_DIR)/scripts
	$(SUDO) install -m 755 $(BUILD)/pik1d.aarch64     $(PI_DIR)/pik1d
	$(SUDO) install -m 755 $(UTILITY_SCRIPTS) $(PI_DIR)/scripts/
	sed -e 's|@INSTALL_DIR@|$(PI_DIR)|g' -e 's|@TCP_SPEC@|$(PI_TCP_SPEC)|g' \
		files/pi/pik1.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1.service > /dev/null
	sed 's|@INSTALL_DIR@|$(PI_DIR)|g' files/pi/pik1-peer-reboot.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1-peer-reboot.service > /dev/null
	sed 's|@INSTALL_DIR@|$(PI_DIR)|g' files/pi/pik1-peer-poweroff.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1-peer-poweroff.service > /dev/null
	$(SUDO) systemctl daemon-reload
	$(SUDO) systemctl enable pik1.service
	$(SUDO) systemctl enable pik1-peer-reboot.service
	$(SUDO) systemctl enable pik1-peer-poweroff.service

uninstall-pi:
	-$(SUDO) systemctl disable pik1.service
	-$(SUDO) systemctl disable pik1-peer-reboot.service
	-$(SUDO) systemctl disable pik1-peer-poweroff.service
	$(SUDO) rm -f $(PI_SYSTEMD_DIR)/pik1.service \
		$(PI_SYSTEMD_DIR)/pik1-peer-reboot.service \
		$(PI_SYSTEMD_DIR)/pik1-peer-poweroff.service
	$(SUDO) systemctl daemon-reload
	$(SUDO) rm -rf $(PI_DIR)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(TOOLCHAIN_DIR)
