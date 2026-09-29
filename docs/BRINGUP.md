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

## Firmware update over USB — DFU (the normal way)

A board that already runs FPVault updates itself over the same USB-C it
uses as a card reader. It enumerates as a composite device: the mass-
storage interface plus a DFU 1.1 interface named "FPVault firmware".
Host side needs [dfu-util](https://dfu-util.sourceforge.net/)
(`brew install dfu-util` / `apt install dfu-util`, Windows builds exist):

```sh
dfu-util -D fpvault.bin        # or: make dfu
```

What happens: the image is staged in DRAM, checked (size, load header),
burnt to the NOR firmware slot at 1 MB, read back and compared, and only
then does the board reboot into it — `[boot] previous reset: requested`
on the console. Nothing touches the flash until the whole image has
arrived, so a cable pull mid-transfer changes nothing. The second or so
of the burn itself is the only time a power cut can hurt; FEL below is
the way back if it does.

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

Host side needs [sunxi-tools](https://github.com/linux-sunxi/sunxi-tools).
Debian/Ubuntu: `apt install sunxi-tools`. macOS has no Homebrew formula;
build from source (needs `brew install libusb pkgconf`):

```sh
git clone --depth 1 https://github.com/linux-sunxi/sunxi-tools.git
cd sunxi-tools && make tools && make install-tools PREFIX=/opt/homebrew
```

Verify the link:

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

Both binaries are attached to the project's GitHub Releases. From v0.9.8 their
names carry the release tag - `fpvault-v0.9.8.bin`,
`u-boot-sunxi-with-spl-v0.9.8.bin` - so a downloaded file says which version
it is; substitute those names in the commands here. To build
U-Boot from source instead (mainline v2026.07 + the two files in
`uboot/`):

```sh
git clone --depth 1 -b v2026.07 https://source.denx.de/u-boot/u-boot.git
cp uboot/f1c200s_dvr_defconfig          u-boot/configs/
cp uboot/suniv-f1c200s-video-board.dts  u-boot/dts/upstream/src/arm/allwinner/
git -C u-boot apply ../f1c200_dvr_board/uboot/patches/*.patch
make -C u-boot f1c200s_dvr_defconfig
make -C u-boot CROSS_COMPILE=arm-none-eabi- -j8
# result: u-boot/u-boot-sunxi-with-spl.bin
```

**The patch is not optional.** Without it the board only boots while
something external holds UART0's receive pad high, which in practice means
a serial adapter plugged in — see the PE0 entry in
[HARDWARE-ERRATA.md](HARDWARE-ERRATA.md).

The defconfig carries the whole boot story: `CONFIG_BOOTCOMMAND="sf probe;
sf read 0x80000000 0x100000 0x40000; go 0x80000000"`, 1 s autoboot delay.

### Building on macOS

Three things in U-Boot's build assume a GNU userland and need help:

```sh
brew install make bash dtc openssl@3
gmake -C u-boot CROSS_COMPILE=arm-none-eabi- -j8 \
  HOSTCFLAGS="-I$(brew --prefix openssl@3)/include" \
  HOSTLDFLAGS="-L$(brew --prefix openssl@3)/lib"
```

`gmake` because the Makefile needs GNU make 4 and macOS ships 3.81. The
OpenSSL paths because the host tools include `openssl/evp.h` and macOS
ships no OpenSSL headers. `bash` because `scripts/check-local-export` uses
`shopt -s lastpipe`, which needs bash 4 against the system's 3.2 — that one
is a lint over exported symbols, so a build that cannot get bash 4 can
neutralise it without affecting the image.

One further trap if the build reaches `scripts/dtc/pylibfdt`: SWIG 4.5
generates Python 2 calls that no longer compile, so it needs SWIG 4.4 or
earlier, and the extension must link with `-undefined dynamic_lookup`,
which is what the `HOSTLDFLAGS` above is really for.

## Cutting a release

```sh
tools/release.sh v0.9.5 notes.md ../u-boot/u-boot-sunxi-with-spl.bin \
    [--requires-recovery]
```

It checks the tag against `FW_VERSION_STR` in `src/board.h`, builds the
firmware, applies the same two checks the board makes before it burns
anything (size within the 256 KB slot, byte 3 is the `0xEA` branch), checks
the U-Boot image really carries an `eGON.BT0` header, and attaches the
normal build, the debug build (below) and U-Boot.

**`--requires-recovery` matters.** It appends a trailer to the notes:

```
FPVault-Requires-Recovery: yes
```

FPVault Desktop parses that, strips it from the notes it shows, and warns
beside the Install button that the release has to go on over recovery.
Pass it whenever the release changes anything outside the firmware slot at
0x100000 — U-Boot at offset 0 above all — because the USB update writes
that slot and nothing else.

It cannot be inferred from the assets. Every release ships a U-Boot image
whether or not that image changed, and two builds of identical source differ
anyway because U-Boot stamps its build date in. So the release declares it.

## Debug build — a log on the card

For a board that misbehaves somewhere with no serial console to watch.

```sh
make debug        # build-debug/fpvault-debug.bin
make dfu-debug    # build it and install over USB
```

The same firmware, plus a text log written to the SD card while the board is
connected to a computer (card reader + camera mode). The banner and the log
say `0.9.x-debug`. The normal build contains none of this code.

**What is logged.** Only lines sent with `DLOG()` (`src/dlog.h`), not the
console output: boot and the cause of the previous reset, USB bus events,
every control request and what endpoint 0 did with it, the camera stream
starting, stopping and stalling, signal lost and found, capture restarts,
encoder aborts, and one line of counters every 5 s. Every line starts with
the uptime in seconds. A gap in the 5 s lines means the main loop stopped.

**How it avoids damaging the card.** In USB mode the computer owns the
filesystem, so the firmware never touches it then. Before USB starts it
creates `/FPVLOG.TXT`, 4 MB in one unbroken piece, and notes where that piece
is. Afterwards it only overwrites those sectors. The file is always 4 MB:
text first, spaces after.

- Each power-up continues where the last one stopped. When less than a
  quarter is left, the next power-up clears the file.
- If the computer deletes the file and reuses its space, logging stops for
  that session and the next power-up makes a new file.
- Lines not yet written when the board resets survive in RAM through a warm
  reset and are written first on the next boot.

**Collecting it.** The computer caches what it has read, so it shows the
file as it was when the card was mounted. Unplug the board, plug it back in,
then copy `FPVLOG.TXT` off the card.

`tools/release.sh` builds both and publishes the debug image as
`fpvault-<tag>-debug.bin`. FPVault Desktop installs only `fpvault-<tag>.bin`;
the debug image goes on with `dfu-util` or over FEL.

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
- **No FEL either, on a freshly assembled board** (no USB device at all,
  rails and crystal fine, every IC warm): suspect the RESET pin's solder
  joint at the QFN before the chip. Measured at the pull-up or the reset
  button the net reads 3.3 V while pin 70 itself floats, so the SoC never
  leaves reset. One touch of the iron on that pin fixed the first FPVault v1 board
  after two days of chasing power rails and swapping SoCs.
- **loader.py "no '=>' prompt"**: something else is running — press reset,
  or if a previous DVR/passthru build is live, its `r` command reboots to
  U-Boot.
- **YMODEM stalls with an ESP32 bridge inline**: loader.py already holds
  DTR/RTS low to avoid resetting the bridge; check the bridge's own power.
