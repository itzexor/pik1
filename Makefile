CC       ?= gcc
CFLAGS   := -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE \
             -ffunction-sections -fdata-sections
LDFLAGS  := -Wl,--gc-sections
SRCS     := src/muxd.c src/cobs.c
HDRS     := src/cobs.h

STATIC   := -static
BUILD    := build

# ── Install ───────────────────────────────────────────────────────────────────
# Pi install commands that touch system paths run under $(SUDO).
# Default: sudo (prompts as needed). Override with SUDO= if already root.
# K1 needs no SUDO — target system runs entirely as root.
SUDO            ?= sudo

# K1: fixed install path (embedded target)
K1_DIR          := /usr/data/pik1
K1_INIT_DIR     := /etc/init.d
# Services disabled in the installed state (S* -> _S*).
# install-k1 disables them; uninstall-k1 restores them.
K1_DISABLE_SVCS := S50nginx_service S50unslung S50webcam \
                   S55klipper_mcu S55klipper_service \
                   S56moonraker_service S99guppyscreen

# Pi: overridable
PI_DIR          ?= /opt/pik1
PI_SYSTEMD_DIR  ?= /etc/systemd/system

# ── Cross toolchains ─────────────────────────────────────────────────────────
# 'make toolchain' downloads musl.cc prebuilts into .toolchain/.
# Override any *_CC / *_STRIP on the command line to use a different compiler.
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

native: $(BUILD)/serialmux

$(BUILD)/serialmux: $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

mipsel: $(BUILD)/serialmux.mipsel
$(BUILD)/serialmux.mipsel: $(SRCS) $(HDRS) | $(BUILD)
	$(MIPSEL_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(SRCS)
	-$(MIPSEL_STRIP) $@

aarch64: $(BUILD)/serialmux.aarch64
$(BUILD)/serialmux.aarch64: $(SRCS) $(HDRS) | $(BUILD)
	$(AARCH64_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(SRCS)
	-$(AARCH64_STRIP) $@

armv7: $(BUILD)/serialmux.armv7
$(BUILD)/serialmux.armv7: $(SRCS) $(HDRS) | $(BUILD)
	$(ARMV7_CC) $(CFLAGS) $(LDFLAGS) $(STATIC) -o $@ $(SRCS)
	-$(ARMV7_STRIP) $@

$(BUILD):
	mkdir -p $@

# Download toolchain tarballs from musl.cc into .toolchain/
# (GNU Make pattern rules only allow one %, so these are explicit)
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

install-k1: $(BUILD)/serialmux.mipsel
	install -d $(K1_DIR)
	install -m 755 $(BUILD)/serialmux.mipsel $(K1_DIR)/serialmux
	install -m 755 S99pik1 $(K1_INIT_DIR)/S99pik1
	@for svc in $(K1_DISABLE_SVCS); do \
		if [ -f $(K1_INIT_DIR)/$$svc ]; then \
			echo "Disabling $$svc"; \
			mv $(K1_INIT_DIR)/$$svc $(K1_INIT_DIR)/_$$svc; \
		fi; \
	done

uninstall-k1:
	rm -f $(K1_INIT_DIR)/S99pik1
	rm -f $(K1_DIR)/serialmux
	@for svc in $(K1_DISABLE_SVCS); do \
		if [ -f $(K1_INIT_DIR)/_$$svc ]; then \
			echo "Restoring $$svc"; \
			mv $(K1_INIT_DIR)/_$$svc $(K1_INIT_DIR)/$$svc; \
		fi; \
	done

install-pi: $(BUILD)/serialmux.aarch64
	$(SUDO) install -d $(PI_DIR)
	$(SUDO) install -m 755 $(BUILD)/serialmux.aarch64 $(PI_DIR)/serialmux
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
