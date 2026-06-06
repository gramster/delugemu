# Register coverage

What each emulated device model actually implements, so you know which firmware
behaviour is real and which is faked. Keep this in sync with
[memory-map.md](memory-map.md) and the `RZA1L_*_BASE` defines in
[`src/include/hw/arm/rza1l_soc.h`](../src/include/hw/arm/rza1l_soc.h).

**Coverage legend**

- **modelled** — read and write have real, firmware-visible behaviour.
- **shadow** — value is stored and read back but has no side effects.
- **stub** — returns a fixed/constant value; writes are ignored or only stored.
- **special** — non-trivial behaviour (FIFO window, DMA-routed, IRQ line, etc.).

## MMIO devices

### SCIF — serial / UART · `rza1l-scif`

Base `0xE8007000` (SCIF0, MIDI) and `0xE8007800` (SCIF1, PIC) · size `0x100`.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00 | SCSMR | shadow | mode |
| 0x04 | SCBRR | shadow | bit rate |
| 0x08 | SCSCR | shadow | control; RIE gates RXI |
| 0x0C | SCFTDR | special | TX byte → chardev (`qemu_chr_fe_write_all`) |
| 0x10 | SCFSR | modelled | TDFE\|TEND always set; RDF/DR on RX |
| 0x14 | SCFRDR | special | RX holding byte; clears rx_full |
| 0x18 | SCFCR | shadow | FIFO control |
| 0x1C | SCFDR | stub | data count, returns 0 or 1 |
| 0x20–0x28 | SCSPTR/SCLSR/SCEMR | stub | |

TX is synchronous to the chardev; RX is interrupt-driven (RXI when `SCSCR.RIE`).

SCIF0 (MIDI) additionally has its receive path bound to DMAC channel 13
(`MIDI_RX_DMA_CHANNEL`): bytes arriving from the host `-serial`/`--midi` chardev
are pushed straight into the channel's self-linking receive ring (the firmware
reads MIDI by polling the channel's `CRDA`), mirroring the PIC's DMA receive on
channel 12. Each received MIDI byte also triggers the receive-timing capture
path (DMA channel 14, `MIDI_RX_TIMING_DMA_CHANNEL`), which snapshots the SSI
sample counter into its own self-linking ring so the firmware can timestamp
incoming MIDI events.

### DMAC — `rza1l-dmac`

Base `0xE8200000` · size `0x800` · 16 channels (0x40-byte blocks), group commons at `0x300`/`0x700`.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| +0x00/04/08 | N0SA/N0DA/N0TB | shadow | next-0 src/dst/count |
| +0x0C–0x14 | N1SA/N1DA/N1TB | shadow | link descriptor |
| +0x18/1C/20 | CRSA/CRDA/CRTB | special | current regs; CRSA advances at audio rate on the TX ring |
| +0x24 | CHSTAT | modelled | EN\|TACT\|ER\|END\|TC |
| +0x28 | CHCTRL | special | write-to-act: SETEN runs the transfer |
| +0x2C | CHCFG | shadow | SAD/DAD/SDS/DDS/DEM |
| +0x30–0x3C | CHITVL/CHEXT/NXLA/CRLA | shadow | |
| 0x300/0x700 | DCTRL_G0/1 | shadow | |
| 0x310–0x320 | DSTAT_EN/ER/END/TC/SUS | modelled | computed from per-channel state |

On `CHCTRL.SETEN` it synchronously copies `N0TB` bytes `N0SA → N0DA` honouring
addr modes, then sets `END|TC` and pulses the per-channel DMAINT (if `DEM` clear).
Channel 6 is a self-advancing audio TX ring; channel 12 is the PIC RX ring.

### SDHI — SD host · `rza1l-sdhi`

