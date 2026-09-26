# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2013-2016 OpenWrt.org

# Optional A733 packages. Do not add these to DEVICE_PACKAGES.
# Default firmware: not installed. CI builds NPU/overclock as extra .ipk (=m).
ALLWINNER_A733_MENU:=Allwinner A733 extras

define KernelPackage/rtc-sunxi
    SUBMENU:=$(OTHER_MENU)
    TITLE:=Sunxi SoC built-in RTC support
    DEPENDS:=@(TARGET_sunxi&&RTC_SUPPORT)
    KCONFIG:= \
	CONFIG_RTC_DRV_SUNXI \
	CONFIG_RTC_CLASS=y
    FILES:=$(LINUX_DIR)/drivers/rtc/rtc-sunxi.ko
    AUTOLOAD:=$(call AutoLoad,50,rtc-sunxi)
endef

define KernelPackage/rtc-sunxi/description
 Support for the AllWinner sunXi SoC's onboard RTC
endef

$(eval $(call KernelPackage,rtc-sunxi))

define KernelPackage/sunxi-ir
    SUBMENU:=$(OTHER_MENU)
    TITLE:=Sunxi SoC built-in IR support
    DEPENDS:=@(TARGET_sunxi&&RTC_SUPPORT) +kmod-input-core
    KCONFIG:= \
	CONFIG_MEDIA_SUPPORT=y \
	CONFIG_MEDIA_RC_SUPPORT=y \
	CONFIG_RC_DEVICES=y \
	CONFIG_RC_CORE=y \
	CONFIG_IR_SUNXI
    FILES:=$(LINUX_DIR)/drivers/media/rc/sunxi-cir.ko
    AUTOLOAD:=$(call AutoLoad,50,sunxi-cir)
endef

define KernelPackage/sunxi-ir/description
 Support for the AllWinner sunXi SoC's onboard IR
endef

$(eval $(call KernelPackage,sunxi-ir))

define KernelPackage/ata-sunxi
    TITLE:=AllWinner sunXi AHCI SATA support
    SUBMENU:=$(BLOCK_MENU)
    DEPENDS:=@TARGET_sunxi +kmod-ata-ahci-platform +kmod-scsi-core
    KCONFIG:=CONFIG_AHCI_SUNXI
    FILES:=$(LINUX_DIR)/drivers/ata/ahci_sunxi.ko
    AUTOLOAD:=$(call AutoLoad,41,ahci_sunxi,1)
endef

define KernelPackage/ata-sunxi/description
 SATA support for the AllWinner sunXi SoC's onboard AHCI SATA
endef

$(eval $(call KernelPackage,ata-sunxi))

define KernelPackage/sun4i-emac
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=AllWinner EMAC Ethernet support
  DEPENDS:=@TARGET_sunxi +kmod-of-mdio +kmod-libphy
  KCONFIG:=CONFIG_SUN4I_EMAC
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/allwinner/sun4i-emac.ko
  AUTOLOAD:=$(call AutoProbe,sun4i-emac)
endef

$(eval $(call KernelPackage,sun4i-emac))

define KernelPackage/sound-soc-sunxi
  TITLE:=AllWinner built-in SoC sound support
  KCONFIG:=CONFIG_SND_SUN4I_CODEC
  FILES:=$(LINUX_DIR)/sound/soc/sunxi/sun4i-codec.ko
  AUTOLOAD:=$(call AutoLoad,65,sun4i-codec)
  DEPENDS:=@TARGET_sunxi +kmod-sound-soc-core
  $(call AddDepends/sound)
endef

define KernelPackage/sound-soc-sunxi/description
  Kernel support for AllWinner built-in SoC audio
endef

$(eval $(call KernelPackage,sound-soc-sunxi))

