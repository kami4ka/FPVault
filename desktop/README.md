# FPVault Desktop

English | [Українська](README.uk.md)

The companion app for the [FPVault](../README.md) FPV DVR board: import clips,
repair the ones a power cut truncated, give them real timestamps, join a
session into one file, and update the board's firmware over USB.

Everything it needs is bundled. No Python, no ffmpeg, no dfu-util, no
sunxi-tools to install.

## Why it exists

Three things the board cannot do for itself:

- **Clips have no real date.** There is no RTC, and FatFs is built with
  `FF_FS_NORTC`, so every clip on every card is stamped 2020-01-01 00:00:00
  (which most systems then display as 2019-12-31). The app is the only place
  a real wall-clock time can come from.
- **A power cut leaves a 200 MB file.** The recorder preallocates
  209,715,200 bytes per clip and only trims it when the clip closes cleanly.
  Pull the battery and the clip keeps the full preallocation, loses its index,
  and carries old cluster data after the real video. Copying one naively moves
  200 MB to recover perhaps 40 MB of it.
- **Updating means a terminal.** `dfu-util` and `sunxi-fel` both have to be
  installed by hand, and on macOS sunxi-tools has no Homebrew formula at all.

## Development

```sh
cd desktop
npm install
npm run dev        # electron-vite with renderer HMR
npm run build      # typecheck + bundle main/preload/renderer
npm test           # AVI parser tests
```

## Device detection

The app never identifies a card by its name. The USB mass-storage strings are
CherryUSB's defaults (`MASS` / `Storage Device`), not "FPVault", so anything
keying on the label would fail. A volume is a FPVault card when it contains
`DCIM/<NNN>FCDVR/` — the DCF layout `src/dcf.c` creates.

USB presence is probed per platform with no native module:

| Platform | How |
|---|---|
| macOS | `ioreg -r -c IOUSBHostInterface` |
| Linux | `/sys/bus/usb/devices` |
| Windows | `Get-PnpDevice` via PowerShell |

Deliberately **not** node-usb: libusb on Windows only enumerates devices bound
to WinUSB, and a board in card-reader mode is bound to `usbstor`, so a native
module would see nothing in the most common state. It would also need a
rebuild per Electron version per platform, for a capability the app does not
need — `dfu-util` and `sunxi-fel` bring their own libusb.

Five states the UI distinguishes: no board; board in reader mode; board whose
firmware predates the DFU interface (v0.9.1 or earlier); board in FEL
recovery; and a card in a native reader with no board.

## The 2.5-second rule

The board is bus-powered, and `main()` waits only 2.5 s after power-on for a
host to configure it. A host can therefore only exist from power-on: the board
cannot be switched into card-reader mode later, and **changing its mode always
means unplugging and plugging back in**. The app says so rather than appearing
to hang, and the bulk capacitor at the SD socket means "unplug, count to five"
rather than a quick replug — see [docs/HARDWARE-ERRATA.md](../docs/HARDWARE-ERRATA.md).

## Firmware version is not readable

`bcdDevice` is hardcoded `0x0100` in `src/usbmsc.c` and the serial string is a
constant, the DFU interface refuses uploads, and the app has no serial console.
So the installed version genuinely cannot be read, and the UI says "unknown"
rather than guessing. What it *can* tell is whether the DFU interface exists at
all, which separates v0.9.2-and-later from everything before it.

The fix is one line of firmware: derive `bcdDevice` from the release version
(`0x0092` for v0.9.2). The app already decodes it when it is not the hardcoded
default.

## Testing against real clips

`local/` is git-ignored and is the repo's convention for bench captures. Drop
real clips there — especially crash-cut ones — and the corpus tests will pick
them up. A synthesised fixture can imitate a truncated file, but only a real
one has real prior-cluster garbage after the last frame, which is the
interesting adversarial input for the chunk walker.

## Licence

GPL-3.0-or-later, like the firmware. Bundled tools keep their own licences;
see [CREDITS.md](../CREDITS.md).
