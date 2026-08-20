/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * board.h - the single source of truth for pins, IRQs and the DRAM map.
 *
 * CAPTURE_BASE arrives from the Makefile with -D AND --defsym so the C code
 * and the link script cannot drift apart (the link script ASSERTs that code,
 * heap and stacks all finish below it - without that the TVD capture DMA
 * silently corrupts BSS, a failure the predecessor project actually hit).
 * Everything else in the map is derived from it here.
 */
#pragma once

#include <stdint.h>

/* ---- identity ----------------------------------------------------------- */
#define BOARD_NAME "f1c200s-dvr"

/* ---- DRAM map (64 MB, flat MMU, virt == phys) ---------------------------
 *
 * 0x8000_0000  firmware: text/data/bss/heap/stacks           (cacheable)
 * 0x8080_0000  test-pattern staging (NV16)                   (cacheable)
 * 0x8100_0000  CAPTURE_BASE: 3 x (Y,C) planes, 4 MB apart    (non-cacheable)
 *              Y0 +0x000000  C0 +0x400000
 *              Y1 +0x800000  C1 +0xC00000
 *              Y2 +0x1000000 C2 +0x1400000  (+4 MB guard = the ring's end)
 * 0x8280_0000  JPEG bitstream ring: 40 slots x 256 KB        (non-cacheable)
 * 0x8320_0000  idx1 staging                                  (cacheable)
 * 0x8330_0000  AVI header staging + FatFs work area          (cacheable)
 * 0x8340_0000  free
 *
 * The 4 MB plane spacing is NOT cosmetic: the TVD DMA overruns the Y plane
 * into whatever follows it (measured; adjacent planes = every other line grey).
 *
 * Capture planes and the bitstream ring are mapped NON-cacheable: the CPU
 * touches only a 720-byte sentinel row and ~11 header/pad bytes per frame, so
 * the uncached write penalty applies to nothing, and it removes every
 * clean/invalidate hazard between the three DMA masters that share the
 * regions (TVD writes, VE reads and writes, SD IDMAC reads).
 */
#ifndef CAPTURE_BASE
#error "CAPTURE_BASE must come from the Makefile (-D and --defsym)"
#endif

#define TESTPAT_BASE   0x80800000u

#define CAP_PLANE_SPACING 0x400000u              /* 4 MB */
#define CAP_NBUF          3
#define CAP_REGION_SIZE   (CAP_NBUF * 2 * CAP_PLANE_SPACING) /* 24 MB incl. guard */

#define BSRING_BASE       (CAPTURE_BASE + CAP_REGION_SIZE)   /* 0x82800000 */
#define BSRING_SLOTS      40u
#define BSRING_SLOT_SIZE  0x40000u               /* 256 KB */
#define BSRING_SIZE       (BSRING_SLOTS * BSRING_SLOT_SIZE)  /* 10 MB */
/* In-slot offsets: one contiguous AVI chunk = header + JPEG prefix + VE
 * bitstream + EOI. The VE output MUST be 1 KB aligned: measured on silicon
 * (wipe-encode-scan), VE 1663 writes at VLE_ADDR aligned DOWN - programming
 * slot+640 landed the stream at slot+0. */
#define BSRING_CHUNK_OFF  444u  /* 8-byte '00dc'+len */
#define BSRING_PREFIX_OFF 452u  /* 572-byte SOI+DQT+DHT */
#define BSRING_DATA_OFF   1024u /* VE VLE output base (1 KB aligned) */

#define IDX_BASE          (BSRING_BASE + BSRING_SIZE)        /* 0x83200000 */
#define IDX_SIZE          0x100000u              /* 1 MB: 16 B/frame ~ 36 min */

#define AVISTAGE_BASE     (IDX_BASE + IDX_SIZE)              /* 0x83300000 */
#define AVISTAGE_SIZE     0x100000u

/* One non-cacheable MMU window covers capture + bitstream contiguously. */
#define NCNB_BASE         CAPTURE_BASE
#define NCNB_SIZE         (CAP_REGION_SIZE + BSRING_SIZE)    /* 34 MB */

/* ---- pins ---------------------------------------------------------------
 * PE0/PE1  UART0 console (AF5)          PC0-PC3  SPI0 NOR (boot)
 * PE3      status LED                   PF0-PF5  SDC0, 4-bit (AF2)
 * PA2/PA3  UART1 - RunCam link to the flight controller
 * TV_IN    analog CVBS from the camera (high-impedance tap off the camera
 *          line - the DVR must NOT terminate it; exactly one 75R per link,
 *          and that one belongs to the VTX/display end)
 */
#define LED_PORT  GPIOE
#define LED_PIN   PIN3

/* ---- timebase ----------------------------------------------------------- */
#define TICKS_PER_SEC 24000000u   /* TIM0 free-running at HOSC 24 MHz */
