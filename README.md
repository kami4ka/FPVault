<p align="center"><img src="docs/img/logo.png" alt="FPVault" width="460"></p>

English | [Українська](README.uk.md)

Open-source airborne FPV DVR firmware for the Allwinner F1C200s.

Records an analog CVBS camera to SD card as MJPEG-in-AVI with near-zero CPU
load: every stage of the pipeline is a hardware peripheral, and the ARM926
core only orchestrates.

![FPVault pipeline](docs/img/pipeline.svg)

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
  card mounts as USB mass storage ("FPVault SD Card"); no card removal, no
  extra files on the card. Powered by anything that is not a computer
  (charger, FC 5 V), it records instead.
- **Firmware update over the same USB cable** — the board also shows up as
  a standard DFU device; `dfu-util -D fpvault.bin` stages, verifies, burns
  NOR and reboots. No buttons, no serial adapter, nothing on the card.
- **A desktop app that needs nothing installed** — [FPVault
  Desktop](desktop/README.md) imports and repairs clips, gives them real
  dates the board has no clock to provide, joins or exports a session, plays
  them frame by frame, and installs firmware over USB.

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
- [x] M7 — endurance and fault-injection hardening: **80-minute
      uninterrupted soak, 16 consecutive 5-min segments, 144,000 frames,
      0 drops, every JPEG verified**; crash safety proven against 8 real
      unplanned power cuts (every clip playable to within 1 frame of the
      cut); stall injection passes. The soak also produced the supply
      errata in docs/HARDWARE-ERRATA.md — the bench board needed a bulk
      capacitor at the SD socket to survive soft supplies.
- [x] M8 — standalone SPI-NOR boot: cold power → recording in ~5 s
      (U-Boot with baked-in bootcmd at NOR 0, firmware at NOR 0x100000)
- [x] M9 — USB mass storage: connect to a computer, the card mounts
      (CherryUSB device stack on the MUSB controller, USB 2.0 High-Speed)
- [x] M10 — **FPVault Desktop**, the companion app in `desktop/`: imports
      clips and repairs the ones a power cut truncated, gives them real
      timestamps, joins or exports a session, plays them frame-accurately,
      and updates firmware over USB with animated help for the parts that
      need hands on the board
- [x] M11 — **USB camera**: plugged into a computer the board is a webcam as
      well as a card reader, streaming the same hardware-encoded MJPEG the
      recorder writes. Measured 30 fps, 0 drops, ~690 KB/s, with the card
      mounted and readable at full speed throughout. Nothing to switch on:
      UVC streaming is host-initiated, so it starts when an application
      opens the camera

Power the board with a card inserted and it records — no host, no
commands.

Host test suite: `make -C tests/host` (no cross-toolchain needed).

## Hardware

### FPVault board v1

![FPVault board v1](docs/img/board-v1.jpg)

The purpose-built DVR board, 2-layer, four mounting holes. It sits inline
in the video link: `CVBS_IN` from the camera, `CVBS_OUT` to the VTX. The
bypass is analog — a THS7374 video amplifier feeds a TS5A3153 switch that
passes the camera straight to the output with the firmware doing nothing
(PE4 at its pull-down default), and the same amplifier taps the picture
for the F1C200s's decoder. Power comes from USB-C or a `5V_IN` pad
through a TPS2116 mux; `RX`/`TX` pads expose UART0. Same F1C200s + 16 MB
W25Q128 NOR + microSD + EA3059C core as the dev board, with SW1 reset and
SW2 for FEL (see docs/BRINGUP.md). Two units brought up and recording;
findings in docs/HARDWARE-ERRATA.md.

Schematic: [PDF](docs/hw/fpvault-board-v1-schematic.pdf) ·
[PNG](docs/hw/fpvault-board-v1-schematic.png)

### Development board

![development board](docs/img/dev-board.jpg)

The generic F1C200s module everything was first brought up on: F1C200s
(under the heatsink), W25Q128 SPI-NOR, USB-C, microSD, CVBS `TV IN` pins
and a `TV_OUT` header, EA3059C PMIC. Its quirks are the first part of
docs/HARDWARE-ERRATA.md.

## Building

Needs `arm-none-eabi-gcc` (tested with 14.2) and GNU make.

