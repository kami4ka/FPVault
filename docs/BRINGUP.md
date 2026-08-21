# Bring-up

English | [Українська](BRINGUP.uk.md)

## Bench setup

- Board at the U-Boot prompt (U-Boot lives on SPI-NOR; the DVR binary is
  RAM-loaded during development, so a power cycle always returns to U-Boot).
- USB serial on UART0 (PE0/PE1), 115200 8N1. Default port in the tooling:
  `/dev/cu.usbserial-0001` (override with `PORT=`).
- CVBS camera on TV_IN as a **high-impedance parallel tap** off the camera
  line. Do not terminate: exactly one 75 Ω per link, and it belongs at the
  VTX/display end (double termination halves the amplitude and the AGC
  hides it — measured the hard way in the predecessor project).
- SD card in SDC0. FAT32.

## Deploy

```sh
make            # build/fpvault.bin
make deploy     # YMODEM to 0x80000000 + go (tools/loader.py)
```

Console commands (single characters): `s` state, `r` watchdog reset,
`v` VE info, `q` cycle JPEG quality 50/75/90, `m` cycle ISP input format,
`J` encode test pattern (timing only), `j` encode + base64 JPEG dump.

## Flashing over USB — FEL (blank or bricked board, no UART)

FEL is the recovery mode inside the SoC's mask ROM: when the BROM finds no
bootable image, it enumerates as a USB device on the same USB-C that powers
the board, and accepts uploads. That makes a just-soldered board flashable
with nothing but a USB cable:

- **Blank NOR** (fresh board): plug into a computer — the BROM falls
  through to FEL by itself.
- **Occupied or bricked NOR**: hold **SW2 while plugging in**. SW2 shorts
  a NOR pin, so the BROM's SPI probe fails and it lands in FEL; release
  the button once the device enumerates (~1 s). The flash itself is
  unharmed — the short only blinds the BROM's probe.

Host side needs [sunxi-tools](https://github.com/linux-sunxi/sunxi-tools)
(`brew install sunxi-tools` / `apt install sunxi-tools`). Verify the link:

```sh
sunxi-fel ver        # expect: AWUSBFEX soc=00001663 (F1C100s/F1C200s)
```

Write both images (U-Boot at 0, firmware at 1 MB):

```sh
sunxi-fel -p spiflash-write 0        u-boot-sunxi-with-spl.bin
sunxi-fel -p spiflash-write 0x100000 build/fpvault.bin
```

Power-cycle: the board boots into recording in ~5 s. UART never needed —
though once U-Boot is on NOR, the serial `make deploy` flow above is the
faster loop for iterating on firmware.

Both binaries are attached to the project's GitHub Releases. To build
U-Boot from source instead (mainline v2026.07 + the two files in
`uboot/`):

```sh
git clone --depth 1 -b v2026.07 https://source.denx.de/u-boot/u-boot.git
cp uboot/f1c200s_dvr_defconfig          u-boot/configs/
cp uboot/suniv-f1c200s-video-board.dts  u-boot/dts/upstream/src/arm/allwinner/
make -C u-boot f1c200s_dvr_defconfig
make -C u-boot CROSS_COMPILE=arm-none-eabi- -j8
# result: u-boot/u-boot-sunxi-with-spl.bin
```

The defconfig carries the whole boot story: `CONFIG_BOOTCOMMAND="sf probe;
sf read 0x80000000 0x100000 0x40000; go 0x80000000"`, 1 s autoboot delay.

## M1 — VE first light (go/no-go)

The one genuinely open silicon question: does the Cedar VE's JPEG encoder
respond on suniv the way it does on A10/A20 (jepoc)? Everything else in the
product is proven ground.

1. `make deploy` — the banner prints `[ve] version XXXXXXXX`. Record it.
   All-zeros or all-ones = clocking/reset problem; anything else is the ID
   (top 16 bits) and first light.
2. `python3 tools/vedump.py /dev/cu.usbserial-0001 --out ve.jpg`
   — sends `j`, captures the base64 dump, decodes with PIL.
3. Pass: the JPEG opens and shows 8 color bars over a luma ramp, correct
   colors (a U/V swap shows red↔blue-ish tints), 720×480, 20–150 KB,
   encode time printed in µs (expect single-digit ms).
4. Sweep `q` and re-dump; sizes must track quality. Try `m` to probe the
   NV16 format-value question (1 vs 2 — the references disagree).

If status never leaves 0 or reads "failed": re-check `v`, try the alternate
sub-engine select values (see ve.h), then smaller resolutions. The fallback
ladder is in the plan; the decision lands before anything else is invested.

## Troubleshooting

- **Silent console**: board unpowered, or U-Boot never started (SPI-NOR
  erased?). FEL over USB is the recovery path (sunxi-tools).
- **loader.py "no '=>' prompt"**: something else is running — press reset,
  or if a previous DVR/passthru build is live, its `r` command reboots to
  U-Boot.
- **YMODEM stalls with an ESP32 bridge inline**: loader.py already holds
  DTR/RTS low to avoid resetting the bridge; check the bridge's own power.
