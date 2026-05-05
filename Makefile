CC       ?= gcc
CFLAGS   := -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE \
             -ffunction-sections -fdata-sections -Isrc/nanocobs
LDFLAGS  := -Wl,--gc-sections

STATIC   := -static
BUILD    := build

COBS_SRCS   := src/nanocobs/cobs.c
COBS_HDRS   := src/nanocobs/cobs.h

PIK1D_SRCS  := src/pik1d.c src/serialmux.c $(COBS_SRCS)
PIK1D_HDRS  := src/serialmux.h $(COBS_HDRS)

TB_SRCS     := src/tcpbridge.c $(COBS_SRCS)
TB_HDRS     := $(COBS_HDRS)

# ── Install ───────────────────────────────────────────────────────────────────
SUDO            ?= sudo

K1_DIR          := /usr/data/pik1
K1_INIT_DIR     := /etc/init.d
K1_DISABLE_SVCS := S50nginx_service S50unslung S50webcam \
                   S55klipper_mcu S55klipper_service \
                   S56moonraker_service S99guppyscreen

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

.PHONY: all native mipsel aarch64 armv7 toolchain clean distclean \
        install-k1 uninstall-k1 install-pi uninstall-pi

all: native

native: $(BUILD)/pik1d $(BUILD)/tcpbridge

# ── Native builds ─────────────────────────────────────────────────────────────
$(BUILD)/pik1d: $(PIK1D_SRCS) $(PIK1D_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PIK1D_SRCS)

$(BUILD)/tcpbridge: $(TB_SRCS) $(TB_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TB_SRCS)

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
install-k1: $(BUILD)/pik1d.mipsel $(BUILD)/tcpbridge.mipsel
	install -d $(K1_DIR)
	install -m 755 $(BUILD)/pik1d.mipsel    $(K1_DIR)/pik1d
	install -m 755 $(BUILD)/tcpbridge.mipsel $(K1_DIR)/tcpbridge
	install -m 755 S99pik1 $(K1_INIT_DIR)/S99pik1
	@for svc in $(K1_DISABLE_SVCS); do \
		if [ -f $(K1_INIT_DIR)/$$svc ]; then \
			echo "Disabling $$svc"; \
			mv $(K1_INIT_DIR)/$$svc $(K1_INIT_DIR)/_$$svc; \
		fi; \
	done

uninstall-k1:
	rm -f $(K1_INIT_DIR)/S99pik1
	rm -f $(K1_DIR)/pik1d $(K1_DIR)/tcpbridge
	@for svc in $(K1_DISABLE_SVCS); do \
		if [ -f $(K1_INIT_DIR)/_$$svc ]; then \
			echo "Restoring $$svc"; \
			mv $(K1_INIT_DIR)/_$$svc $(K1_INIT_DIR)/$$svc; \
		fi; \
	done

install-pi: $(BUILD)/pik1d.aarch64 $(BUILD)/tcpbridge.aarch64
	$(SUDO) install -d $(PI_DIR)
	$(SUDO) install -m 755 $(BUILD)/pik1d.aarch64     $(PI_DIR)/pik1d
	$(SUDO) install -m 755 $(BUILD)/tcpbridge.aarch64  $(PI_DIR)/tcpbridge
	$(SUDO) install -m 755 setup_pik1.sh $(PI_DIR)/setup_pik1.sh
	sed 's|@INSTALL_DIR@|$(PI_DIR)|g' pik1.service.in | \
		$(SUDO) tee $(PI_SYSTEMD_DIR)/pik1.service > /dev/null
	$(SUDO) systemctl daemon-reload
	$(SUDO) systemctl enable pik1.service

uninstall-pi:
	-$(SUDO) systemctl disable pik1.service
	$(SUDO) rm -f $(PI_SYSTEMD_DIR)/pik1.service
	$(SUDO) systemctl daemon-reload
	$(SUDO) rm -rf $(PI_DIR)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(TOOLCHAIN_DIR)
