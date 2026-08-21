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
