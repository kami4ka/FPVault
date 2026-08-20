/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * recorder.h - v0 recording glue: DCF-named MJPEG-in-AVI clips on SD.
 *
 * Synchronous first cut for M4: the live loop encodes into slot 0 and the
 * same loop writes the finished chunk with one f_write. A long card stall
 * therefore drops frames instead of buffering them - the bitstream ring +
 * IRQ pipeline that fixes this is the planned M3-tail/M4 work. The AVI is
 * crash-safe from day one: header-first, periodic refresh + f_sync.
 */
#pragma once

#include <stdint.h>

int recorder_active(void);

/* Manual toggle: stop pauses auto-record until toggled again. */
void recorder_toggle(void);

/* The state machine (NO_CARD -> WAIT_SIGNAL -> RECORDING with 5-minute
 * segments; dropouts stay in-clip as empty frames for <5 s, close it
 * beyond; card errors retry the mount). Call every main-loop pass. */
void recorder_task(void);
void recorder_toggle_auto(void);      /* default ON: record on stable signal */
uint8_t recorder_led_pattern(void);   /* 8 bits, 125 ms each, MSB first */

/* Called by the live loop after each successful encode: the complete AVI
 * chunk is assembled in the slot (prefix already staged, header + EOI
 * filled here) and written. quality names the jpegtab tables that match
 * the encoded frame. */
void recorder_on_frame(uint32_t slot_base, uint32_t bitstream_len, int quality);

/* 1 Hz stats line while recording. */
void recorder_stats(void);
