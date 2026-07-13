CC       ?= gcc
CFLAGS   := -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE \
             -ffunction-sections -fdata-sections \
             -Isrc/app -Isrc/core -Isrc/platform -Isrc/protocol \
             -Isrc/transport -Isrc/vendor
LDFLAGS  := -Wl,--gc-sections

STATIC   := -static
BUILD    := build
TEST_BUILD := $(BUILD)/tests

COBS_SRCS   := src/vendor/nanocobs/cobs.c
COBS_HDRS   := src/vendor/nanocobs/cobs.h
CORE_SRCS   := src/core/util.c src/core/logging.c
CORE_HDRS   := src/core/util.h src/core/logging.h src/core/version.h
PLATFORM_SRCS := src/platform/tty.c src/platform/fd.c
PLATFORM_HDRS := src/platform/tty.h src/platform/fd.h
TRANSPORT_SRCS := src/transport/frame.c src/transport/link.c
TRANSPORT_HDRS := src/transport/frame.h src/transport/link.h
COMMON_SRCS := $(CORE_SRCS) $(PLATFORM_SRCS) $(TRANSPORT_SRCS)
COMMON_HDRS := $(CORE_HDRS) $(PLATFORM_HDRS) $(TRANSPORT_HDRS)
USB_SRCS    := src/platform/usb_discovery.c
USB_HDRS    := src/platform/usb_discovery.h
CONTROL_SRCS := src/protocol/control.c
CONTROL_HDRS := src/protocol/control.h src/protocol/control_proto.h
SERIALMUX_SRCS := src/protocol/serialmux.c
SERIALMUX_HDRS := src/protocol/serialmux.h src/protocol/serialmux_proto.h

PIK1D_SRCS  := src/app/pik1d.c $(SERIALMUX_SRCS) $(CONTROL_SRCS) $(USB_SRCS) $(COBS_SRCS) $(COMMON_SRCS)
PIK1D_HDRS  := $(SERIALMUX_HDRS) $(CONTROL_HDRS) $(USB_HDRS) $(COBS_HDRS) $(COMMON_HDRS)

TB_SRCS     := src/app/tcpbridge.c $(COBS_SRCS) $(COMMON_SRCS)
TB_HDRS     := src/protocol/tcpbridge_proto.h $(COBS_HDRS) $(COMMON_HDRS)

# ── Install ───────────────────────────────────────────────────────────────────
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

# ── Cross toolchains ─────────────────────────────────────────────────────────
TOOLCHAIN_DIR  := $(CURDIR)/.toolchain
MUSL_CC_BASE   := https://musl.cc

MIPSEL_TRIPLE  := mipsel-linux-musl
AARCH64_TRIPLE := aarch64-linux-musl
ARMV7_TRIPLE   := arm-linux-musleabihf

MIPSEL_CC    ?= $(TOOLCHAIN_DIR)/$(MIPSEL_TRIPLE)-cross/bin/$(MIPSEL_TRIPLE)-gcc
MIPSEL_STRIP ?= $(TOOLCHAIN_DIR)/$(MIPSEL_TRIPLE)-cross/bin/$(MIPSEL_TRIPLE)-strip
AARCH64_CC   ?= $(TOOLCHAIN_DIR)/$(AARCH64_TRIPLE)-cross/bin/$(AARCH64_TRIPLE)-gcc
AARCH64_STRIP ?= $(TOOLCHAIN_DIR)/$(AARCH64_TRIPLE)-cross/bin/$(AARCH64_TRIPLE)-strip
ARMV7_CC     ?= $(TOOLCHAIN_DIR)/$(ARMV7_TRIPLE)-cross/bin/$(ARMV7_TRIPLE)-gcc
ARMV7_STRIP  ?= $(TOOLCHAIN_DIR)/$(ARMV7_TRIPLE)-cross/bin/$(ARMV7_TRIPLE)-strip

.PHONY: all native mipsel aarch64 armv7 test test-unit test-cli \
        test-integration test-tcpbridge-integration toolchain clean distclean \
        render-k1-init install-k1 uninstall-k1 install-pi uninstall-pi FORCE

all: native

native: $(BUILD)/pik1d $(BUILD)/tcpbridge

test: test-unit test-cli test-integration

test-unit: $(TEST_BUILD)/test_util $(TEST_BUILD)/test_cobs $(TEST_BUILD)/test_frame \
      $(TEST_BUILD)/test_logging $(TEST_BUILD)/test_control_names \
      $(TEST_BUILD)/test_control_protocol $(TEST_BUILD)/test_pik1d_supervisor \
      $(TEST_BUILD)/test_serialmux_protocol
	$(TEST_BUILD)/test_util
	$(TEST_BUILD)/test_cobs
	$(TEST_BUILD)/test_frame
	$(TEST_BUILD)/test_logging
	$(TEST_BUILD)/test_control_names
	$(TEST_BUILD)/test_control_protocol
	$(TEST_BUILD)/test_pik1d_supervisor
	$(TEST_BUILD)/test_serialmux_protocol

