/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sdtest.h - the M2 bench harness: mount, boot-sector scrub, write/read
 * benchmark with the per-write latency histogram that sizes the DVR's
 * bitstream ring.
 */
#pragma once

void sdtest_mount(void);        /* 'M': init card + mount FAT, print info */
void sdtest_scrub(void);        /* 'Z': neutralize a stale eGON boot blob */
void sdtest_benchmark(void);    /* 'B': 64 MB write + verify, latency histogram */
void sdtest_toggle_width(void); /* 'W': 1-bit <-> 4-bit (takes effect on M) */
