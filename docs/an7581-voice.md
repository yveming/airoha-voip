# AN7581 voice bring-up notes

This is the AN7581/HG5582A reference, not the EN75xx driver.

## Why the EN75xx stack is not used

The upstream `en75xx` voice stack is validated on EN751221, EN7528 and
EN7523.  Its README and `docs/07` state that the Airoha LTS SDK targets
AN7551/AN7581, which are newer than the parts that driver supports, and
that the SDK `pcm1.ko` register table still carries the legacy MIPS
`0xbfbd0000` layout.  HG5582A is AN7581.  Do not bind the EN75xx PCM,
ZSI or SLIC drivers to it.

The old HG5582A voice patches built on `en75xx` are obsolete.  The
AN7581-specific platform data is borrowed from the XG2010G work, while
the ZSI/PCM register sequence comes from the OEM AN7581 modules.

## OEM reference modules

Hashes (sha256):

```text
pcm1.ko  e407c28281651db1290c5a336b6170f3c195b8df9e76603d9e141567ac58287d
spi.ko   4c61ecf22bd7efe3ec0e2cd228065029959c3f8bd9b0c48ad5e77457711041fe
slic3.ko 2deb19fac164ce9b4171fe3e28031b63656f4aba3397622697047ad25fcbfb6d
fxs3.ko  b2c424a98130ff82da98d58e88cc85738b2414db2e7804f9bf61f9811a37eb5b
```

The ARM64 `pcm1.ko` is unstripped.  Its exported `regMap` array has 42
entries of 24 bytes:

```c
struct pcm_reg {
    const char *name;   /* relocated pointer */
    u32 flags;          /* 0 in this build */
    u32 pad;            /* 3 in this build */
    u32 writable_mask;
    u32 address;        /* legacy virtual address 0xbfbd.... */
    u32 reset_value;
};
```

## AN7581 PCM register map (from pcm1.ko regMap)

