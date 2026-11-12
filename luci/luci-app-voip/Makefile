include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI VoIP / SIP configuration
LUCI_DEPENDS:=+luci-base +asterisk +asterisk-chan-an7581 \
	+asterisk-pjsip +asterisk-res-pjproject +ip-full
PKG_VERSION:=0.2.0
PKG_RELEASE:=10
PKG_LICENSE:=Apache-2.0

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
