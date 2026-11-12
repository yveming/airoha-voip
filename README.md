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

## Documentation

- `docs/an7581-voice.md` — register-level AN7581/Le9641 bring-up
- `docs/an7581-voice-audio-sip.md` — working PCM/ZSI/Asterisk image
- `docs/voice-pitfalls.md` — symptom/cause/fix log for the difficult bugs
- `docs/voice-packaging.md` — feed layout, thin packages, device profile, seed

## Credits

References:

- [airoha_voip](https://github.com/Sirherobrine23/airoha_voip) — vendor voice API and OSLEC integration reference; Le9641 VP-API/profile data originates from Microsemi/Microchip.
- OSLEC by Steve Underwood and David Rowe.
- naoki66's ONU_CONFIG/LuCI voice page for UI inspiration.

## Requirements

- OpenWrt/ImmortalWrt with the `airoha/an7581` target
- `telephony` and `luci` feeds for Asterisk and LuCI
- A device tree that provides the `airoha,an7581-pcm`, `airoha,an7581-zsi`
  and Le9641 SLIC nodes

## Thin packaging

`openwrt-pon-feed` carries the thin Makefiles that fetch this repository by
`PKG_SOURCE_URL`/`PKG_SOURCE_VERSION`.
