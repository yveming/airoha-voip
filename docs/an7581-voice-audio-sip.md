# AN7581 Le9641 voice: working PCM/ZSI audio and Asterisk channel

This document records the validated audio path on the FiberHome HG5582A
(AN7581 + Le9641 over ZSI) and the Asterisk integration.

## Validated hardware configuration

```text
ZSI ID:                 08 49
PCM controller:         ctrl=0xf5071306
PCM channel:            channel 0 only
PCM TX/RX slot config:  0x10301020  (bus byte slot 4, bit offset 0x20)
PCM channel enable:     0x1
PCM frame:              160 bytes = 80 samples = 10 ms, 8 kHz, 16-bit
SLIC state:             0x23 (active + codec)
SLIC OPFUNC:            0xbf (linear + all filters)
SLIC TX/RX slots:       3 / 4 (ZSI TX shift applied by the driver)
SLIC GR:                0xa871 (-6 dB, OEM default)
SLIC VPGAIN:            0x00 (OEM default)
```

The PCM controller and SLIC are connected through the PCM1 pin group.  The
ZSI control channel uses `pcm1` + `pcm_spi_cs1`; the PCM data path is the
normal PCM1 data pins.

Validated results:

- SLIC timeslot loopback (`OPCOND=0x04`) returns the PCM TX data on the PCM
  RX channel.
- A 1 kHz WAV played by Asterisk through `chan_an7581` is a clean tone.
- `Echo()` through Asterisk is stable in both directions.
- Voice capture from the SLIC ADC produces a normal 16-bit waveform.

## Kernel interfaces

### `/dev/an7581-pcm`

Misc character device, 8 kHz signed linear 16-bit mono.

- `read()` returns 160-byte (10 ms) frames from PCM channel 0 (SLIC ADC).
- `write()` accepts 8 kHz signed linear frames for PCM channel 0 (SLIC DAC).
- `poll()` reports `POLLIN` when a frame is available or the stream stops.
- The device is single-open.
- Opening the device starts the PCM DMA stream; closing it stops the stream.

### `/dev/an7581-slic`

Misc character device for the Le9641 control path.

```c
#define AN7581_SLIC_IOC_MAGIC 'V'

struct an7581_slic_status {
        __u32 id;
        __u32 state;
        __u32 opfunc;
        __u32 hook;
        __u32 tx;
        __u32 rx;
};

struct an7581_slic_event {
        __u32 hook;
        __u32 state;
};

#define AN7581_SLIC_GET_STATUS _IOR('V', 1, struct an7581_slic_status)
#define AN7581_SLIC_SET_RING   _IOW('V', 2, int)
#define AN7581_SLIC_SET_ACTIVE _IOW('V', 3, int)
#define AN7581_SLIC_GET_HOOK   _IOR('V', 4, int)
```

- `read()` blocks until the hook state changes and returns an event.
- `poll()` reports `POLLIN` for a pending hook event.
- `SET_RING(1)` enters ring state; `SET_RING(0)` returns to idle.
- `SET_ACTIVE(1)` applies the audio profile and enters active+codec state.
- The driver polls the SLIC hook every 50 ms and caches the state.

## Boot integration

The `asterisk-chan-an7581` package installs:

- `/usr/lib/asterisk/modules/chan_an7581.so`
- `/etc/asterisk/an7581.conf`
- `/etc/init.d/an7581-voice`

`/etc/asterisk/an7581.conf`:

```ini
[general]
pcm_dev = /dev/an7581-pcm
slic_dev = /dev/an7581-slic
context = from-analog
exten = s
line_name = line1
```

`/etc/init.d/an7581-voice` loads the three kernel modules in order, runs
`echo id > /proc/an7581-zsi`, runs `printf init > /proc/an7581-slic`, and
sets the device permissions so Asterisk can open them.

The init script intentionally does **not** start a PCM stream at boot.
`chan_an7581` opens `/dev/an7581-pcm` when a call starts.

## Asterisk package set

The voice image selects:

```text
asterisk
asterisk-pjsip
asterisk-chan-an7581
asterisk-app-stack
asterisk-app-verbose
asterisk-func-volume
asterisk-app-read
asterisk-app-waituntil
asterisk-app-senddtmf
```

`chan_an7581` uses:

- `/dev/an7581-pcm` for audio;
- `/dev/an7581-slic` for hook events, ring, and line state;
- `ast_dsp` for in-band DTMF detection (digits from the FXS phone).

## Dialplan examples

Echo test:

```ini
[from-analog]
exten => s,1,Answer()
 same => n,Echo()
```

Outbound SIP with digits entered on the phone:

```ini
[from-analog]
exten => s,1,Answer()
 same => n,Read(NUMBER,,,10)
 same => n,Dial(PJSIP/${NUMBER}@provider,60)
 same => n,Hangup()
```

Outbound SIP to a fixed destination:

```ini
[from-analog]
exten => s,1,Answer()
 same => n,Dial(PJSIP/1000@provider,60)
 same => n,Hangup()
```

Inbound SIP call to the FXS phone:

```ini
[from-sip]
exten => _X.,1,Dial(AN7581/line1,30)
 same => n,Hangup()
```

The PJSIP endpoint used for incoming calls must set `context=from-sip`.
`Dial(AN7581/line1)` calls `chan_an7581`'s requester, which rings the SLIC
and answers when the phone goes offhook.

## PJSIP sample

Replace `REGISTRAR`, `USER`, `PASSWORD`, and the endpoint/number values.

```ini
[global]
type=global
user_agent=AN7581

[transport-udp]
type=transport
protocol=udp
bind=0.0.0.0:5060

[provider-reg]
type=registration
transport=transport-udp
server_uri=sip:REGISTRAR
client_uri=sip:USER@REGISTRAR
contact_user=USER
outbound_auth=provider-auth
retry_interval=60

[provider-auth]
type=auth
auth_type=userpass
username=USER
password=PASSWORD

[provider-aor]
type=aor
contact=sip:REGISTRAR
qualify_frequency=30

[provider]
type=endpoint
transport=transport-udp
context=from-sip
disallow=all
allow=alaw
allow=ulaw
outbound_auth=provider-auth
aors=provider-aor
from_user=USER
from_domain=REGISTRAR

[provider-identify]
type=identify
endpoint=provider
match=REGISTRAR
```

## Current limitations / next work

- Audio is 8 kHz narrowband.  The clean 1 kHz tone and stable echo prove the
  digital path; voice quality is limited by the analog front end and the
  missing OEM codec calibration.
- The Le9641 codec calibration sequence from the OEM VP-API has not been
  ported.  It may improve DC offset, gain accuracy, and voice quality.
- DTMF from SIP to the FXS phone is not yet generated by `chan_an7581`;
  only FXS-to-Asterisk DTMF detection is implemented.
- Multi-line support is not implemented.  The driver currently owns one
  `AN7581/line1` device.
- The first voice-debug image omitted the PON userspace (`ponctl`/`pond`).
  The combined image now builds the PON userspace from the source packages
  (`airoha-ponctl`, `airoha-pond`, `luci-app-pon`) and also includes
  `airoha-pon-debug`; see the combined-image section below.

## Test log summary

```text
[SLIC]  id=08/49 state=23 opfunc=bf tx=3 rx=4
[PCM]   ctrl=f5071306 chan=1 tx0=0x10301020 rx0=0x10301020
[Asterisk] Playback(test1k) -> clean 1 kHz tone
[Asterisk] Echo() -> stable echo in both directions
```

## Built voice-debug image (2026-11-09)

```text
revision 2026-11-9-4765b65762-62ecb1d39a
file     ponwrt-airoha-an7581-fiberhome_hg5582a-voice-squashfs-sysupgrade.itb
size     19231057 bytes
sha256   598b4594cbf5ee1db5211e67133b7206e71337fd0abe9c1dd58eb6b31a0f7a88
```

The image was built offline and is **not flashed**.  It includes
`asterisk-chan-an7581` and the extra Asterisk application packages listed
above.  The archive copy is under:

```text
HG5582A-archive/an7581-voice-2026-11-09/working-audio/
```

This is still the voice-debug profile without the Rust PON userspace.  Do
not flash it over the live PON link; use a maintenance window or build a
combined PON+voice production image first.


## Combined PON + voice image (2026-11-09)

The voice profile no longer omits the PON userspace.  The image now contains:

- Airoha PON kernel drivers (`kmod-airoha-xpon`, `kmod-airoha-paged-bosa`)
- Source PON userspace: `airoha-ponctl`, `airoha-pond` (built from the
  `airoha-pon-daemons` source package), `luci-app-pon`,
  `luci-i18n-pon-zh-cn` and `airoha-pon-debug` (the diagnostic collector,
  including `/usr/sbin/pon-debug`).  The temporary `pon-prebuilt` binary
  package was removed; Rust is built from source in the voice tree.
- `chan_an7581`, the Asterisk voice applications and `luci-app-voice`