```sh
make            # build/fpvault.bin
make deploy     # send to a board sitting at the U-Boot prompt (YMODEM)
make dfu        # update a running board over USB (dfu-util)
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

## Preparing a card

The firmware cannot format a card — FatFs is built without `f_mkfs` — so the
card has to arrive ready. It also never writes anything to the card but
clips, and it expects the same courtesy back.

### Format it FAT32, with an MBR

| | |
|---|---|
| Filesystem | **FAT32**. exFAT is compiled out (`FF_FS_EXFAT 0`) |
| Partitioning | **Master Boot Record**, or none. GPT is not read |
| Names | 8.3 uppercase; long filenames are compiled out (`FF_USE_LFN 0`) |
| Size | Anything up to FAT32's limits. Clips run ~2.4 GB/hour, so 32 GB is about 13 hours |
| Speed | Any Class 10 / U1 card. Recording needs under 1 MB/s; the path does 7.8 |

The format matters most for larger cards. The SD specification assigns exFAT
to SDXC — every card over 32 GB — and FAT32 only to SDHC, so a new 64 GB card
will be exFAT out of the packet and the board will not mount it. Cards of
32 GB and under are usually already FAT32 and can go straight in.

macOS, replacing `diskN` with the card from `diskutil list`:

```sh
diskutil list                                          # find the card, carefully
diskutil eraseDisk FAT32 FPVAULT MBRFormat /dev/diskN
```

Linux, replacing `sdX`:

```sh
sudo parted /dev/sdX mklabel msdos
sudo parted -a optimal /dev/sdX mkpart primary fat32 1MiB 100%
sudo mkfs.vfat -F 32 -n FPVAULT /dev/sdX1
```

Windows Disk Management will not offer FAT32 above 32 GB; use `format /FS:FAT32`
from an elevated prompt, or format the card on another machine.

### Erase any old boot blob

This one bites hardest and looks like a dead board. The F1C200s BROM checks
the SD card for a boot header **before** it falls back to SPI-NOR, so a card
that has ever held an Allwinner image — Armbian, a LicheePi image, anything
written with `dd` — will hijack the boot and the board will not start its own
firmware. Reformatting does not remove it: the header lives at sector 16, in
the gap before the first partition, which no filesystem touches.

The board can clear it itself. On the console, `:M` to mount, then `:Z`:

```
[sd] partition 0 starts at LBA 2048
[sd] STALE BOOT BLOB FOUND (eGON at sector 16) - scrubbing
[sd] scrubbed sectors 16..47 - the card can no longer hijack boot
```

`:Z` refuses to run if the first partition starts before LBA 48, because
then there is no gap and sectors 16–47 belong to the filesystem. That is
another reason to use an MBR rather than a partitionless card — a normal
partition table leaves the gap.

### Keep it to clips

Nothing but recordings should live on the card. macOS in particular writes
`.Spotlight-V100`, `.fseventsd`, `.Trashes` and `._` files to any volume it
mounts, and a card that has been plugged into a Mac will collect them.

```sh
mdutil -i off /Volumes/FPVAULT                 # stop Spotlight indexing it
touch /Volumes/FPVAULT/.metadata_never_index   # and keep it stopped
mkdir -p /Volumes/FPVAULT/.fseventsd && touch /Volumes/FPVAULT/.fseventsd/no_log
defaults write com.apple.desktopservices DSDontWriteUSBStores -bool true
dot_clean /Volumes/FPVAULT                     # merge away existing ._ files
```

### What the board does for itself

Nothing needs creating by hand. On the first recording the firmware builds
the DCF tree itself:

```
/DCIM/100FCDVR/FCDV0001.AVI
/DCIM/100FCDVR/FCDV0002.AVI
/DCIM/101FCDVR/FCDV0003.AVI     <- next power-on, next directory
```

A new directory per power-up groups clips by session without needing a clock.
File numbers are monotonic across the whole card and are never reused, so no
two clips can share a name even after deletions — the boot-time scan of the
card is the only authority, which means the numbering self-heals after a card
swap or a manual cleanup.

At `FCDV9999` or directory `999` the recorder stops rather than overwrite
anything, and asks for a cleanup or a reformat.

## License

GPL-3.0-or-later. See [CREDITS.md](CREDITS.md) for the vendored and derived
components and their origins.