test-cli: native
	PIK1D=$(BUILD)/pik1d TCPBRIDGE=$(BUILD)/tcpbridge bash tests/test_cli.sh

test-integration: test-tcpbridge-integration

test-tcpbridge-integration: native
	TCPBRIDGE=$(BUILD)/tcpbridge python3 tests/test_tcpbridge_integration.py

# ── Native builds ─────────────────────────────────────────────────────────────
$(BUILD)/pik1d: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PIK1D_SRCS)

$(BUILD)/tcpbridge: $(TB_SRCS) $(TB_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TB_SRCS)

$(TEST_BUILD)/test_util: tests/test_util.c src/core/util.c src/core/util.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_util.c src/core/util.c

$(TEST_BUILD)/test_frame: tests/test_frame.c src/transport/frame.c src/transport/frame.h $(COBS_SRCS) $(COBS_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_frame.c src/transport/frame.c $(COBS_SRCS)

$(TEST_BUILD)/test_cobs: tests/test_cobs.c $(COBS_SRCS) $(COBS_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_cobs.c $(COBS_SRCS)

$(TEST_BUILD)/test_logging: tests/test_logging.c src/core/logging.c src/core/logging.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_logging.c src/core/logging.c

$(TEST_BUILD)/test_control_names: tests/test_control_names.c src/protocol/control.c src/protocol/control.h $(COBS_SRCS) $(COMMON_SRCS) $(COMMON_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_control_names.c src/protocol/control.c $(COBS_SRCS) $(COMMON_SRCS)

$(TEST_BUILD)/test_control_protocol: tests/test_control_protocol.c tests/test_harness.h src/protocol/control.c src/protocol/control.h $(COBS_SRCS) $(COMMON_SRCS) $(COMMON_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_control_protocol.c src/protocol/control.c $(COBS_SRCS) $(COMMON_SRCS)

$(TEST_BUILD)/test_pik1d_supervisor: tests/test_pik1d_supervisor.c tests/test_harness.h src/app/pik1d.c src/protocol/control.c src/protocol/control.h $(COBS_SRCS) $(COMMON_SRCS) $(COMMON_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_pik1d_supervisor.c src/protocol/control.c $(COBS_SRCS) $(COMMON_SRCS)

$(TEST_BUILD)/test_serialmux_protocol: tests/test_serialmux_protocol.c tests/test_harness.h src/protocol/serialmux.c src/protocol/serialmux.h $(COBS_SRCS) $(COMMON_SRCS) $(COMMON_HDRS) | $(TEST_BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_serialmux_protocol.c src/protocol/serialmux.c $(COBS_SRCS) $(COMMON_SRCS)

# ── MIPSEL ────────────────────────────────────────────────────────────────────
mipsel: $(BUILD)/pik1d.mipsel $(BUILD)/tcpbridge.mipsel

$(BUILD)/pik1d.mipsel: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(MIPSEL_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(PIK1D_SRCS)
	-$(MIPSEL_STRIP) $@

$(BUILD)/tcpbridge.mipsel: $(TB_SRCS) $(TB_HDRS) | $(BUILD)
	$(MIPSEL_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(TB_SRCS)
	-$(MIPSEL_STRIP) $@

# ── AARCH64 ──────────────────────────────────────────────────────────────────
aarch64: $(BUILD)/pik1d.aarch64 $(BUILD)/tcpbridge.aarch64

$(BUILD)/pik1d.aarch64: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(AARCH64_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(PIK1D_SRCS)
	-$(AARCH64_STRIP) $@

$(BUILD)/tcpbridge.aarch64: $(TB_SRCS) $(TB_HDRS) | $(BUILD)
	$(AARCH64_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(TB_SRCS)
	-$(AARCH64_STRIP) $@

# ── ARMV7 ─────────────────────────────────────────────────────────────────────
armv7: $(BUILD)/pik1d.armv7 $(BUILD)/tcpbridge.armv7

$(BUILD)/pik1d.armv7: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(ARMV7_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(PIK1D_SRCS)
	-$(ARMV7_STRIP) $@

$(BUILD)/tcpbridge.armv7: $(TB_SRCS) $(TB_HDRS) | $(BUILD)
	$(ARMV7_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(TB_SRCS)
	-$(ARMV7_STRIP) $@

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

# ── Toolchain download ────────────────────────────────────────────────────────
define fetch_toolchain
$(TOOLCHAIN_DIR)/$(1)-cross/bin/$(1)-gcc:
	mkdir -p $(TOOLCHAIN_DIR)
	curl -fL --progress-bar $(MUSL_CC_BASE)/$(1)-cross.tgz | tar -xz -C $(TOOLCHAIN_DIR)
endef
$(eval $(call fetch_toolchain,$(MIPSEL_TRIPLE)))
$(eval $(call fetch_toolchain,$(AARCH64_TRIPLE)))
$(eval $(call fetch_toolchain,$(ARMV7_TRIPLE)))

toolchain: \
	$(TOOLCHAIN_DIR)/$(MIPSEL_TRIPLE)-cross/bin/$(MIPSEL_TRIPLE)-gcc \
	$(TOOLCHAIN_DIR)/$(AARCH64_TRIPLE)-cross/bin/$(AARCH64_TRIPLE)-gcc \
	$(TOOLCHAIN_DIR)/$(ARMV7_TRIPLE)-cross/bin/$(ARMV7_TRIPLE)-gcc

# ── Install targets ───────────────────────────────────────────────────────────
install-k1: $(BUILD)/S99pik1 $(BUILD)/shutdown_command.sh
	@test -x $(BUILD)/pik1d.mipsel || \
		{ echo "Missing $(BUILD)/pik1d.mipsel; run 'make mipsel' first"; exit 1; }
	@test -x $(BUILD)/tcpbridge.mipsel || \
		{ echo "Missing $(BUILD)/tcpbridge.mipsel; run 'make mipsel' first"; exit 1; }
	install -d $(K1_DIR)
	install -m 755 $(BUILD)/pik1d.mipsel    $(K1_DIR)/pik1d
	install -m 755 $(BUILD)/tcpbridge.mipsel $(K1_DIR)/tcpbridge
	install -m 755 $(BUILD)/shutdown_command.sh $(K1_DIR)/shutdown_command.sh
	install -m 755 $(BUILD)/S99pik1 $(K1_INIT_DIR)/S99pik1
	@for svc in $(K1_DISABLE_SVCS); do \
		if [ -f $(K1_INIT_DIR)/$$svc ]; then \
			echo "Disabling $$svc"; \
			mv $(K1_INIT_DIR)/$$svc $(K1_INIT_DIR)/_$$svc; \
		fi; \
	done

uninstall-k1:
	rm -f $(K1_INIT_DIR)/S99pik1
	rm -f $(K1_DIR)/pik1d $(K1_DIR)/tcpbridge $(K1_DIR)/shutdown_command.sh
	@for svc in $(sort $(K1_DISABLE_SVCS) $(K1_SCREEN_SVCS)); do \
		if [ -f $(K1_INIT_DIR)/_$$svc ]; then \
			echo "Restoring $$svc"; \
			mv $(K1_INIT_DIR)/_$$svc $(K1_INIT_DIR)/$$svc; \
		fi; \
	done

install-pi:
	@test -x $(BUILD)/pik1d.aarch64 || \
		{ echo "Missing $(BUILD)/pik1d.aarch64; run 'make aarch64' first"; exit 1; }
	@test -x $(BUILD)/tcpbridge.aarch64 || \
		{ echo "Missing $(BUILD)/tcpbridge.aarch64; run 'make aarch64' first"; exit 1; }
	$(SUDO) install -d $(PI_DIR)
	$(SUDO) install -m 755 $(BUILD)/pik1d.aarch64     $(PI_DIR)/pik1d
	$(SUDO) install -m 755 $(BUILD)/tcpbridge.aarch64  $(PI_DIR)/tcpbridge
	$(SUDO) install -m 755 files/pi/setup_otg.sh $(PI_DIR)/setup_otg.sh
	sed -e 's|@INSTALL_DIR@|$(PI_DIR)|g' -e 's|@TCP_SPEC@|$(PI_TCP_SPEC)|g' \
		files/pi/pik1.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1.service > /dev/null
	sed 's|@INSTALL_DIR@|$(PI_DIR)|g' files/pi/pik1-peer-reboot.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1-peer-reboot.service > /dev/null
	sed 's|@INSTALL_DIR@|$(PI_DIR)|g' files/pi/pik1-peer-poweroff.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1-peer-poweroff.service > /dev/null
	sed "s|@PIK1_USER@|$$(id -nu 1000)|g" files/pi/pik1-polkit.rules.in | \
		$(SUDO) tee /etc/polkit-1/rules.d/49-pik1.rules > /dev/null
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
		$(PI_SYSTEMD_DIR)/pik1-peer-poweroff.service \
		/etc/polkit-1/rules.d/49-pik1.rules
	$(SUDO) systemctl daemon-reload
	$(SUDO) rm -rf $(PI_DIR)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(TOOLCHAIN_DIR)