### Voice bring-up check

```sh
asterisk -rx 'module show like chan_an7581'
asterisk -rx 'pjsip show transports'
asterisk -rx 'dialplan show from-analog'
asterisk -rx 'core show channels'
/usr/libexec/voice-apply status
```

`luci-app-voice` owns `/etc/config/voice` and generates
`/etc/asterisk/pjsip.conf`, `extensions.conf` and `an7581.conf`.  The
default mode is Echo; fill in the SIP account and switch the off-hook mode
to SIP when the credentials are available.

### Bench state at the time of writing

`pon0` is `NO-CARRIER` because the HG5582A is not connected to the optical
line.  The live-line test still needs the device connected and the voice
checks above repeated.

### Combined image built and flashed (2026-11-09)

```text
revision 2026-11-9-4765b65762-0e5f67a122
file     ponwrt-airoha-an7581-fiberhome_hg5582a-voice-squashfs-sysupgrade.itb
size     19616081 bytes
sha256   9a7d0742e5b886f160b27cb95c6a556fa318a3c91fb2ada61330107583d907fd
```

The image was flashed with `sysupgrade` while the HG5582A was off the PON
line; `/etc/config` was preserved.  After the reboot the following were
verified:

- `luci-app-voice`, `airoha-ponctl`, `airoha-pond`, `airoha-pon-debug`,
  `luci-app-pon`, `kmod-airoha-xpon`, `asterisk-chan-an7581` and
  `asterisk` are installed.
- `airoha-pond` is running (`wait-optical-signal`).
- Asterisk is running, `chan_an7581.so` is loaded, PJSIP is bound to
  `0.0.0.0:5060`, and `from-analog` is in Echo mode.

PON mode note: the preserved `/etc/config/pon` selects `epon-10g-1g`.
The previous image reported `active_mode=xgpon` with `mode_pending=1`;
the new image applied the configured `epon-10g-1g` at boot.  Confirm the
live line is 10G EPON before connecting.  If it is XG-PON, set
`pon.line0.mode='xgpon'` in LuCI → PON (or with `uci`) and restart the
`pon`/`airoha-pond` services before the live test.

### Live-line test checklist (10G-EPON confirmed)

The line is 10G-EPON, so keep `pon.line0.mode='epon-10g-1g'`.

1. Connect the HG5582A to the PON line and wait for registration.
2. Check the PON state:

   ```sh
   ponctl status | sed -n '1,25p'
   ```

   Expected after registration: `optical_signal: 1`, `pcs_sync: 1`,
   `llid_valid: 1`, and `mpcp_state: registered`.

3. Check voice:

   ```sh
   asterisk -rx 'module show like chan_an7581'
   asterisk -rx 'pjsip show transports'
   asterisk -rx 'dialplan show from-analog'
   ```

   Pick up the FXS phone; the default `from-analog` context answers and
   runs `Echo()`.  This confirms the PCM/ZSI/SLIC path after the live
   reboot.  Real SIP calls need the provider credentials filled into
   **Services → Voice** and the off-hook mode switched from Echo to SIP.


## ONU_CONFIG-style voice page (first release)

`luci-app-voice` 0.2.0 replaces the small Echo/SIP form with the
ONU_CONFIG-style vendor page, but keeps the configuration under
`/etc/config/voice` (not `onu-voice`):

```text
config voice 'config'      protocol / test mode / DTMF / gains / SIP account
config network 'network'  voice VLAN, route only in this release
config sip 'sip'          proxy / registrar / outbound / home domain / heartbeat
config digitmap 'digitmap'
config codec '...'        G.711A/U, G.722, G.729
```

### Voice network

- `route` is the only delivery mode.  The page no longer exposes the delivery
  mode/bridge/trunk selectors or the read-only "Voice interface" row.
- The VLAN has **no default** in the package.  The operator/region value must
  be entered on the page.

- The read-only hardware identity (`/dev/an7581-slic`, `PCM0` channel 0 slot
  4, terminal ID `AN7581/<line_name>`) lives on the Status page, not in
  Configuration.
- The device name is `voice.network.name`, default `voice`, and is editable on
  the page.  The logical interface stays `voice`; changing the device name
  only renames the 8021q VLAN device and updates the interface's `device`
  reference.