define KernelPackage/sound-soc-sunxi-spdif
  TITLE:=Allwinner A10 SPDIF Support
  KCONFIG:=CONFIG_SND_SUN4I_SPDIF
  FILES:=$(LINUX_DIR)/sound/soc/sunxi/sun4i-spdif.ko
  AUTOLOAD:=$(call AutoLoad,65,sun4i-spdif)
  DEPENDS:=@TARGET_sunxi +kmod-sound-soc-spdif
  $(call AddDepends/sound)
endef

define KernelPackage/sound-soc-sunxi-spdif/description
  Kernel support for Allwinner A10 SPDIF Support
endef

$(eval $(call KernelPackage,sound-soc-sunxi-spdif))

define KernelPackage/aw-nna-galcore
  SUBMENU:=$(ALLWINNER_A733_MENU)
  TITLE:=Allwinner Vivante NPU (galcore / unified)
  DEPENDS:=@TARGET_allwinner
  KCONFIG:=CONFIG_AW_NNA_GALCORE
  FILES:=$(LINUX_DIR)/bsp/drivers/npu/aw_nna_galcore/galcore.ko
  AUTOLOAD:=$(call AutoProbe,galcore)
  CONFLICTS:=kmod-aw-nna-vip
endef

define KernelPackage/aw-nna-galcore/description
  Extra software (not in the default image). In-tree Vivante galcore
  driver for the A733 NPU (VIP9000). Creates /dev/galcore. Kernel ABI
  is 6.4.18.6.904649 and must match userspace exactly; public TIM-VX
  builds are often 6.4.15.x and will not work. Vendor userspace is
  glibc; musl images will not run it.
  Mutually exclusive with kmod-aw-nna-vip.
endef

$(eval $(call KernelPackage,aw-nna-galcore))

define KernelPackage/aw-nna-vip
  SUBMENU:=$(ALLWINNER_A733_MENU)
  TITLE:=Allwinner NPU VIPLite 2 (vipcore)
  DEPENDS:=@TARGET_allwinner
  KCONFIG:= \
	CONFIG_AW_NNA_VIP \
	CONFIG_NNA_VIP2=y \
	CONFIG_NNA_VIP1=n
  FILES:=$(LINUX_DIR)/bsp/drivers/npu/aw_nna_vip/vip2/vipcore.ko
  AUTOLOAD:=$(call AutoProbe,vipcore)
  CONFLICTS:=kmod-aw-nna-galcore
endef

define KernelPackage/aw-nna-vip/description
  Extra software (not in the default image). In-tree VIPLite 2.0.3
  driver for the A733 NPU. Creates /dev/vipcore. Preferred OpenWrt
  inference stack versus galcore. Userspace must match VIPLite 2.0.3;
  vendor libs are glibc.
  Mutually exclusive with kmod-aw-nna-galcore (same DT node).
endef

$(eval $(call KernelPackage,aw-nna-vip))

define KernelPackage/drm-powervr
  SUBMENU:=$(ALLWINNER_A733_MENU)
  TITLE:=Imagination PowerVR BXM (drm/imagination backport)
  DEPENDS:=@TARGET_allwinner +powervr-firmware
  KCONFIG:=CONFIG_DRM_POWERVR
  FILES:=$(LINUX_DIR)/drivers/gpu/drm/imagination/powervr.ko
  AUTOLOAD:=$(call AutoProbe,powervr)
  MODPARAMS.powervr:=exp_hw_support=1
endef

define KernelPackage/drm-powervr/description
  Extra software (not in the default image). Backport of upstream
  drm/imagination (Linux 6.18+) to this 6.6 tree.
  Binds the A733 BXM-4-64 GPU (compatible img,img-rogue) and creates a
  render node. Needs firmware powervr/rogue_36.56.104.183_v1.fw.
  A733 BVNC 36.56.104.183 is experimental; exp_hw_support=1 is set.
  Mesa pvr userspace is a separate, typically glibc, problem.
endef

$(eval $(call KernelPackage,drm-powervr))
