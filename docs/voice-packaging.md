# VoIP packaging and feed split

## Repository layout

`airoha-voip` is a feed-compatible source repository.  Do not put a
top-level `package/` directory in it; OpenWrt feed scanning expects package
directories directly below the repository root:

```text
kernel/an7581-voice/
net/asterisk-chan-an7581/
luci/luci-app-voip/
```

## Thin packages in openwrt-pon-feed

`openwrt-pon-feed` carries only Makefiles.  The source is fetched with:

```text
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/yveming/airoha-voip.git
PKG_SOURCE_VERSION:=<full commit>
PKG_SOURCE_DATE:=YYYY-MM-DD
PKG_SOURCE_SUBDIR:=airoha-voip-YYYY-MM-DD~<short>
PKG_SOURCE:=airoha-voip-YYYY-MM-DD~<short>.tar.zst
PKG_MIRROR_HASH:=<sha256>
```

All three thin Makefiles must pin the same commit.  Update the version,
subdir, tarball name and mirror hash together.

## Device profile

The HG5582A image uses one profile without a `voice` suffix:

```text
Device/fiberhome_hg5582a
```

- `DEVICE_DTS := an7581-fiberhome-hg5582a`
- `SUPPORTED_DEVICES += fiberhome,hg5582a`
- no `DEVICE_VARIANT`; the image is
  `...-fiberhome_hg5582a-squashfs-sysupgrade.itb`
- DTS keeps `fiberhome,hg5382a` as the first compatible string for the
  stock HG5382A U-Boot
- `05_fiberhome_board_name` restores the real board name

## Required VoIP packages

The device profile and seed must provide the runtime set, either directly
or through a `voip-meta` package:

```text
asterisk
asterisk-chan-an7581
asterisk-pjsip
asterisk-res-pjproject
asterisk-app-playtones
asterisk-app-read
asterisk-app-waituntil
asterisk-app-senddtmf
asterisk-app-stack
asterisk-app-verbose
asterisk-func-volume
asterisk-codec-alaw
asterisk-codec-ulaw
asterisk-codec-g722
asterisk-format-wav
asterisk-res-agi
asterisk-res-musiconhold
asterisk-res-rtp-asterisk
asterisk-res-speech
asterisk-sounds
kmod-an7581-zsi
kmod-an7581-pcm
kmod-an7581-slic-le9641
luci-app-voip
luci-i18n-voip-zh-cn
```

`asterisk-pjsip` and `asterisk-res-pjproject` are pulled by
`luci-app-voip`, so they may not appear in the seed/profile even though
they are installed.

## Verification

Check the per-device APK database, not only the seed:

```sh
T=build_dir/target-aarch64_cortex-a53_musl/linux-airoha_an7581/target-dir-*
grep -E '^P:(asterisk|kmod-an7581|luci-app-voip|luci-i18n-voip)' \
  "$T/lib/apk/db/installed"
```

On a running device:

```sh
for p in asterisk asterisk-chan-an7581 asterisk-pjsip \
  kmod-an7581-zsi kmod-an7581-pcm kmod-an7581-slic-le9641 \
  luci-app-voip luci-i18n-voip-zh-cn
do
  apk info -e "$p" >/dev/null 2>&1 || echo "MISS $p"
done
```