| # | name | mask | address (phys) | reset |
| ---: | --- | ---: | ---: | ---: |
| 0 | pcmCtrl | 0x1f7f1f1f | 0x1fbd0000 | 0x0500040a |
| 1 | txTimeSlotCfg0 | 0x13ff13ff | 0x1fbd0004 | 0x00080000 |
| 2 | txTimeSlotCfg1 | 0x13ff13ff | 0x1fbd0008 | 0x00180010 |
| 3 | txTimeSlotCfg2 | 0x13ff13ff | 0x1fbd000c | 0x00280020 |
| 4 | txTimeSlotCfg3 | 0x13ff13ff | 0x1fbd0010 | 0x00380030 |
| 5 | txTimeSlotCfg4 | 0x13ff13ff | 0x1fbd0048 | 0x00480040 |
| 6 | txTimeSlotCfg5 | 0x13ff13ff | 0x1fbd004c | 0x00580050 |
| 7 | txTimeSlotCfg6 | 0x13ff13ff | 0x1fbd0050 | 0x00680060 |
| 8 | txTimeSlotCfg7 | 0x13ff13ff | 0x1fbd0054 | 0x00780070 |
| 9 | txTimeSlotCfg8 | 0x13ff13ff | 0x1fbd0058 | 0x00880080 |
| 10 | txTimeSlotCfg9 | 0x13ff13ff | 0x1fbd005c | 0x00980090 |
| 11 | txTimeSlotCfg10 | 0x13ff13ff | 0x1fbd0060 | 0x00a800a0 |
| 12 | txTimeSlotCfg11 | 0x13ff13ff | 0x1fbd0064 | 0x00b800b0 |
| 13 | txTimeSlotCfg12 | 0x13ff13ff | 0x1fbd0068 | 0x00c800c0 |
| 14 | txTimeSlotCfg13 | 0x13ff13ff | 0x1fbd006c | 0x00d800d0 |
| 15 | txTimeSlotCfg14 | 0x13ff13ff | 0x1fbd0070 | 0x00e800e0 |
| 16 | txTimeSlotCfg15 | 0x13ff13ff | 0x1fbd0074 | 0x00f800f0 |
| 17 | rxTimeSlotCfg0 | 0x13ff13ff | 0x1fbd0014 | 0x00080000 |
| 18 | rxTimeSlotCfg1 | 0x13ff13ff | 0x1fbd0018 | 0x00180010 |
| 19 | rxTimeSlotCfg2 | 0x13ff13ff | 0x1fbd001c | 0x00280020 |
| 20 | rxTimeSlotCfg3 | 0x13ff13ff | 0x1fbd0020 | 0x00380030 |
| 21 | rxTimeSlotCfg4 | 0x13ff13ff | 0x1fbd0078 | 0x00480040 |
| 22 | rxTimeSlotCfg5 | 0x13ff13ff | 0x1fbd007c | 0x00580050 |
| 23 | rxTimeSlotCfg6 | 0x13ff13ff | 0x1fbd0080 | 0x00680060 |
| 24 | rxTimeSlotCfg7 | 0x13ff13ff | 0x1fbd0084 | 0x00780070 |
| 25 | rxTimeSlotCfg8 | 0x13ff13ff | 0x1fbd0088 | 0x00880080 |
| 26 | rxTimeSlotCfg9 | 0x13ff13ff | 0x1fbd008c | 0x00980090 |
| 27 | rxTimeSlotCfg10 | 0x13ff13ff | 0x1fbd0090 | 0x00a800a0 |
| 28 | rxTimeSlotCfg11 | 0x13ff13ff | 0x1fbd0094 | 0x00b800b0 |
| 29 | rxTimeSlotCfg12 | 0x13ff13ff | 0x1fbd0098 | 0x00c800c0 |
| 30 | rxTimeSlotCfg13 | 0x13ff13ff | 0x1fbd009c | 0x00d800d0 |
| 31 | rxTimeSlotCfg14 | 0x13ff13ff | 0x1fbd00a0 | 0x00e800e0 |
| 32 | rxTimeSlotCfg15 | 0x13ff13ff | 0x1fbd00a4 | 0x00f800f0 |
| 33 | ISR | 0x000007ff | 0x1fbd0024 | 0 |
| 34 | INTMask | 0x000007ff | 0x1fbd0028 | 0 |
| 35 | txPolling | 0xffffffff | 0x1fbd002c | 0 |
| 36 | rxPolling | 0xffffffff | 0x1fbd0030 | 0 |
| 37 | txRingBaseAddr | 0xffffffff | 0x1fbd0034 | 0 |
| 38 | rxRingBaseAddr | 0xffffffff | 0x1fbd0038 | 0 |
| 39 | txrxRingSizeAndOff | 0x000000ff | 0x1fbd003c | 0x000000c0 |
| 40 | txRxDMA | 0x0000000f | 0x1fbd0040 | 0x0f000000 |
| 41 | txRxChanEnable | 0x0000000f | 0x1fbd00ac | 0x0000000f |

`descInit()` also writes `0x1fbd00a8 = 0x000000a0`, a register not listed
in the exported regMap.  The EN75xx driver removed that write; AN7581
still performs it.

## PCM descriptor layout

`descInit()` allocates `0xb4` bytes for each direction, and descriptors
are indexed with a `0xc` stride.  That is the 12-byte generation:

```c
struct an7581_pcm_desc {
    u32 status;      /* OWN(31) | sample-size(9:0); other bits preserved */
    u32 ch_valid;    /* (1 << n_channels) - 1 */
    u32 buf_addr;    /* bus address, encoded by pcmSend/rxDescSet */
};
```

There are 15 descriptors (`0xb4 / 0xc`).  `pcmSend()` writes
`0x1fbd002c = 1` to kick TX; `pcmRecv()` clears/sets bit 1 of
`0x1fbd0040` and writes `0x1fbd0030 = 1`.

## PCM reset

`softReset()` for HIR 10/14/12 pulses `0x1fb00834` bit 11 (`0x800`),
not the ZSI block bits.  Do not mix this with the SLIC/ZSI reset.

## AN7581 ZSI wrapper

- Base `0x1fbd1000`, one wrapper for HIR 14/12.
- `+0x00` CFG, `+0x04` TXRX, `+0x08` CTL, `+0x0c` RX, `+0x10` EN,
  `+0x14` channel select.
- Every transaction writes `(cfg & 0xe0ffffef) | 0x0f000010` to CFG.
- CTL is read-modify-write: `status | 2` after TX, `status | 1` before
  RX, `status | 4` after RX.
