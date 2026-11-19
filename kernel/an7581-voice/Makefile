# SPDX-License-Identifier: GPL-2.0-only
#
# AN7581 PCM/ZSI/SLIC/FXS source package.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=an7581-voice
PKG_RELEASE:=8
PKG_LICENSE:=GPL-2.0-only

PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/an7581-voice
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

PKG_VERSION:=

define KernelPackage/an7581-pcm
  SUBMENU:=Voice over IP
  TITLE:=Airoha AN7581 PCM engine
  DEPENDS:=@TARGET_airoha_an7581
  FILES:=$(PKG_BUILD_DIR)/an7581-pcm.ko
  AUTOLOAD:=
endef

define KernelPackage/an7581-pcm/description
 AN7581 PCM engine with the PCM clock/slot bring-up, OSLEC line echo
 cancellation and a /proc/an7581-pcm diagnostic interface.
endef

define KernelPackage/an7581-zsi
  SUBMENU:=Voice over IP
  TITLE:=Airoha AN7581 ZSI transport
  DEPENDS:=@TARGET_airoha_an7581 +kmod-an7581-pcm
  FILES:=$(PKG_BUILD_DIR)/an7581-zsi.ko
  AUTOLOAD:=
endef

define KernelPackage/an7581-zsi/description
 AN7581 ZSI transport for Zarlink SLICs.  The PCM clock/slot setup is owned
 by kmod-an7581-pcm; this module only configures the ZSI wrapper and reset.
endef

define KernelPackage/an7581-slic
  SUBMENU:=Voice over IP
  TITLE:=Airoha AN7581 SLIC
  DEPENDS:=@TARGET_airoha_an7581 +kmod-fxs-core +kmod-an7581-zsi
  FILES:=$(PKG_BUILD_DIR)/an7581-slic.ko
  AUTOLOAD:=
endef

define KernelPackage/an7581-slic/description
 Single AN7581 SLIC module.  Zarlink Le9641 is supported; Le9642 and
 PEF32001 backend entries are reserved.  Exposes /proc/an7581-slic.
endef

define KernelPackage/an7581-fxs
  SUBMENU:=Voice over IP
  TITLE:=AN7581 FXS line provider
  DEPENDS:=@TARGET_airoha_an7581 +kmod-fxs-core +kmod-an7581-pcm \
	+kmod-an7581-slic
  FILES:=$(PKG_BUILD_DIR)/an7581-fxs.ko
  AUTOLOAD:=
endef

define KernelPackage/an7581-fxs/description
 Binds the AN7581 PCM engine and the selected SLIC backend to the generic
 fxs-core line.  Creates /dev/fxs0 and /dev/fxs0-pcm.
endef

define KernelPackage/an7581-fxs/install
	$(INSTALL_DIR) $(1)/etc/modules.d
	echo an7581-fxs > $(1)/etc/modules.d/an7581-fxs
endef

define Build/Prepare
	rm -rf $(PKG_BUILD_DIR)
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,an7581-pcm))
$(eval $(call KernelPackage,an7581-zsi))
$(eval $(call KernelPackage,an7581-slic))
$(eval $(call KernelPackage,an7581-fxs))