- Enabling the voice network with the VLAN of the legacy `network.voip`
  interface takes over that interface: the backend removes `network.voip`
  and its `pon0.<vid>` device, copies its MAC into `voice.network.macaddr`,
  and creates:

  ```text
  network.luci_voice_vlan  type=8021q, name=voice, vid=<vlan>
  network.voice            device=voice, DHCP/static, ip4table=101
  ```

  The kernel VLAN device and the UCI logical interface are both named
  `voice`; this works with netifd and keeps the page/status simple.
- A `voice` firewall zone with input DROP and explicit SIP (UDP/TCP 5060) and
  RTP (UDP 10000-20000) accept rules is created.
- `defaultroute` is not set, so netifd's default (1) applies.  The voice
  default route is installed **into table 101 only**; the customer's main
  table/default WAN is unaffected.
- netifd already adds the policy rules for an `ip4table` interface when the
  address is added; `voice-apply` does **not** add a manual rule.  The rules
  are:

  ```text
  10000: from <voice_ip>/32 lookup 101
  20000: to <voice_subnet>/<mask> lookup 101
  900xx: from all iif lo lookup 101
  ```

  The source rule sends packets sourced from the voice IP to table 101 (which
  now has the voice default route); the destination rule covers the voice
  subnet.  A manual `from <voice_ip> lookup 101 priority 30010` rule is
  redundant and was removed.
- The `voice` hotplug waits briefly for the address, regenerates PJSIP with
  `bind=<voice_ip>:5060`, and reloads the PJSIP modules.  If the bind address
  changed, Asterisk is restarted because `res_pjsip` cannot reliably rebind an
  existing transport socket; an unchanged address only reloads the modules.
  The source bind is what makes the kernel source rule match; binding to
  `0.0.0.0` would let the source fall back to the WAN.

### SIP / PJSIP mapping

- Proxy / registrar / outbound proxy / home domain and their ports and
  transports generate PJSIP transport, endpoint, AOR, auth, registration and
  identify objects.
- Registration period, retry period, session timers, DSCP and heartbeat map
  to `expiration`, `retry_interval`, `timers_min_se`/`timers_sess_expires`,
  `tos`, and `qualify_frequency`.  `timers_min_se` is clamped to Asterisk's
  minimum of 90 seconds.
- The codec table is sorted by priority and emitted as `allow=` lines.
- `dtmf_method` maps to `dtmf_mode`; the RFC2833 payload is stored for
  reference because PJSIP negotiates the telephone-event payload from SDP.
- Standby servers are stored; the primary server is used in this release.

### Analog / quality

- `chan_an7581` now applies `transmit_gain` and `receive_gain` (software,
  -14..+6 dB) to the PCM read/write paths.  Changes take effect after an
  Asterisk restart.
- `echo_cancellation` is stored in `an7581.conf` and applied to the
  `an7581_pcm.lec_enable` module parameter (OSLEC in the PCM RX path).
- Empty `indications-en75xx.conf` and `musiconhold-en75xx.conf` files are
  installed so the Asterisk package's includes resolve; `Playtones` and the
  China (`cn`) indication tones load correctly.

### First-release behavior

- There is no field migration from the old `/etc/config/voice`
  (`voice.general`) or from `/etc/config/onu-voice`.  If the new
  `voice.config.protocol` is absent, `voice-apply` saves the old file as
  `/etc/config/voice.bak` and starts from the new default.
- The old `voip` network interface is only removed when the new voice network
  is enabled with the same VLAN; otherwise it is left untouched.
- H.248 was removed from this SIP image: the Configuration page no longer
  exposes it, the package default has no `h248` section, and the current
  board's `voice.h248` was deleted.  Asterisk has no native H.248 MG; an
  H.248-only line would need a separate MG stack (the vendor stack is
  EN75xx-specific and not usable with the AN7581 drivers).

### Current bench state after the takeover test

```text
voice.network.enabled=1, mode=route, name=voice, vlan=45, proto=dhcp
voice.network.macaddr=02:00:00:00:00:45   (from the legacy pon0.45)
network.luci_voice_vlan.name=voice, vid=45
network.voice.device=voice, ip4table=101
ifstatus voice: pending DHCP (pon0 has NO-CARRIER)
firewall.luci_voice_zone: voice zone + SIP/RTP rules
Asterisk: Echo mode, transport-udp 0.0.0.0:5060, no provider yet
```

The old `network.voip`/`pon0.45` interface is gone.  The user still needs to
fill the SIP server/account/password on the page and switch the test mode
from Echo to a SIP mode when ready.


### Voice UI pages

`luci-app-voice` now follows the `luci-app-pon` layout:

Both pages start with **Voice service network**, then **Voice service**;
the status page has no top `h2`/description and starts directly with the
network card.