- ZSI read sends the register, then `0x06`, then reads.
- 5 ms gap per byte.

## Safe HG5582A bring-up sequence (validated by devmem)

```text
0x1fb00094 = (old & ~0xf) | 5       ZSI interface mode
0x1fa20218 = (old & ~0x003f3300) | 0x00001000
0x1fa201d0 |= 0x00000c01
0x1fbd1000 = 0x0f030313
0x1fbd1010 |= 0x19
pulse 0x1fb00834 bit4 only
0x1fbd1000 = 0x0f030313
0x1fbd1010 |= 0x19
```

Then `0x4a=1`, `0x73`, `0x06` reads `08 49` (Le9641 RCN/PCN).

Do not pulse `0x1fb00834` bits 0 or 17 on HG5582A.  They are PCM1/PCM2
block resets and wedge this board during bring-up.

## SLIC/VP-API

`pcm1.ko`/`spi.ko` only provide transport.  The SLIC init remains the
OEM `slic3.ko` sequence:

- `VpdeviceType[5] = 0x0d` (VP_DEV_887_SERIES, Le9641 tracker)
- PCN 0x49
- `VpInitDevice(DEV_PROFILE_100V_BB_124_ZSI, AC_FXS_RF14_600R_DEF_LE9641,
  DC_FXS_miSLIC_BB_DEF, RING_ZL880_BB90V_DEF, ...)`
- `VpSetOption(CODEC = VP_OPTION_LINEAR)`; bus slot 4 for one FXS
- `VpSetOption(TIMESLOT = 4)`
- event mask, line standby, `VpCalLine()`

The Microchip VP-API/profile sources from `airoha_voip` are reusable;
the EN75xx PCM/ZSI transport is not.

## Bring-up order

1. `an7581-zsi`: transport-only, read `08 49`.
2. `an7581-pcm`: register map above, 12-byte descriptors, clock/IRQ.
3. `an7581-slic-le9641`: VP-API 887 init, linear, slot 4.
4. ring, hook, SIP.

## Hardware state observed on the running 6.18.55 debug image

Read-only `devmem` on the old HG5582A image (2026-11-09):

```text
0x1fb00094 = 0xe0004c20   low nibble 0, ZSI interface mode not set
0x1fa20218 = 0x00000000
0x1fa201d0 = 0x00000000
0x1fa201c4 = 0x00000000   PCM clock gate not set
0x1fa201cc = 0x00800005
0x1fbd1000 = 0x19030303
0x1fbd1010 = 0x00000019
0x1fbd0000 = 0x7500040b
0x1fbd0040 = 0x00000000
0x1fbd00ac = 0x0000000f
0x1fbd003c = 0x000000c0
```

The bootloader does not program the ZSI interface mode.  A transport
module loaded with `init_mode=1` would therefore only touch CFG/EN and
then time out.  The driver default is now `init_mode=0` (full safe SCU
sequence), matching the devmem sequence that was already validated.

The old HG5582A DTB (EN75xx-era debug image) used the compatible
`econet,an7581-zsi` for the wrapper, so the new driver also matches
that string.  The old DTB's PCM node used:

```text
pcm@1fbd0000
  compatible = "airoha,en7581-pcm"
  interrupts = <GIC_SPI 27 IRQ_TYPE_LEVEL_HIGH>
```

GIC SPI 27 is the AN7581 PCM interrupt line.  The PCM node itself is
board-level and can be reused, but the old `en7581-pcm` driver and its
register/slot assumptions are not used.

## PCM clock gate

`spi.ko:SPI_cfg()` uses the HIR-14 table returned by
`pcm1.ko:pcmChipScuQuery()` and, when `0x1fb00094` bit 0 is clear,
executes:

```text
0x1fa201c4 = (old & 0xffff) | 0x10000
```

That is the PCM clock gate.  The ZSI detection sequence does not need
it, but an `an7581-pcm` driver must enable it before touching the PCM
DMA engine.

## What not to copy from the old EN75xx debug work

The old board DTS and EN75xx PCM driver are still useful as a source of
board facts (PCM IRQ, pin groups, bus slot 4).  They are not a source of
AN7581 PCM register semantics.  In particular:

- `SPI_Reset(0)` in `spi.ko` pulses `0x1fb00834` bits 25 and 0; bit 0
  is the PCM block reset that wedged this board.  Only the ZSI reset
  bit 4 is used by the new driver.
- The OEM `pcm1.ko` reset value for `pcmCtrl` is `0x0500040a`, not the
  EN75xx default `0xf5051306`.
- The OEM descriptor layout is 12 bytes x 15 descriptors, not the
  EN75xx layout.

## Validated ZSI + PCM clock bring-up (2026-11-09)

ZSI control bytes are clocked over the PCM bus.  On a cold HG5582A boot
the PCM block is not generating PCLK/FSYNC, so a pure ZSI-wrapper init
reads back `ff ff` even though the wrapper handshake is correct.

The OEM `pcm1.ko:init_module()` path for HIR 14 does:

```text
0x1fa20218 |= 0x00100000        /* pcmChipScuQuery()[0] |= [8] */
0x1fa201d0  = 0x000c0001        /* pcmChipScuQuery()[1] = [6] | 1 */
```

`spi.ko:SPI_cfg()` additionally sets the PCM clock gate:

```text
0x1fa201c4 |= 0x00010000
```

`defaultConfigNodeSet()` + `timeSlotCfgInit()` + `pcmConfigSetup()` then
produce the default 32-slot/16-bit table and `pcmCtrl = 0x75021706`
(config-valid bit 26 set; the non-valid intermediate is `0x71021706`).

The exact sequence was validated on the live HG5582A twice:

1. On the running 6.18.55 image, after a cold reboot, a single
   `an7581-zsi.ko` load with `init_mode=0` programmed the PCM clock and
   returned `AN7581 ZSI ID: 08 49`.
2. After sysupgrade to the 6.18.60 voice-debug image, loading
   `/lib/modules/6.18.60/an7581-zsi.ko` again returned `08 49`.

The PCM clock setup currently lives in `an7581-zsi.c` as a bring-up
prerequisite; it will move to `an7581-pcm` once the DMA engine is added.

The proc write path had a separate bug: `strndup_user(ubuf, count)` gets
a write count without a NUL terminator, so every command returned
`-EINVAL`.  It now uses `memdup_user_nul(ubuf, count)`.

Observed register state after a successful load:

```text
0x1fa20218 = 0x00121000
0x1fa201d0 = 0x00000001   (bits 18/19 read back as 0)
0x1fa201c4 = 0x00010000
0x1fbd0000 = 0x75021706
0x1fb00094 = 0xe0004825   (low nibble 5)
0x1fbd1000 = 0x0f030313
0x1fbd1010 = 0x00000019
```

## PCM DMA loopback and Le9641 diagnostics (2026-11-09)

`an7581-pcm` now has a diagnostic DMA implementation based on the OEM
12-byte/15-descriptor ring:

- 15 TX + 15 RX descriptors, coherent DMA memory
- ring base `(phys & 0x3fffffff) | 0x80000000`
- `txrxRingSizeAndOff = 0x3f`, `0xa8 = 0xa0`
- descriptor `status = OWN | sample-size`; the OEM unit is **samples**,
  so 80 samples = 160 bytes at 16-bit
- TX kick `0x2c = 1`, RX kick `0x30 = 1`, `txRxDMA = 3`
- `/proc/an7581-pcm` commands: `test`, `start`, `loopback`, `stop`,
  `dump`

Validated with the internal PCM loopback bit (`pcmCtrl` bit 25):
301 RX frames were seen in ~3 s and the payload matched the TX pattern.
The PCM clock is 8 kHz frame / 80 samples per descriptor, i.e. 10 ms.

`an7581-slic-le9641` uses the OEM raw-MPI profiles and:

- detects `08 49`
- loads device/DC/AC/ring profiles (devmode reads `14`)
- sets OPFUNC linear (`0xbf`), TX slot 3, RX slot 4
- reads SIGREG and reports hook state

Known issue: the line state readback is `0x2f` (shutdown) and SWCTRL
reads `0x60` (switcher mode bits off) even after writing `0x23` active
and `0x6f` SWCTRL.  Charge-pump enable (`DEVMODE |= 0x08`) does not
change it.  Detection, profile load, PCM DMA and hook read all work;
line feed/ring/audio needs the missing OEM/VP-API initialisation
(switcher enable/calibration) before it can be called complete.