Base `0xE804E800` · size `0x100` · IRQ GIC SPI 273 (INTC 305).

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00 | SD_CMD | modelled | issues command on the QEMU SD bus |
| 0x04/06 | SD_ARG0/1 | modelled | argument |
| 0x08/0A | SD_STOP/SECCNT | shadow | |
| 0x0C–0x1A | SD_RESP0–7 | modelled | latched response |
| 0x1C | SD_INFO1 | modelled | RESP/DATA/card-detect events |
| 0x1E | SD_INFO2 | modelled | error/busy status |
| 0x20/22 | SD_INFO1/2_MASK | modelled | inverted-logic masks |
| 0x24–0x2E | CLK_CTRL/SIZE/OPTION/ERR_STS | shadow | |
| 0x30 | SD_BUF0 | special | data FIFO |
| 0x00–0x3F | (DMA window) | special | when `CC_EXT_MODE.DMASDRW`, the FIFO is mirrored over the low 64 bytes |
| 0xD8 | CC_EXT_MODE | shadow | bit 1 = DMASDRW |
| 0xE0 | SOFT_RST | special | write-to-act |
| 0xE2 | SD_VERSION | stub | 0xB001 |
| 0xF0 | EXT_SWAP | shadow | endian |

### OSTM — OS timer · `rza1l-ostm`

Base `0xFCFEC000` · size `0x800` · two channels (OSTM0 @ +0x000, OSTM1 @ +0x400) · 33.33 MHz.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| +0x00 | CMP | shadow | compare/period |
| +0x04 | CNT | modelled | free-running counter off the virtual clock |
| +0x10 | TE | modelled | enable status |
| +0x14 | TS | special | write starts / re-bases the counter |
| +0x18 | TT | special | write stops / freezes the counter |
| +0x20 | CTL | shadow | bit 1 = free-run vs interval |

### MTU2 — multi-function timer · `rza1l-mtu2`

Base `0xFCFF0000` · size `0x400` · 33.33 MHz.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x006/0x306/0x386 | TCNT_2/0/1 | modelled | free-running counters |
| 0x210/0x212/0x220 | TCNT_3/4/S | modelled | free-running counters |
| (others) | TCR/TMDR/TIOR… | shadow | no compare-match or interrupts |

Reads return the current value; writes re-base the counter. No prescaler / compare / interrupt modelling.

### GPIO — `rza1l-gpio`

Base `0xFCFE3004` · size `0x4F00` · shadow byte array, no symbolic register defines.

| Offset range | Reg | Coverage | Notes |
| ------------ | --- | -------- | ----- |
| 0x000–0x04C | P_n | shadow | output latch |
| 0x100–0x12C | PSR_n | shadow | |
| 0x200–0x22C | PPR_n | special | read-only; loops back the P_n latch (undriven = 0) |
| 0x300–0x54C | PM/PMC/PFC_n | shadow | |
| 0x4000–0x424B | PIBC/PBDC/PIPC | shadow | |

Pure register shadow with loopback. No pin multiplexing, edge detection, or bidirectional buffers.

### RSPI0 — SPI · `rza1l-rspi`

Base `0xE800C800` · size `0x100` · SPRI IRQ GIC SPI 239 (INTC 271).

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00 | SPCR | shadow | bit 7 = SPRIE |
| 0x03 | SPSR | stub | always SPTEF\|TEND (0x60) |
| 0x04 | SPDR | special | TX low byte → OLED panel; RX reads 0; write raises level-sensitive SPRI if SPRIE |

### SPIBSC — SPI multi-I/O flash controller · `rza1l-spibsc`

Base `0x3FEFA000` · size `0x100` · shadow array with special status.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00–0x34 | CMNCR/SSLDR/SPBCR/DRCR/DR*…/SMCR/SM* | shadow | DRCR.SSLN negates SSL; SMCR.SPIE starts, SSLKP keeps SSL |
| 0x38/0x3C | SMRDR0/1 | stub | read 0 (flash not busy / WIP clear) |
| 0x40/0x44 | SMWDR0/1 | shadow | |
| 0x48 | CMNSR | special | TEND always 1; SSLF tracks the SSL line |
| 0x50–0x68 | CKDLY/DR*/SM*/SPODLY | shadow | |

