# Voice bring-up pitfalls

Each item records the symptom, the root cause and the verification step.
The register-level bring-up notes live in `an7581-voice.md`; the current
working image and interfaces are described in `an7581-voice-audio-sip.md`.

## 1. Do not bind the EN75xx voice stack

- Symptom: EN75xx PCM/ZSI/SLIC modules probe but the HG5582A has no usable
  FXS audio.
- Cause: the EN75xx stack targets EN751221/EN7528/EN7523 and its register
  table still contains the legacy MIPS PCM layout.
- Fix: use the AN7581-specific `an7581-zsi`, `an7581-pcm` and
  `an7581-slic-le9641` drivers only.
- Verify: `lsmod` shows no `en75xx-*` module.

## 2. Le9641 needs the IB profile, not BB

- Symptom: the SLIC detects and profiles load, but the line stays in
  shutdown.
- Cause: the BB profile disables the charge pump; the HG5582A uses the IB
  profile.
- Fix: use `DEV_PROFILE_90V_IB_124` with the ZSI clock slot patched from
  `0x40` to `0x46`.
- Verify: `devmode` reads `0x0c`, not `0x14`.

## 3. Le9641 shutdown: SWCTRL and CALCTRL order

- Symptom: state writes are accepted but the chip remains in shutdown.
- Cause: writing `SWCTRL=0x6f` directly is not the OEM init sequence.
- Fix: leave the switcher in low-power mode (`SWCTRL=0x05`), clear
  `CALCTRL`, then set `STATE=active|codec`.
- Verify: `STATE` reads `0x23`; ring reads `0xa7` and returns to `0x04`.

## 4. 10 ms PCM clicks

- Symptom: regular clicks in tones and speech.
- Cause: a 10 ms delayed work item drifts against the PCM frame clock and
  inserts a zero-filled descriptor.
- Fix: use the PCM IRQ when available, with a 2 ms hrtimer kthread as the
  watchdog, and keep a 40 ms TX FIFO prefill.
- Verify: `echo dump > /proc/an7581-pcm` shows no growing underruns; a
  1 kHz tone is clean.

## 5. DTMF from short analogue bursts

- Symptom: digits are lost, especially the first one, while the phone is
  actually sending DTMF.
- Cause: this phone emits very short DTMF bursts; `ast_dsp` expects two
  consecutive analysis blocks before reporting BEGIN.
- Fix: a custom Goertzel detector over the full 20 ms frame, with:
  - row/column energy threshold `1e8`
  - twist limit `6.31`
  - peak/second-peak quality `>= 4x`
  - two consecutive matching frames
- Verify: `AN7581: DTMF custom '1'` appears once per key, not once per
  frame.

## 6. First dialled digit overwritten

- Symptom: dialling `10000000000` produces `0000000000`.
- Cause: `Read(NUMBER,dial,1)` is followed by `Read(NUMBER,,19)`, which
  overwrites the first digit.
- Fix:
  ```text
  Read(FIRST,dial,1,i,,15)
  GotoIf($["${FIRST}"=""]?congestion,1)
  Read(REST,,19,,,5)
  Set(NUMBER=${FIRST}${REST})
  ```
- Verify: the console prints `Voice dialled 10000000000`.

## 7. DTMF after `Dial`

- Symptom: carrier progress tones or echo produce a false `DTMF custom '2'`
  after the call has already been dialled.
- Cause: the custom detector kept running after digit collection.
- Fix: `collecting` is set only for the collection phase and cleared on
  `RINGING`, `PROGRESS`, `ANSWER`, `BUSY` and `CONGESTION`. In-call DTMF
  goes back to `ast_dsp`.
- Verify: no custom digit appears after the `Dial()` line.

## 8. Sidetone is not line echo

- Symptom: the user hears their own voice in the handset, and toggling
  OSLEC makes no difference.
- Cause: this is the analogue phone's sidetone or acoustic coupling, not
  the 2-wire hybrid echo of the far-end signal. OSLEC's reference is the
  TX signal; the user's own voice is not in that reference.
- Fix: no OSLEC change. Test with another handset or an IVR; only a
  repeated far-end voice is line/network echo.
- Verify: `lec_enable` toggles but the sidetone remains; that is expected.

## 9. IMS P-Preferred-Identity

- Symptom: IMS accepts registration but outbound calls are rejected or
  present the wrong identity.
- Cause: `res_pjsip` only copies `P-Associated-URI` to flow (TCP/TLS)
  transports; the UDP registration needs an explicit PPI.
- Fix: add a Dial pre-dial Gosub that sets:
  ```text
  PJSIP_HEADER(add,P-Preferred-Identity)=<sip:user@domain;user=phone>
  ```
- Verify: the outbound INVITE contains PPI with the phone number and home
  domain.

## 10. HG5582A U-Boot compatibility

- Symptom: stock HG5382A recovery rejects the FIT, or a HG5382A is allowed
  to flash a voice image it cannot run.
- Cause: `compatible` and `SUPPORTED_DEVICES` serve different purposes.
- Fix:
  - DTS first compatible: `fiberhome,hg5382a`, second:
    `fiberhome,hg5582a`.
  - `SUPPORTED_DEVICES` in the image: only `fiberhome,hg5582a`.
  - preinit `05_fiberhome_board_name` restores `/tmp/sysinfo/board_name`
    to `fiberhome,hg5582a`.
- Verify: `cat /tmp/sysinfo/board_name` after boot; `sysupgrade` refuses a
  HG5382A target.

## 11. Phone LED

- Symptom: phone LED never lights or follows the wrong GPIO.
- Cause: the panel phone LED is GPIO0 active-low and is not a PON LED.
- Fix: DTS `led-phone` label `blue:phone`; `phone-led` polls
  `pjsip show registrations` for `provider-reg.*Registered`.
- Verify:
  ```sh
  cat /sys/class/leds/blue:phone/brightness
  /etc/init.d/phone-led status
  ```

## 12. UI translation collisions

- Symptom: status shows `策略路由 已连接`, or the top-level menu shows
  `Voice` instead of `语音`.
- Cause: `Present` is translated by `luci-base` as `已连接`; regenerating
  `voice.po` from only the JS files dropped the `menu.d` titles.
- Fix: use `Configured`/`Exists` for status values; keep `Voice` and
  `Configuration` in `voice.po`.
- Verify: menu shows `语音`/`配置`; status shows `策略路由 已配置`.