## Current module load order for testing

```sh
insmod an7581-zsi.ko          # PCM clock + ZSI wrapper
insmod an7581-pcm.ko          # DMA diagnostics
insmod an7581-slic-le9641.ko  # raw-MPI SLIC diagnostics
echo id > /proc/an7581-zsi    # expect 08 49
printf test > /proc/an7581-pcm
printf init > /proc/an7581-slic
printf hook > /proc/an7581-slic
```

The PCM clock setup still lives in `an7581-zsi`; it should move to
`an7581-pcm` once the latter owns the clock/reset lifecycle.

## SLIC profile: use IB, not BB

The OEM HG5582A boot script loads `slic3.ko` with the default `mode`
(IB), not BB.  The BB device profile sets `DEVMODE=0x14` (charge pump
off) and `SWCTRL=0x60`; the IB profile sets `DEVMODE=0x0c/0x80` (charge
pump on) and `SWCTRL=0x00`.  Switching the raw-MPI driver to
`DEV_PROFILE_90V_IB_124` (with the ZSI clock slot patched 0x40 -> 0x46)
loads `devmode=0x0c`.

Even with the IB profile, the Le9641 state still reads `0x2f`
(shutdown) and SWCTRL reads `0x60` after writing active/SWCTRL.  The
chip detects, profiles load, OPFUNC/TX/RX slots and the hook register
all work.  The remaining gap is the OEM VP-API device init/calibration
and switcher enable sequence, not the raw MPI profile bytes.

## Line state fix: SWCTRL LP + CALCTRL 0

The Le9641 stayed in shutdown because the raw driver wrote
`SWCTRL = 0x6f` (HP/HP) directly.  The OEM VP-API init instead leaves
the switcher in low-power mode and clears CALCTRL before setting the
state:

```text
DCFEED = 0x92 | ILA
SWCTRL = 0x05        (LP/LP)
CALCTRL = 00 00 00
STATE = 0x03/0x23    (active, codec)
```

With that order the live chip reads back:

```text
SWCTRL = 0x02
STATE  = 0x23       active + codec
```

Ring state (`STATE = 0x07`, no codec) reads back `0xa7` (ring active)
and returns to `0x04` (idle) on ring stop.  The hook read continues to
work (`SIGREG` byte 0 bit 0).  A 10-second 1 kHz PCM tone was run with
the line active; the hook stayed onhook because no phone was picked up,
but the PCM DMA, SLIC state and ring path are all live.

`/proc/an7581-pcm` now also accepts `tone` (10 s, 1 kHz, 16-bit linear).

## Single-FXS PCM slot mapping

The SLIC audio path is channel 0, SLIC TX slot 3 / RX slot 4.  The PCM
slot fields are in bit offsets, so the validated table is:

```text
TX slot 0 = 0x00001040   (enable + 4 * 16)
RX slot 0 = 0x00001030   (enable + 3 * 16)
other TX/RX slot registers = 0
```

ZSI detection still returns `08 49` with only the single audio slot
enabled; the ZSI control path does not depend on the other PCM slots.

The single-slot table (TX slot 4 / RX slot 3) was tested later: ZSI
detection still worked, but the PCM DMA loopback stopped completing
descriptors (`seen=0`).  The current driver therefore keeps the OEM
full 32-slot table; the SLIC channel-to-PCM-slot mapping still needs
the OEM `pcm_channel_num_set`/`pcm_chan_state_config` equivalent.

## Working PCM/ZSI audio and Asterisk (2026-11-09)

The audio path is now validated: PCM channel 0 at bus slot 4
(`0x10301020`), `pcmCtrl=0xf5071306`, SLIC tx=3/rx=4, OEM GR/VPGAIN.
A clean 1 kHz tone and a stable Asterisk `Echo()` were verified.
See `docs/an7581-voice-audio-sip.md` for the final register map, the
`/dev/an7581-slic` ioctl API, the `chan_an7581` Asterisk driver, PJSIP
templates, and the built image hash.  The earlier `demo-congrats` playback
was silent because that sound file is not installed.