Manual-mode transfers complete instantly; no backing flash.

### SSIF0 — serial sound interface (I²S) · `rza1l-ssif`

Base `0xE820B000` · size `0x800` · IRQs SSII0/SSIRXI0/SSITXI0 (GIC SPI 140/141/142 ← INTC 172/173/174), wired but quiescent.

The firmware streams audio through DMA (ch6 TX → SSIFTDR, ch7 RX ← SSIFRDR) and
tracks playback from the DMA source address, so the FIFOs are never CPU-serviced.

With an audio backend bound (`-audiodev …,id=deluge0 -global rza1l-ssif.audiodev=deluge0`,
e.g. via `run.sh --audio`), the device opens a 44.1 kHz stereo S32 output voice
and mirrors the transmit DMA ring from guest memory to the host (silence until
the firmware arms the ring). Without an audiodev the audio path is inactive.

The receive channel is modelled symmetrically: ch7's CRDA advances from virtual
time at the sample rate (so the firmware's input-latency resync tracks a live
write pointer), and an input voice writes captured frames into the RX ring at
CRDA. With no capture source the input is silence.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00 | SSICR | shadow | control (TEN/REN etc.) |
| 0x04 | SSISR | shadow | no under/overrun modelled |
| 0x0C | SSIFCR | shadow | FIFO control; reset = TFRST\|RFRST |
| 0x10 | SSIFSR | stub | reads TDE (TX FIFO empty/ready); RX flags clear |
| 0x14 | SSIFTDR | special | TX data absorbed (delivered via DMA) |
| 0x18 | SSIFRDR | stub | reads 0 (RX data delivered via DMA) |
| 0x1C | SSITDMR | shadow | TDM mode |
| 0x20 | SSIFCCR | shadow | FIFO clock control |
| 0x24 | SSIFCMR | shadow | FIFO clock measure |
| 0x28 | SSIFCSR | shadow | FIFO clock status |

### USB200 / USB201 — USB 2.0 host/function · `rza1l-usb`

Base `0xE8010000` (USB200, used) and `0xE8207000` (USB201) · size `0x200` ·
IRQs USBI0/USBI1 (GIC SPI 41/42 ← INTC 73/74).

The firmware brings up the Renesas USB stack at start-up in host mode. Two
modes are modelled, selected by the `midi` property (default off):

**Disconnected (default).** No device is attached, mirroring an unplugged
cable:

- SYSSTS0.LNST = 00 (SE0). The firmware's synchronous attach probe in
  `hw_usb_hmodule_init` sees no device and falls back to peripheral mode.
- Configuration registers (SYSCFG0, BUSWAIT, DVSTCTR0, the FIFO/pipe selectors,
  INTENB0/1, …) are shadowed; status registers read back zero, so the USBIn
  line stays deasserted.

**Attached USB-MIDI device** (`-global rza1l-usb.midi=on`). The controller
presents a permanently-attached full-speed USB-MIDI device on the host port and
drives the firmware's host enumeration to completion:

- SYSSTS0.LNST reports FS-J from boot, so the firmware's synchronous attach
  probe detects the device, performs the inline bus reset (USBRST→UACT, HSPROC
  poll), and stays in host mode.
- Writing INTENB1.ATTCH with a device present raises a one-shot ATTCH interrupt,
  which kicks the MGR task into enumeration.
- Control transfers over the DCP (pipe 0) are answered with synthetic
  descriptors: an 18-byte device descriptor, a 101-byte configuration
  descriptor describing an AudioControl + MIDIStreaming interface pair (one
  MIDI IN jack, one MIDI OUT jack, bulk IN/OUT endpoints 0x81/0x01), and the
  LANGID string. The model generates the SACK / BRDY / BEMP interrupts the
  firmware's control-transfer state machine expects, including the multi-packet
  BRDY re-arm for the 101-byte read and the IN/OUT status-stage handshakes.
