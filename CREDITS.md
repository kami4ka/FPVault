# Credits

This firmware stands on prior work:

- **[nminaylov/F1C100s_projects](https://github.com/nminaylov/F1C100s_projects)**
  (GPL-3.0) — the bare-metal HAL vendored under `vendor/F1C100s_projects/`
  (clock/GPIO/INTC/timer/UART/TVD/SDC drivers, ARM926 startup and cache code,
  build scaffolding, mksunxi).
- **[milosladni/jepoc](https://github.com/milosladni/jepoc)** (LGPL-2.1+, by
  Manuel Braga) — the register-level Cedar VE JPEG-encode proof of concept
  that `src/ve.c` / `src/vejpeg.c` are ported from.
- **[ChaN's FatFs](http://elm-chan.org/fsw/ff/00index_e.html)** (BSD-style)
  — vendored under `vendor/fatfs/`.
- **f1c200-video-board** (GPL-3.0, same author) — the predecessor experiments
  project whose measured findings this design is built on: the TVD capture
  ring discipline (3 buffers, 4 MB plane spacing, arm-on-completion via
  sentinel rows), clock/MMU bring-up, and the U-Boot YMODEM loader.
- **[mirkerson/c600](https://github.com/mirkerson/c600)** Linux 3.10 BSP —
  reference for the suniv (F1C-family) Video Engine clock/reset bring-up
  sequence.
- **[uli/allwinner-bare-metal](https://github.com/uli/allwinner-bare-metal)**
  `h264avi.c` (MIT) and
  **[s60sc/ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD)**
  `avi.cpp` — reference implementations studied for the AVI muxer.
- **[linux-sunxi.org](https://linux-sunxi.org)** — Video Engine register
  documentation and the CedarX reverse-engineering effort.
- **[CherryUSB](https://github.com/cherry-embedded/CherryUSB)** v1.2.0
  (Apache-2.0) — USB device stack (core + MSC class + MUSB port), vendored
  under `vendor/cherryusb/` with bare-metal config tweaks and a
  READ CAPACITY(16) addition noted in the file headers.
- **[lhdjply/f1c200s_library](https://github.com/lhdjply/f1c200s_library)**
  (MIT) — USB PHY/clock bring-up recipe and MSC descriptor reference
  (`src/usbphy.c`, parts of `src/usbmsc.c`), cross-checked against mainline
  Linux `musb_sunxi` and `phy-sun4i-usb`.

Combined work licensed **GPL-3.0-or-later**; vendored trees keep their
original license files. Derived source files carry their origin in the
header.
