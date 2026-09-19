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
npm run pack       # a real .app/.exe in dist/, unpacked
```

`npm run pack` is worth running before believing anything about tools or
paths: `resolveBin` resolves somewhere different in a packaged app than in
dev, and a bundled executable that is fine under `npm run dev` can be
missing from the bundle entirely.

FPVault has no Developer ID, so the macOS build is signed ad-hoc by
`scripts/afterPack.js` — a valid local signature with no identity behind it.
That is not optional: an arm64 Mac refuses to run a bundle whose signature
does not cover it, and what electron-builder leaves behind when signing is
off is only the linker's signature on the main executable. The build config
sets `identity: null` so it cannot quietly pick up an unrelated certificate
from the keychain instead. Such a build runs on the machine that made it; a
public release needs a real Developer ID and notarization.

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

## Firmware version

There is no serial console, so the descriptors are the only channel. Firmware
from v0.9.3 derives `bcdDevice` from its own version (`0x0093` for v0.9.3,
`FW_VERSION_BCD` in `src/board.h`) and the app decodes it.

Anything older reports the hardcoded `0x0100`, which carries no version at
all, and the UI says "unknown" rather than guessing — along with why, because
an unexplained "unknown" reads as a fault. The one thing it can still tell
about an old board is whether the DFU interface exists, which separates
v0.9.2 from everything before it. Installing the same release twice is
harmless, so being unsure costs nothing.

## Testing against real clips

`local/` is git-ignored and is the repo's convention for bench captures. Drop
real clips there — especially crash-cut ones — and the corpus tests will pick
them up. A synthesised fixture can imitate a truncated file, but only a real
one has real prior-cluster garbage after the last frame, which is the
interesting adversarial input for the chunk walker.

## Bundled tools

`npm run fetch-binaries` downloads the executables named in
`resources/binaries.lock.json` into `resources/bin/<platform>-<arch>/` and
verifies each one against a pinned sha256. They are not committed: four
platforms of ffmpeg is ~250 MB, which has no business in a firmware repo.

**ffmpeg** is the only one downloaded; `sunxi-fel` is built from source by
`npm run build-fel` because no prebuilt exists for any platform this app
ships to. Import, repair, join and playback use neither — they are pure
TypeScript — so a missing tool costs one feature rather than the app.
Settings lists what this build actually has, with the path it was found at,
which is what explains a button that is not there.

## Join and export

Two ways out of the library:

- **Join** is lossless and needs no ffmpeg. Every segment shares a geometry
  and a timebase and every frame is an independent JPEG, so joining is one
  header, each segment's frames copied byte for byte, and one rebuilt index.
  Measured on a real 14-clip session: 2.22 GB and 123,379 frames in 20
  seconds, 112 MB/s, and the result passes `tools/checkavi.py` with no
  warnings. AVI indexes are 32-bit, so a joined file caps at 4 GB — about 95
  minutes; past that the app says so instead of writing something broken.
- **Export MP4** re-encodes to H.264 for sharing, roughly a tenth the size.
  It runs the concat demuxer over the *repaired* library copies, never the
  card's originals, because the demuxer needs a real index and an
  unfinalised clip would feed ffmpeg 200 MB of old cluster data.

## Firmware update

`Firmware` lists this repository's releases and installs one over USB. The
app speaks DFU 1.1 itself rather than bundling dfu-util: that gives real
byte-level progress, proper cancellation, and one less 80 MB executable per
platform. node-usb is an *optional* dependency used only for this, so a
machine where the native module will not load still imports, repairs and
plays clips.

Two things the implementation has to get right, both learned against real
hardware:

- **Zero-length control writes need an explicit empty buffer.** node-usb's
  WebUSB layer dereferences the data argument even when there is none, so
  `CLRSTATUS` and the final `DNLOAD` must pass `new Uint8Array(0)`.
- **The board keeps its DFU state across host sessions.** The firmware only
  resets its block counter when a `DNLOAD` arrives in `dfuIDLE`, so an
  interrupted update leaves it expecting block N and the next attempt's
  block 0 is rejected as a bad address. The app sends `CLRSTATUS` and
  `ABORT` and confirms `dfuIDLE` before the first byte; without that, one
  cancelled update would poison every later one until a replug.

Nothing reaches the flash until the whole image has arrived and the board
has checked it, so a cable pulled mid-transfer is harmless. Measured on a
real board: 80,096 bytes sent, burnt, verified and rebooted in 0.5 s.

## Recovery and guidance

`BoardGuide` animates the things that need hands on the board, drawn from
`docs/img/board-v1.jpg` in the same flat SVG style as `docs/img/pipeline.svg`.
What makes it better than a recording is that steps advance on the live
device state: hold SW2 and plug in, and the moment the app sees the boot ROM
the caption moves on to "release it" by itself. Under
`prefers-reduced-motion` it becomes a numbered list of the same captions.

Three sequences: board not detected, entering FEL, and the replug after an
update. Two of the "not detected" steps carry facts a user cannot guess —
the board decides its mode in the first 2.5 s of power, and the bulk
capacitor at the card socket means a quick replug may not power the card
down at all.

FEL recovery uses `sunxi-fel`, built from a pinned commit by
`npm run build-fel` rather than downloaded: no prebuilt exists for any
platform this app ships to. It is linked fully statically (libusb, libfdt
and zlib), which is both what makes it portable and what keeps macOS
notarization simple — 225 KB with no non-system dependencies.

Recovery writes U-Boot at NOR 0 and firmware at 1 MB, the layout
`uboot/f1c200s_dvr_defconfig` boots from. It is the path for a board that
cannot be reached any other way: blank flash, firmware older than the DFU
interface, or an image that bricked the normal boot.

## Icon

Derived from `docs/img/logo.png` — the mark the repository README already
shows — by `scripts/make-icons.py`, which writes `resources/icon.icns`,
`icon.ico` and `icon.png`. The outputs are committed so a build never needs
an image toolchain; the script exists so the derivation can be repeated
rather than being a binary someone once made in an editor.

Only the mark is used, never the wordmark: at 16 px a wordmark is a smudge,
and the mark alone is what a dock, an Alt-Tab switcher and an installer
actually show. It sits on a white superellipse tile, which is the shape
macOS uses, carrying a hairline in the app's own `--color-line` so the tile
still has an edge against a white background. The logo is drawn on white and
the tile is white, so the two meet without any colour keying and there is no
halo to go wrong.

## Settings

Short on purpose. Three things there change what the app does, and each one
exists because the right answer depends on the person rather than on the
hardware:

- **Library folder.** Choosing another starts a fresh index there; clips
  already imported stay where they are.
- **MP4 export.** Deinterlacing, on by default because the TVD feeds the
  board interlaced analogue video, and a quality choice that maps to an x264
  CRF. The middle setting is what every export used before it was a choice,
  so an existing library keeps producing the files it already has.
- **Gap between clips.** A clip that fills up rolls into the next with a gap
  of exactly zero, and that is detected rather than assumed. A clip that
  ended early ended on lost signal, and how long that lasted is recorded
  nowhere — so six seconds is the firmware's floor, not a measurement, and
  someone who knows their own flight can say better. No preference can move
  the rollover case; that one is a fact about the recorder.

The other two panels are not settings. Bundled tools says what this build
has, which is the only thing a hidden button cannot explain for itself, and
the licences panel carries the offer of corresponding source that shipping
GPL binaries requires.

## Licence

GPL-3.0-or-later, like the firmware. Bundled tools keep their own licences;
see [CREDITS.md](../CREDITS.md).