- The firmware completes GET_DESCRIPTOR → SET_ADDRESS → GET_DESCRIPTOR(full
  config) → SET_CONFIGURATION, recognises the audio-class interface as a
  USB-MIDI device, and sets up its bulk MIDI pipes.
- Once configured, raw MIDI on the device's data chardev (`chardev` property /
  `-serial chardev:` slot 1) is bridged over those bulk pipes in both
  directions (see *Bulk MIDI data bridge* below).

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00 | SYSCFG0 | shadow | module/clock enable; reads back written bits |
| 0x02 | BUSWAIT | shadow | bus wait cycles |
| 0x04 | SYSSTS0 | model | LNST = SE0 (disconnected) or FS-J (midi attached) |
| 0x08 | DVSTCTR0 | model | bus reset / UACT / speed; RHST reports FS after reset |
| 0x14 | CFIFO | model | DCP control data port **and** bulk pipe data port (CURPIPE-routed) |
| 0x20–0x22 | CFIFOSEL/CTR | model | FIFO selector (CURPIPE/ISEL) + FRDY/DTLN; BVAL/BCLR commit |
| 0x30/0x32 | INTENB0/1 | model | interrupt enables; INTENB1.ATTCH arms attach |
| 0x36–0x3A | BRDY/NRDY/BEMPENB | model | per-pipe interrupt enables (re-evaluate IRQ on write) |
| 0x40/0x42 | INTSTS0/1 | model | INTSTS0 BRDY/NRDY/BEMP are live summaries |
| 0x46–0x4A | BRDY/NRDY/BEMPSTS | model | write-0-to-clear per-pipe status |
| 0x54–0x5A | USBREQ/VAL/INDX/LENG | model | latched control SETUP packet |
| 0x5C–0x60 | DCPCFG/MAXP/CTR | model | SUREQ triggers SETUP; CCPL/PID handshakes |
| 0x64/0x68 | PIPESEL/PIPECFG | shadow | bulk-pipe setup absorbed |
| 0x70–0x80 | PIPE1–9CTR | model | PID=BUF arms a pipe; INBUFM reads 0 (send-complete gate) |
| others | pipe/DCP regs | shadow | absorbed |

**Bulk MIDI data bridge.** The Deluge's customised ("rohan") USB driver does not
use the D0/D1 FIFOs for MIDI — both directions stream 32-bit USB-MIDI event
packets through the shared **CFIFO** with the active pipe selected on
`CFIFOSEL.CURPIPE`. The model routes CFIFO accesses by the selected pipe:

- *Receive* (host → device, bulk IN, pipes 2–5). Raw MIDI from the chardev is
  framed into 4-byte USB-MIDI packets (running-status aware, with SysEx
  continuation) and queued. When the firmware arms a receive pipe (`PIPEnCTR`
  PID=BUF) with its `BRDYENB` bit set, the model latches up to 64 bytes,
  reports the byte count via `CFIFOCTR.DTLN`, hands the packets out on CFIFO
  reads, and raises that pipe's **BRDY**.
- *Transmit* (device → host, bulk OUT, pipe 1). The firmware writes packets to
  CFIFO and commits the buffer with `CFIFOCTR.BVAL`; the model deframes them
  (per the USB-MIDI CIN→length table) back to raw MIDI on the chardev and
  raises pipe 1's **BEMP** send-complete interrupt. Because the firmware enables
  `BEMPENB` *after* committing the buffer, enable-register writes re-evaluate
  the level-triggered interrupt so the already-latched completion fires and
  multi-transfer sends (e.g. a SysEx reply spanning several 8-byte chunks)
  continue. `PIPE1CTR.INBUFM` reads back 0 so the firmware's BEMP handler
  invokes its send-complete path.

