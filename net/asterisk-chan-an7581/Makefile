include $(TOPDIR)/rules.mk

PKG_NAME:=asterisk-chan-an7581
PKG_RELEASE:=11
PKG_LICENSE:=GPL-2.0

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/asterisk-chan-an7581
  SUBMENU:=Telephony
  SECTION:=net
  CATEGORY:=Network
  DEPENDS:=+asterisk +kmod-an7581-zsi +kmod-an7581-pcm +kmod-an7581-slic-le9641
  TITLE:=AN7581 Le9641 FXS channel driver
endef

define Package/asterisk-chan-an7581/description
 Asterisk channel driver for the AN7581 PCM/ZSI Le9641 FXS line.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) -o $(PKG_BUILD_DIR)/chan_an7581.o -c $(PKG_BUILD_DIR)/chan_an7581.c \
		-MD -MT chan_an7581.o -MF $(PKG_BUILD_DIR)/.chan_an7581.o.d -MP -pthread \
		$(TARGET_CFLAGS) -DAST_MODULE_SELF_SYM=__internal_chan_an7581_self \
		$(TARGET_CPPFLAGS) -Wall -Wstrict-prototypes -Wmissing-prototypes \
		-Wmissing-declarations $(FPIC) -DAST_MODULE=\"chan_an7581\"
	$(TARGET_CC) -o $(PKG_BUILD_DIR)/chan_an7581.so -pthread $(TARGET_LDFLAGS) -shared \
		-Wl,--version-script,$(PKG_BUILD_DIR)/chan_an7581.exports,--warn-common \
		$(PKG_BUILD_DIR)/chan_an7581.o -lm
endef

define Package/asterisk-chan-an7581/install
	$(INSTALL_DIR) $(1)/usr/lib/asterisk/modules
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/chan_an7581.so $(1)/usr/lib/asterisk/modules/
	$(INSTALL_DIR) $(1)/etc/asterisk
	$(INSTALL_DATA) $(CURDIR)/files/an7581.conf $(1)/etc/asterisk/an7581.conf
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(CURDIR)/files/an7581-voice.init $(1)/etc/init.d/an7581-voice
endef

$(eval $(call BuildPackage,asterisk-chan-an7581))
