/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlog.h - the debug build's log, written to the SD card.
 *
 * There are two builds of this firmware from one tree. The normal one
 * writes nothing to the card but recordings. The debug one (make debug,
 * -DFPV_DEBUG_LOG) also keeps a text log there, for faults that only happen
 * on boards with no serial console attached to them.
 *
 * It is a separate channel on purpose. printf goes to the console as it
 * always has and none of it ends up in the file; only what is sent with
 * DLOG() does. In the normal build DLOG() compiles to nothing at all, its
 * arguments included, and none of the functions below exist.
 *
 * Why the file is strange. In USB mode the computer owns the filesystem, and
 * a second writer would corrupt it. So the firmware only touches filesystem
 * structures before USB comes up: it makes /FPVLOG.TXT at a fixed size in
 * one contiguous piece, notes which sectors the data occupies, and lets go.
 * From then on it overwrites those sectors and nothing else - the file's
 * size, its chain in the FAT and its directory entry never change, so there
 * is nothing for the two sides to disagree about. The price is a file that
 * is always 4 MB, text at the front and spaces after, and a host that shows
 * a stale copy until the board has been unplugged and plugged back in.
 */
#pragma once

#include <stdint.h>

#define DLOG_FILE_NAME "FPVLOG.TXT"
#define DLOG_FILE_SIZE (4u * 1024u * 1024u)

#ifdef FPV_DEBUG_LOG

void dlog_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define DLOG(...) dlog_printf(__VA_ARGS__)

void dlog_boot(void);     /* first thing: keep what the last life left unwritten */
void dlog_prepare(void);  /* before USB starts: find or make the file */
void dlog_activate(void); /* the host has the card: start writing */
void dlog_tick(void);        /* 1 kHz, from the timer interrupt */
void dlog_clock_start(void); /* once the free-running timer runs */
void dlog_poll(void);     /* main loop */
void dlog_host_wrote(uint32_t sector, uint32_t count);
/* Seconds since the last stats line was due; main() paces them with it. */
void dlog_stats(void);
void dlog_second(uint32_t uptime_s); /* main loop, once a second */
uint32_t dlog_now_ms(void);          /* the log's own clock */
uint32_t dlog_hold_max_ms(void);     /* longest the log kept USB waiting, since last asked */

#else

#define DLOG(...) ((void)0)

static inline void dlog_boot(void) {}
static inline void dlog_prepare(void) {}
static inline void dlog_activate(void) {}
static inline void dlog_tick(void) {}
static inline void dlog_clock_start(void) {}
static inline void dlog_poll(void) {}
static inline void dlog_host_wrote(uint32_t sector, uint32_t count) {
    (void)sector;
    (void)count;
}
static inline void dlog_stats(void) {}

#endif
