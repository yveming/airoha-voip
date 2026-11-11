# airoha-voice

Airoha AN7581 + Le9641 FXS voice stack for OpenWrt.

This repository is the source tree for the voice packages.  The package
directories follow the OpenWrt feed layout, so the repository can also be used
directly as a feed:

```sh
echo 'src-git airoha_voice https://github.com/yveming/airoha-voice.git' >> feeds.conf
./scripts/feeds update airoha_voice
./scripts/feeds install -a -p airoha_voice
```

## Packages

| Path | Package | Description |
| --- | --- | --- |
| `kernel/an7581-voice/` | `kmod-an7581-zsi`, `kmod-an7581-pcm`, `kmod-an7581-slic-le9641` | AN7581 PCM/ZSI and Le9641 SLIC backends |
| `net/asterisk-chan-an7581/` | `asterisk-chan-an7581` | Asterisk FXS channel driver |
| `luci/luci-app-voice/` | `luci-app-voice` | LuCI voice configuration, `voice-apply` and `phone-led` |

The current code keeps the `an7581`/`Le9641` names and `/dev/an7581-*` device
paths.  A chip-independent HAL is intentionally not part of this first split.

## Requirements

- OpenWrt/ImmortalWrt with the `airoha/an7581` target
- `telephony` and `luci` feeds for Asterisk and LuCI
- A device tree that provides the `airoha,an7581-pcm`, `airoha,an7581-zsi`
  and Le9641 SLIC nodes

## Thin packaging

`openwrt-pon-feed` carries the thin Makefiles that fetch this repository by
`PKG_SOURCE_URL`/`PKG_SOURCE_VERSION`, in the same way as the IPTV packages.