- **Services → Voice → Status**: a structured runtime summary, not raw
  command output:
  - **Voice service network**: status, route mode, VLAN, device name, link
    carrier, IPv4 address, gateway, MAC, policy-route presence.
  - **Voice service**: status, protocol, test mode, Asterisk
    running/stopped, active calls.
  - **SIP / PJSIP**: account configured/not configured, registrar, proxy,
    transport, registration state, endpoint loaded, registration detail and
    a short troubleshooting note.
  - **FXS line**: FXS device, PCM channel, terminal ID, hook, SLIC state,
    PCM device.
  - **Restart Asterisk** and **Refresh** buttons only.  The standard LuCI
    form footer is suppressed with `handleSaveApply: null`, `handleSave:
    null` and `handleReset: null`, matching `luci-app-pon/status.js`.
- **Services → Voice → Configuration**: **Voice service network**,
  **Voice service**, SIP, digit map and codec pages.  No H.248, no delivery
  mode/bridge/trunk, no read-only "Voice interface" row, and no status card.
  The two `enabled` switches are both labelled just **Enable** with no
  descriptions.
- The menu view paths are `voice/status` and `voice/config`.  The old
  `voice/overview` view was removed; a browser that still shows the old page
  needs one hard refresh.

The voice VLAN device name is `voice.network.name` and defaults to `voice`.
It is editable, and the logical interface remains `voice`.

### Voice UI image (2026-11-10)

```text
revision 2026-11-10-4765b65762-b1a266cb5f
file     ponwrt-airoha-an7581-fiberhome_hg5582a-voice-squashfs-sysupgrade.itb
size     19648850 bytes
sha256   b4c0e4742b711817b7937cec51c548ac0d0482ef33dfd7acf383f4b81c21d897
```

This was the first split-page image.  Later builds in this document supersede
it; the running HG5582A also had the `luci-app-voice` and `chan_an7581`
packages installed manually at that point.


### Routing verification (2026-11-10)

With a temporary static voice address the netifd behaviour was verified:

```text
network.voice.ip4table=101, no defaultroute option (netifd default 1)

ip route show table 101:
default via 192.0.2.1 dev voice proto static metric 2100 linkdown
192.0.2.0/24 dev voice proto static scope link metric 2100 linkdown

ip rule show:
10000: from 192.0.2.2 lookup 101
20000: from all to 192.0.2.2/24 lookup 101
900xx: from all iif lo lookup 101

ip route get 8.8.8.8 from 192.0.2.2:
8.8.8.8 from 192.0.2.2 via 192.0.2.1 dev voice table 101
```

The manual `ip rule` is gone.  PJSIP initially stayed on `0.0.0.0:5060`
after the address was added; `res_pjsip` could not rebind the existing
transport.  `voice-apply` now detects a changed `bind=` line and restarts
Asterisk, after which `pjsip show transports` reports
`192.0.2.2:5060`.  Restoring DHCP with no carrier returns the bind to
`0.0.0.0:5060` and removes the table-101 rules.

## 2026-11-10: dial-tone clicks, DTMF masking and line echo

The 10 ms PCM descriptor ring is drained from the PCM descriptor interrupt
when it is available.  When the interrupt cannot be requested, a high-resolution kthread is used
instead of the old delayed work item; its poll interval defaults to 10 ms
(poll_ms) and is capped at 20 ms.
A 40 ms silence reservoir is kept in the TX FIFO so the 20 ms Asterisk
producer cannot empty the FIFO at a 10 ms descriptor boundary.  The initial
descriptors are armed with silence rather than the old 0x1111 diagnostic
pattern.  The interrupt is enabled by default; the kthread only starts as a
fallback when the interrupt is unavailable.

The Le9641 hybrid reflects the locally generated 450 Hz dial tone into the
ADC.  That reflected tone was strong enough to hide the DTMF rows/columns
from `ast_dsp`, so the first digit could time out.  `chan_an7581` now applies
a Q=3 450 Hz biquad notch to the RX path while `dialing` is set, and clears
`dialing` on the first detected DTMF end (Read stops the dial tone then).
Detected digits are logged at verbose level 3.

OSLEC (GPL-2.0, from the airoha_voip vendor tree) is compiled into
`an7581-pcm.ko`.  It uses 128 taps (16 ms) by default and processes each RX
descriptor against the exact TX frame that was sent in that descriptor slot.
This is a line echo canceller, not an acoustic canceller.

Runtime controls:

