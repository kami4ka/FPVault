/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlog_core.h - the debug log, minus the hardware.
 *
 * Everything that decides where a byte of log goes lives here and knows
 * nothing about SD cards, FatFs or interrupts: it is handed a block of RAM
 * for the ring and two functions for reading and writing sectors. That is
 * what lets tests/host/test_dlog.c run the whole of it on a desk, including
 * the parts that only matter after a reset or a full file.
 *
 * The log is a fixed-size file whose data sectors are overwritten in place.
 * See src/dlog.h for why it has to be that and not an ordinary file.
 */
#pragma once

#include <stdarg.h>
#include <stdint.h>

#define DLOG_SECTOR    512u
#define DLOG_PAD       ' '
#define DLOG_RING_SIZE 32768u /* power of two */
#define DLOG_RING_MAGIC 0x474F4C44u /* "DLOG" */
#define DLOG_LINE_MAX  200u

/* Lines waiting to reach the card. The indices run free and wrap with the
 * integer; (head - tail) is the number of bytes held. It sits at a fixed
 * address in DRAM so that what had not been written when the board reset is
 * still there for the next boot to write. */
typedef struct dlog_ring {
    uint32_t magic;
    uint32_t head; /* next byte in   */
    uint32_t tail; /* next byte out  */
    uint32_t lost; /* lines dropped for want of room, not yet reported */
    uint8_t data[DLOG_RING_SIZE];
} dlog_ring_t;

typedef struct dlog_io {
    int (*rd)(void* ctx, uint32_t lba, uint8_t* buf, uint32_t nsec);
    int (*wr)(void* ctx, uint32_t lba, const uint8_t* buf, uint32_t nsec);
    void* ctx;
} dlog_io_t;

typedef struct dlog {
    dlog_ring_t* ring;
    dlog_io_t io;
    uint32_t lba0, nsec; /* where the file's data lives */
    uint32_t sec;        /* sector being filled, 0-based in the file */
    uint32_t fill;       /* bytes of it in use */
    uint32_t uncommitted; /* taken from the ring into cur[], not yet on the card */
    uint32_t wr_err;
    uint8_t cur[DLOG_SECTOR];
    uint8_t ready;     /* extent known and position found */
    uint8_t active;    /* allowed to write to the card */
    uint8_t full;
    uint8_t abandoned; /* the host wrote into our extent: it is not ours now */
    uint8_t dirty;     /* cur[] differs from the card */
} dlog_t;

/* Returns the bytes still waiting from before a reset, or 0 after resetting
 * a ring that was not valid. */
uint32_t dlog_ring_recover(dlog_ring_t* r);
/* Whole line or nothing. Returns 1 if stored. Caller excludes interrupts. */
int dlog_ring_put(dlog_ring_t* r, const uint8_t* s, uint32_t n);
uint32_t dlog_ring_pending(const dlog_ring_t* r);

/* "  12345.678 text\r\n" into out; returns the length. */
uint32_t dlog_format(char* out, uint32_t size, uint32_t ms, const char* fmt, va_list ap);

/* Take over an extent. fresh: the file was just created, contents unknown.
 * Otherwise finds where the last session stopped, and starts over from the
 * top if less than a quarter of the file is left. 0 on success. */
int dlog_core_open(dlog_t* d, dlog_ring_t* ring, const dlog_io_t* io, uint32_t lba0,
                   uint32_t nsec, int fresh);

/* Move what the ring holds to the card. Full sectors always; with force, the
 * partly filled one as well. Returns sectors written. */
uint32_t dlog_core_flush(dlog_t* d, int force, uint32_t max_sectors);

/* The host wrote count sectors at lba. If that touches the extent, the file
 * has been deleted and its space reused, and logging stops for good. */
void dlog_core_host_wrote(dlog_t* d, uint32_t lba, uint32_t count);
