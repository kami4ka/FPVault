/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dlog_core.c - see dlog_core.h.
 */
#include <stdio.h>
#include <string.h>
#include "dlog_core.h"

#define RING_MASK (DLOG_RING_SIZE - 1u)
#define WIPE_CHUNK 64u /* sectors per write while clearing */

static const char FULL_MSG[] =
    "*** log full: logging stopped. The next power-up clears the file. ***\r\n";

/* ---- ring ---------------------------------------------------------------- */
uint32_t dlog_ring_recover(dlog_ring_t* r) {
    uint32_t held = r->head - r->tail;
    if(r->magic == DLOG_RING_MAGIC && held <= DLOG_RING_SIZE) return held;
    r->magic = DLOG_RING_MAGIC;
    r->head = r->tail = r->lost = 0;
    return 0;
}

uint32_t dlog_ring_pending(const dlog_ring_t* r) {
    return r->head - r->tail;
}

int dlog_ring_put(dlog_ring_t* r, const uint8_t* s, uint32_t n) {
    uint32_t i, room = DLOG_RING_SIZE - (r->head - r->tail);
    if(n > room) {
        r->lost++;
        return 0;
    }
    for(i = 0; i < n; i++)
        r->data[(r->head + i) & RING_MASK] = s[i];
    r->head += n;
    return 1;
}

/* ---- formatting ---------------------------------------------------------- */
uint32_t dlog_format(char* out, uint32_t size, uint32_t ms, const char* fmt, va_list ap) {
    int n, m;
    if(size < 16u) return 0;
    n = snprintf(out, size, "%7lu.%03lu ", (unsigned long)(ms / 1000u),
                 (unsigned long)(ms % 1000u));
    /* Leave room for the line ending whatever the message does. */
    m = vsnprintf(out + n, size - (uint32_t)n - 2u, fmt, ap);
    if(m < 0) m = 0;
    if((uint32_t)m > size - (uint32_t)n - 3u) m = (int)(size - (uint32_t)n - 3u);
    n += m;
    while(n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        n--;
    out[n++] = '\r';
    out[n++] = '\n';
    return (uint32_t)n;
}

/* ---- card ---------------------------------------------------------------- */
static int is_padding(const uint8_t* s) {
    uint32_t i;
    for(i = 0; i < DLOG_SECTOR; i++)
        if(s[i] != DLOG_PAD) return 0;
    return 1;
}

static int wipe(dlog_t* d, uint32_t from, uint32_t count) {
    static uint8_t pad[WIPE_CHUNK * DLOG_SECTOR];
    memset(pad, DLOG_PAD, sizeof pad);
    while(count) {
        uint32_t n = count > WIPE_CHUNK ? WIPE_CHUNK : count;
        if(d->io.wr(d->io.ctx, d->lba0 + from, pad, n) != 0) return -1;
        from += n;
        count -= n;
    }
    return 0;
}

/* Content always runs from the first sector without a gap, so the first
 * sector that is nothing but padding is found by bisection. */
static int locate(dlog_t* d) {
    uint32_t lo = 0, hi = d->nsec;
    while(lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if(d->io.rd(d->io.ctx, d->lba0 + mid, d->cur, 1) != 0) return -1;
        if(is_padding(d->cur))
            hi = mid;
        else
            lo = mid + 1u;
    }
    d->sec = lo;
    d->fill = 0;
    if(lo > 0) {
        uint32_t i = DLOG_SECTOR;
        if(d->io.rd(d->io.ctx, d->lba0 + lo - 1u, d->cur, 1) != 0) return -1;
        while(i > 0 && d->cur[i - 1u] == DLOG_PAD)
            i--;
        if(i < DLOG_SECTOR) { /* the last sector used still has room */
            d->sec = lo - 1u;
            d->fill = i;
        }
    }
    return 0;
}

int dlog_core_open(dlog_t* d, dlog_ring_t* ring, const dlog_io_t* io, uint32_t lba0,
                   uint32_t nsec, int fresh) {
    memset(d, 0, sizeof *d);
    d->ring = ring;
    d->io = *io;
    d->lba0 = lba0;
    d->nsec = nsec;
    if(nsec < 8u) return -1;

    if(fresh) {
        if(wipe(d, 0, nsec) != 0) return -1;
    } else {
        if(locate(d) != 0) return -1;
        if(d->nsec - d->sec < d->nsec / 4u) {
            /* Only what was used needs clearing; the rest is padding. */
            uint32_t used = d->sec + (d->fill ? 1u : 0u);
            if(used > d->nsec) used = d->nsec;
            if(wipe(d, 0, used) != 0) return -1;
            d->sec = 0;
            d->fill = 0;
        }
    }
    if(d->fill == 0) memset(d->cur, DLOG_PAD, DLOG_SECTOR);
    d->ready = 1;
    return 0;
}

static int put_sector(dlog_t* d) {
    if(d->io.wr(d->io.ctx, d->lba0 + d->sec, d->cur, 1) != 0) {
        d->wr_err++;
        return -1;
    }
    /* Only now are these bytes safe to forget. */
    d->ring->tail += d->uncommitted;
    d->uncommitted = 0;
    d->dirty = 0;
    return 0;
}

uint32_t dlog_core_flush(dlog_t* d, int force, uint32_t max_sectors) {
    uint32_t written = 0;
    if(!d->ready || !d->active || d->full || d->abandoned) return 0;

    while(written < max_sectors) {
        uint32_t avail = (d->ring->head - d->ring->tail) - d->uncommitted;
        uint32_t room = DLOG_SECTOR - d->fill, n = avail < room ? avail : room, i;

        /* The last sector is kept for saying that the log is full. */
        if(d->sec >= d->nsec - 1u) {
            memset(d->cur, DLOG_PAD, DLOG_SECTOR);
            memcpy(d->cur, FULL_MSG, sizeof FULL_MSG - 1u);
            d->sec = d->nsec - 1u;
            d->uncommitted = 0;
            if(put_sector(d) == 0) written++;
            d->full = 1;
            break;
        }
        for(i = 0; i < n; i++)
            d->cur[d->fill + i] =
                d->ring->data[(d->ring->tail + d->uncommitted + i) & RING_MASK];
        if(n) {
            d->fill += n;
            d->uncommitted += n;
            d->dirty = 1;
        }
        if(d->fill == DLOG_SECTOR) {
            if(put_sector(d) != 0) break;
            written++;
            d->sec++;
            d->fill = 0;
            memset(d->cur, DLOG_PAD, DLOG_SECTOR);
            continue;
        }
        /* Ring drained into a part-filled sector. */
        if(force && d->dirty) {
            if(put_sector(d) == 0) written++;
        }
        break;
    }
    return written;
}

void dlog_core_host_wrote(dlog_t* d, uint32_t lba, uint32_t count) {
    if(!d->ready || d->abandoned) return;
    if(lba < d->lba0 + d->nsec && lba + count > d->lba0) {
        d->abandoned = 1;
        d->active = 0;
    }
}