```sh
cat /proc/an7581-pcm           # last RX frame
echo dump > /proc/an7581-pcm   # IRQ, underruns, OSLEC state
echo ec > /proc/an7581-pcm     # OSLEC taps/active/mode
echo ecreset > /proc/an7581-pcm
cat /sys/module/an7581_pcm/parameters/lec_enable
echo 0 > /sys/module/an7581_pcm/parameters/lec_enable   # bypass OSLEC
```

`voice.config.echo_cancellation` is written to `an7581.conf`; `chan_an7581`
imports it into the `an7581_pcm.lec_enable` module parameter when Asterisk
loads, and `voice-apply` also applies it directly via sysfs.  The init script
gives the `asterisk` group write access to that parameter.

IMS outbound calls now send `P-Preferred-Identity` explicitly from a Dial
pre-dial handler.  Asterisk's `res_pjsip` only copies the REGISTER
`P-Associated-URI` to flow (TCP/TLS) transports, so the UDP registration
needed this.  The phone number is taken from `voice.sip.phone_number`, or
from the user part of `voice.sip.auth_username` if that field is empty.

Packages for this round:

```text
kmod-an7581-pcm-6.18.60-r4.apk
kmod-an7581-slic-le9641-6.18.60-r4.apk
kmod-an7581-zsi-6.18.60-r4.apk
asterisk-chan-an7581-11.apk
luci-app-voice-0.2.0-r10.apk
```

## 2026-11-10 LED map and DTMF diagnostics

The HG5582A panel uses the same LED wiring as the HG5382A/HG5585F
family, not the generic `7581ptled.conf` profile:

```text
power       GPIO13 active low
PON         GPIO14 active low  pon0-registration
LOS         GPIO16 active low  pon0-los
Internet    GPIO5  active low  pon0-online
Phone       GPIO0  active low  blue:phone, controlled by phone-led
reset key   GPIO35 active low
```

GPIO35 is the reset key (the HG5585F DTS documents that GPIO0 is
uart1_txd and never sees the key).  The `7581ptled.conf` VOIP GPIO35
entry cannot be used on this board because it conflicts with the key.
The old uncommitted board patch used the correct family GPIOs
(GPIO5/GPIO0); it was simply never committed.

`phone-led` is a procd service that polls:

```sh
asterisk -rx 'pjsip show registrations'
```

and writes `/sys/class/leds/blue:phone/brightness` when `provider-reg`
is `Registered`.  It does not depend on any WAN interface name.

`chan_an7581` logs one confirmed custom digit at verbose level 3:

```text
AN7581: DTMF custom 'x'
```

The per-frame `DTMF cand` diagnostics were removed after the Goertzel
detector was stabilised; `start_audio` is only logged at debug level 3.
The detector runs only while `collecting` is set, so carrier progress
tones after `Dial()` are not reported as digits.

A user hearing their own voice in the handset is hearing the analogue
phone's sidetone or acoustic coupling, not the line echo OSLEC removes.
OSLEC only cancels the far-end signal reflected by the 2-wire hybrid;
toggling `lec_enable` therefore has no effect on sidetone.

## 2026-11-10 cleanup build

The dead-code/config cleanup keeps the standby and digit-map fields for the
next feature round, removes the redundant home-domain field, imports
`echo_cancellation` into the PCM module parameter, wires `no_answer_timer`
into the inbound Dial timeout, fixes digit-map pattern validation so
that a non-empty digit map actually generates outbound extensions.  The status
page no longer reuses the ambiguous `Present` msgid (luci-base translates it
as "已连接"); policy-rule and PCM-device states now use `Configured` and
`Exists`.  Menu titles `Voice` and `Configuration` are kept in `voice.po` so
the top-level menu stays translated.

```text
file     ponwrt-airoha-an7581-fiberhome_hg5582a-voice-squashfs-sysupgrade.itb
size     19620178 bytes
sha256   302ac42a86a13e770c1c0abe8b1adfeff2c085955e11e631d1dff78c9393ea09

kmod-an7581-pcm-6.18.60-r4.apk
kmod-an7581-slic-le9641-6.18.60-r4.apk
kmod-an7581-zsi-6.18.60-r4.apk
asterisk-chan-an7581-11.apk
luci-app-voice-0.2.0-r10.apk
```

Existing `/etc/config/voice` files are preserved across sysupgrade.  The old
`home_domain*` keys are ignored by the new backend; `match_mode` stays at its
old value until it is changed in the UI or with `uci set`.  Standby servers
remain stored but are still not used by the generated PJSIP configuration.
