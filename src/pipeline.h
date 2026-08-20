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

/* Newest encoded frame (for the console JPEG dump): returns 0 if none,
 * else fills the physical address of the bitstream and its length. */
int pipeline_last(uint32_t* phys, uint32_t* len);

/* Pause/resume NEW encodes (capture keeps running). A console dump takes
 * ~15 s at 115200 while the slot ring wraps every ~1.3 s - without the
 * freeze the dumped slot is rewritten mid-dump. */
void pipeline_freeze(int on);