USB mass-storage device emulation remains out of scope.

### ADC — S12AD battery sense · `rza1l-adc`

Base `0xE8005800` · size `0x100`.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x0A | ADDRF | stub | channel-5 result; fixed `0x9000` (~3.7 V) |
| 0x60 | ADCSR | stub | bit 15 (ADST) always reads set ⇒ "conversion ready" |

### RTC — `rza1l-rtc`

Base `0xFCFF1000` · size `0x40` · shadow array seeded with a fixed time.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x02–0x0F | RSECCNT…RYRCNT | stub | fixed BCD 2024-01-01 00:00:00 Monday; does not advance |
| 0x00, 0x10–0x26 | R64CNT / alarms / RCR* | shadow | accept writes |

### CPG — clock pulse generator · `rza1l-cpg`

Base `0xFCFE0010` · size `0x1000` · shadow array.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00/0x04 | FRQCR/FRQCR2 | shadow | PLL/divider config |
| 0x08 | CPUSTS | stub | always 0 ("running") |
| 0x10–0x2C | STBCR1–10 | shadow | module standby |

### BSC — bus state controller · `rza1l-bsc`

Base `0x3FFFC000` · size `0x3000` (incl. SDRAM mode-set windows at +0x1040/+0x2040) · shadow array.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00–0x58 | CMNCR/CSnBCR/CSnWCR/SDCR/RTCSR/RTCNT/RTCOR | shadow | SDRAM bring-up sequence accepted |
| 0x1040, 0x2040 | mode-set windows | shadow | |

Backing SDRAM is created separately by the SoC; this only absorbs the init writes.

### WDT — watchdog · `rza1l-wdt`

Base `0xFCFE0000` · size `0x10`.

| Offset | Reg | Coverage | Notes |
| ------ | --- | -------- | ----- |
| 0x00 | WTCSR | special | keyed write (0xA5 prefix); low byte stored |
| 0x02 | WTCNT | special | keyed write (0x5A prefix); low byte stored |
| 0x04 | WRCSR | shadow | |

Never expires — the firmware's periodic kick is absorbed.

### Built-in QEMU models

| Device | Base | Model | Notes |
| ------ | ---- | ----- | ----- |
| INTC → GIC | dist `0xE8201000`, cpu `0xE8202000` | `arm_gic` (rev 1) | RZ/A1 INTC register layout matches the GICv1, mapped directly |
| PL310 L2 | `0x3FFFF000` | `l2x0` | cache ops report complete (no-op) |

## Board-level devices (no MMIO)

These attach via a chardev, GPIO, or graphic console rather than a physical
window, so they have no register map.

| Device | Type | Attaches via | Role |
| ------ | ---- | ------------ | ---- |
| Input PIC | `chardev-deluge-pic` | SCIF1 chardev + DMA ch10/12 | pad/button/encoder ↔ LED/OLED command protocol |
| OLED | `deluge-oled` | RSPI0 SPI + PIC control lines | SSD130x decoder, 128×48 framebuffer |
| Pad grid | `deluge-padgrid` | PIC colour messages | 18×8 RGB pads |
| 7-segment | `deluge-segment` | PIC command 224 | 4-digit numeric display |
| Host input | `deluge-input` | QEMU keyboard handler | maps host keys to PIC pad/button events |

## Intentionally unmodelled peripherals

These on-chip blocks fall inside the logging catch-all windows and need no
device model because the firmware never clocks them out of module-standby:

| Block | Window | Why unmodelled |
| ----- | ------ | -------------- |
| SCUX (sample-rate converter) | `0xE8208000` | Held in standby (`STBCR8`); the firmware resamples in software and streams I²S directly via SSIF0/DMA |
| RIIC0–3 (I²C) | `0xFCFEE000`–`0xFCFEEFFF` | Held in standby (`STBCR9`); the codec is hardware-strapped and enabled by a GPIO pin, with no I²C configuration |
