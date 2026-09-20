/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pipeline.h - the DVR's frame pipeline, split across two contexts:
 *
 *   1 kHz timer IRQ (pipeline_tick): capture ring advance (two-sentinel,
 *   arm-early), non-blocking VE encode of each completed frame into the
 *   next bitstream slot, input-standard auto-follow. Nothing here blocks,
 *   so an SD stall in the main loop can never cost a capture.
 *
 *   main loop (pipeline_consume): drains READY slots to the recorder
 *   (FatFs writes, free to block). The slot ring absorbs card stalls -
 *   40 x 256 KB covers >1 s of frames.
 */
#pragma once

#include <stdint.h>

void pipeline_toggle(void);     /* ':c' start/stop capture+encode */
void pipeline_fmt_toggle(void); /* ':f' TVD 4:2:0 <-> 4:2:2 (+ISP pairing) */
void pipeline_tick(void);       /* 1 kHz IRQ */
void pipeline_consume(void);    /* main loop */
void pipeline_stats(void);      /* 1 Hz line */
int pipeline_active(void);
int pipeline_quality(void);

/* Turn a ring slot into a standalone JPEG, in place, and return its length.
 *
 * The VE emits entropy-coded scan data only, so a playable frame needs the
 * 608-byte SOI..SOS block in front of it and an EOI behind. Every sink wants
 * exactly that and nothing more: the recorder then wraps it in an AVI chunk,
 * and a USB video interface would hand the same bytes straight to the host,
 * since UVC's MJPEG payload is one whole JPEG per frame and needs the DHT
 * segments this block carries.
 *
 * The file starts at slot_base + BSRING_PREFIX_OFF. 416 + 608 = 1024 exactly,
 * so the header abuts the VE's own output and the result is contiguous - the
 * payload is never copied.
 */
uint32_t pipeline_finish_jpeg(uint32_t slot_base, uint32_t bitstream_len, int quality);

/* Newest encoded frame (for the console JPEG dump): returns 0 if none,
 * else fills the physical address of the bitstream and its length. */
int pipeline_last(uint32_t* phys, uint32_t* len);

/* Pause/resume NEW encodes (capture keeps running). A console dump takes
 * ~15 s at 115200 while the slot ring wraps every ~1.3 s - without the
 * freeze the dumped slot is rewritten mid-dump. */
void pipeline_freeze(int on);
