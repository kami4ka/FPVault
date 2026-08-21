# Architecture

## Pipeline

```
CVBS camera ──> TVD (own DMA) ──> DDR semi-planar YUV ring (3 bufs, 4 MB apart)
                                        │  (VE ISP reads NV12/NV16 directly)
                                        ▼
                              Cedar VE JPEG encode (~ms @ 300 MHz)
                                        │  bitstream → DDR slot ring
                                        ▼
                    main loop: f_write slot (SD 4-bit @ 50 MHz + IDMAC)
                                        ▼
                     AVI ('MJPG') on FAT32, /DCIM/NNNFCDVR/FCDV####.AVI
```

The CPU never touches a pixel. Per frame it stamps/polls one 720-byte
sentinel row, writes ~40 VE registers, an 8-byte AVI chunk header, and the
bookkeeping. Record-only tap: no display path exists in this firmware (the
predecessor passthru firmware remains the eyes-on-video bench tool).

## DRAM map (64 MB, flat MMU, virt == phys)

```
0x8000_0000  firmware text/data/bss/heap/stacks              8 MB  cacheable
0x8080_0000  test-pattern staging (NV12/NV16)                8 MB  cacheable
0x8100_0000  CAPTURE_BASE: 3 x (Y,C) planes, 4 MB apart     24 MB  NON-cacheable
0x8280_0000  JPEG bitstream ring: 40 slots x 256 KB         10 MB  NON-cacheable
0x8320_0000  idx1 staging (16 B/frame ~ 36 min)              1 MB  cacheable
0x8330_0000  AVI header staging + FatFs work area            1 MB  cacheable
0x8340_0000  free                                           12 MB
```

- The 4 MB plane spacing is load-bearing: the TVD DMA overruns the Y plane
  (adjacent planes = every other captured line grey; measured).
- Capture + bitstream are non-cacheable on purpose. The predecessor found
  cached DMA buffers 1.7× faster **for CPU pixel work** — but this firmware
  does no CPU pixel work, and non-cacheable mappings delete the entire
  clean/invalidate hazard class across three DMA masters (TVD write, VE
  read+write, SD IDMAC read). The only CPU touches are the sentinel row
  (uncached reads of 90 bytes/frame) and chunk headers.
- `CAPTURE_BASE` reaches C and the linker from one Makefile variable; the
  link script ASSERTs everything below it.

## Bitstream slot layout (zero-copy mux)

Each '00dc' payload must be a complete JPEG: prefix (SOI+DQT+DHT, 572
bytes, constant per quality) + VE bitstream (SOF0+SOS+scan) + EOI. The
slot reserves headroom so the whole chunk is contiguous and the VE output
lands 64-byte aligned:

```
slot+0   .. slot+59    reserved
slot+60  .. slot+67    '00dc' + length   (CPU, after encode completes)
slot+68  .. slot+639   jpegtab prefix    (CPU, copied once per quality change)
slot+640 ..            VE VLE output, VLE_END = slot end
         then          EOI (2 bytes) + 0..3 zero pad appended by the CPU
```

One `f_write(slot+60, 8 + 572 + len + 2 + pad)` per frame (avi_add_raw
accounting; pad bytes zeroed by the recorder per its contract). Offsets in
src/board.h: BSRING_CHUNK_OFF / BSRING_PREFIX_OFF / BSRING_DATA_OFF.

## Concurrency (planned M3+; M1 is all main-loop)

- Frame-done: TVD frame-done IRQ (experiment) or TIM1 1 kHz sentinel poll →
  capture ring advance + VE trigger, in IRQ context so SD stalls in the
  main loop never cost a capture.
- VE done (IRQ 34): read status/length, mark slot READY.
- UART1 RX (IRQ 2): RunCam byte ring.
- Main loop: watchdog, console, recorder state machine, SD writes, AVI
  bookkeeping, 1 Hz stats. Signal-loss timeouts are paced by TIM0-derived
  time, never by frame-done events (no signal = no events).

## Clip naming

DCF (JEITA CP-3461): `/DCIM/NNNFCDVR/FCDV####.AVI`, 8.3 uppercase, global
monotonic file index (never reused), new NNN directory per power-on
session, boot-time scan is the authority. See src/dcf.h.

## USB mass storage (card reader mode)

Plugging the board into a computer must "just show the videos" - and the
board is USB-powered, so a USB host can only ever be present from power-on.
That collapses the design into a boot-time fork (src/main.c):

- `usbmsc_init()` brings the SD card up **raw, no filesystem** first
  (`disk_raw_init`), because CherryUSB caches the reported capacity exactly
  once, inside `usbd_msc_init_intf`. Registering the interface with the
  card down tells the host "0 blocks" forever and every READ(10) then dies
  on the stack's own LBA range check - found the hard way.
- After init, main waits up to 2.5 s for a SET_CONFIGURATION from a host.
  A charger or a flight controller's 5 V rail never configures, so in the
  air the window expires and recording starts ~2 s late - the entire cost
  of the feature. A computer configures within the window, the recorder
  enters `REC_USB_MODE` before ever mounting the card, and the board is a
  pure card reader until reboot.
- The MSC class runs entirely in the USB IRQ (no thread, no RTOS): sector
  reads/writes go straight to `sdcard_read/write` on the raw card. FatFs
  never runs in reader mode, so there is no dual-writer hazard.

Stack: CherryUSB v1.2.0 device core + MSC class on the F1C's MUSB
controller (`CONFIG_USB_MUSB_SUNXI` shifted register map, base 0x01C13000,
IRQ 26). PHY/clock recipe in src/usbphy.c. Full-Speed for now (~800 KB/s
to a Mac); High-Speed is a config flip (`CONFIG_USB_HS` + 512-byte MPS)
kept as debt until FS has field mileage.
