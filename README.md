# f1c200s-dvr

English | [Українська](README.uk.md)

Open-source airborne FPV DVR firmware for the Allwinner F1C200s.

Records an analog CVBS camera to SD card as MJPEG-in-AVI with near-zero CPU
load: every stage of the pipeline is a hardware peripheral, and the ARM926
core only orchestrates.

```
CVBS camera ──> TVD (TV decoder, own DMA) ──> DDR (semi-planar YUV422 ring)
                                                   │
                                                   ▼
                                    Cedar VE hardware JPEG encoder
                                                   │  bitstream ring
                                                   ▼
                              SD card, 4-bit @ 50 MHz + descriptor DMA
                                                   ▼
                            /DCIM/100FCDVR/FCDV0001.AVI  (DCF naming)
```

- **Bare metal** — no Linux, no RTOS. One binary, single main loop, a handful
  of interrupts.
- **Hardware MJPEG** — the F1C200s Video Engine encodes up to 1280×720@30
  (datasheet §2.5); the analog D1 frames (720×480/576) it records here are
  well inside that.
- **Flight-controller control** — implements the device side of the RunCam
  Device Protocol v1.0, so Betaflight, INAV and ArduPilot can start/stop/
  toggle recording out of the box. A GPIO/RC-PWM pin is the FC-less fallback.
- **Crash-safe by construction** — auto-record on video signal, 5-minute
  segments, preallocated files, periodic AVI header refresh: pulling the
  battery costs at most the last second of the current clip.
- **Record-only tap** — the DVR taps the camera line in parallel and adds
  nothing to the video chain. Tap high-impedance: the DVR input must NOT
  terminate the line; exactly one 75 Ω per link, and it belongs at the
  VTX/display end.
- **USB card reader built in** — plug the board into a computer and the SD
  card mounts as USB mass storage ("DVR SD Card"); no card removal, no
  extra files on the card. Powered by anything that is not a computer
  (charger, FC 5 V), it records instead.

## Status

Early bring-up. Milestones:

- [x] M0 — skeleton: build, boot via U-Boot, console, LED, watchdog
- [x] M1 — Cedar VE hardware JPEG: **2.9 ms/frame on silicon** (VE 1663)
- [x] M2 — SD 4-bit + FatFs: **7.8 MB/s sustained, 64 MB bit-verified**
- [x] M3 — live capture → encode: **full 29.97 fps, IRQ pipeline, 0 drops**
- [x] M4 — crash-safe AVI recording with true color (TVD 4:2:0 → NV12)
- [x] M5 — autonomous recorder: auto-record on signal, 5-min segments,
      DCF naming, dropout policy, LED state UX
- [x] M6 — RunCam Device Protocol live on UART1 (Betaflight/INAV/ArduPilot)
      — *bench-tested against the fuzz suite; real-FC session pending*
- [ ] M7 — endurance and fault-injection hardening — *stall injection and
      power-cut recovery pass; hour-soak clip verification pending*
- [x] M8 — standalone SPI-NOR boot: cold power → recording in ~5 s
      (U-Boot with baked-in bootcmd at NOR 0, firmware at NOR 0x100000)
- [x] M9 — USB mass storage: connect to a computer, the card mounts
      (CherryUSB device stack on the MUSB controller, Full-Speed for now)

Power the board with a card inserted and it records — no host, no
commands.

Host test suite: `make -C tests/host` (no cross-toolchain needed).

## Hardware

![development board](docs/img/dev-board.jpg)

The development board everything above was brought up on: F1C200s (under
the heatsink), 16 MB W25Q128 SPI-NOR, USB-C (power + the mass-storage
card reader), microSD socket, CVBS `TV IN` pins and a `TV_OUT` header,
EA3059C PMIC. Quirks of this revision are collected in
docs/HARDWARE-ERRATA.md.

A proper DVR board is in development: an analog bypass so the unit can
sit inline in the video link (camera in, VTX out — the firmware stays a
record-only tap; the bypass is passive analog), plus mounting holes and
real soldering pads.

## Building

Needs `arm-none-eabi-gcc` (tested with 14.2) and GNU make.

```sh
make            # build/dvr.bin
make deploy     # send to a board sitting at the U-Boot prompt (YMODEM)
```

The dev flow expects U-Boot on the board's SPI-NOR: `loady 0x80000000`,
then `go 0x80000000` — `make deploy` (tools/loader.py) does both. The serial
port defaults to `/dev/cu.usbserial-0001`; override with `make deploy
PORT=...`. Console: 115200 8N1 on UART0 (PE0/PE1), single-character commands,
`s` = state, `r` = reset.

## Hardware requirements

Any F1C200s board with: CVBS input to TV_IN, SD card on SDC0 (PF0–PF5,
4-bit), SPI-NOR on SPI0, UART0 console. UART1 (PA2/PA3) connects to the
flight controller for RunCam control. 64 MB (F1C200s) required — the DMA
arena does not fit the 32 MB F1C100s.

## License

GPL-3.0-or-later. See [CREDITS.md](CREDITS.md) for the vendored and derived
components and their origins.
