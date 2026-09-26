#
# Copyright (C) 2026 OpenWrt
#
# This is free software, licensed under the GNU General Public License v2.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=sunxi-overclock
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/sunxi-overclock
  SUBMENU:=Allwinner A733 extras
  TITLE:=Optional A733 GPU/NPU clock helper
  DEPENDS:=@TARGET_allwinner
  FILES:=$(PKG_BUILD_DIR)/sunxi-overclock.ko
  KCONFIG:=
endef

define KernelPackage/sunxi-overclock/description
  Extra software (not in the default image). Optional helper that
  raises GPU/NPU clocks via clk + OPP, never raw MMIO.
  Default-off: not autoloaded. Enable with /etc/config/overclock and
  /etc/init.d/sunxi-overclock. Hard-capped at datasheet/DT maxima
  (NPU 1120 MHz, GPU 1008 MHz) unless rebuilt with
  SUNXI_OVERCLOCK_ALLOW_EXTREME.
  Extra heat and voltage; can reset or damage the board.
endef

define KernelPackage/sunxi-overclock/conffiles
/etc/config/overclock
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

define KernelPackage/sunxi-overclock/install
	$(INSTALL_DIR) $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_CONF) ./files/overclock.config $(1)/etc/config/overclock
	$(INSTALL_BIN) ./files/sunxi-overclock.init $(1)/etc/init.d/sunxi-overclock
endef

$(eval $(call KernelPackage,sunxi-overclock))
