/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The debug log writes to a card a computer has mounted, so every mistake it
 * could make lands in somebody's filesystem. This runs the whole of its
 * logic against a card made of RAM: that it only ever writes inside its
 * extent, that it picks up where the last session stopped, that nothing is
 * forgotten before it is on the card, and that it lets go the moment the
 * host takes the space back.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dlog_core.h"

#define CARD_SECTORS 4096u
#define LBA0 1000u
#define NSEC 64u

static uint8_t card[CARD_SECTORS * DLOG_SECTOR];
static uint32_t writes, fail_writes;
static int fails = 0;

#define CHECK(what, cond)                                   \
    do {                                                    \
        if(cond) printf("  ok   %s\n", what);               \
        else { printf("  FAIL %s\n", what); fails++; }      \
    } while(0)

static int rd(void* c, uint32_t lba, uint8_t* b, uint32_t n) {
    (void)c;
    if(lba + n > CARD_SECTORS) return -1;
    memcpy(b, card + lba * DLOG_SECTOR, n * DLOG_SECTOR);
    return 0;
}
static int wr(void* c, uint32_t lba, const uint8_t* b, uint32_t n) {
    (void)c;
    if(fail_writes) return -1;
    if(lba + n > CARD_SECTORS) return -1;
    memcpy(card + lba * DLOG_SECTOR, b, n * DLOG_SECTOR);
    writes += n;
    return 0;
}
static const dlog_io_t io = {rd, wr, 0};

static dlog_ring_t ring;
static dlog_t d;

static void put(const char* fmt, ...) {
    char line[DLOG_LINE_MAX];
    va_list ap;
    uint32_t n;
    va_start(ap, fmt);
    n = dlog_format(line, sizeof line, 1234, fmt, ap);
    va_end(ap);
    dlog_ring_put(&ring, (const uint8_t*)line, n);
}

/* 0xEE everywhere, so a write outside the extent is visible as a change. */
static void blank_card(void) {
    memset(card, 0xEE, sizeof card);
    writes = 0;
    fail_writes = 0;
}
static int outside_untouched(void) {
    uint32_t i;
    for(i = 0; i < sizeof card; i++) {
        uint32_t s = i / DLOG_SECTOR;
        if((s < LBA0 || s >= LBA0 + NSEC) && card[i] != 0xEE) return 0;
    }
    return 1;
}
static const uint8_t* sec(uint32_t n) {
    return card + (LBA0 + n) * DLOG_SECTOR;
}
static void fresh(void) {
    blank_card();
    memset(&ring, 0, sizeof ring);
    dlog_ring_recover(&ring);
    dlog_core_open(&d, &ring, &io, LBA0, NSEC, 1);
    d.active = 1;
}

int main(void) {
    uint32_t i, n;
    char line[DLOG_LINE_MAX];

    printf("== test_dlog\n");

    /* ---- formatting ---- */
    {
        va_list ap;
        memset(&ap, 0, sizeof ap);
        n = dlog_format(line, sizeof line, 61234, "plain", ap);
        line[n] = 0;
        CHECK("line carries seconds.milliseconds and CRLF",
              strcmp(line, "     61.234 plain\r\n") == 0);
    }
    put("trailing newline is not doubled\r\n");

    /* ---- a fresh file ---- */
    fresh();
    CHECK("creating clears the whole extent to padding",
          sec(0)[0] == DLOG_PAD && sec(NSEC - 1)[DLOG_SECTOR - 1] == DLOG_PAD);
    CHECK("and nothing outside it", outside_untouched());
    CHECK("starts at the top", d.sec == 0 && d.fill == 0);

    /* ---- nothing is written until there is something to write ---- */
    writes = 0;
    CHECK("an empty ring writes nothing, even forced", dlog_core_flush(&d, 1, 8) == 0 && writes == 0);

    /* ---- a short line: only a forced flush puts a part sector down ---- */
    put("hello");
    CHECK("a part sector waits for a forced flush", dlog_core_flush(&d, 0, 8) == 0);
    CHECK("a forced flush writes it", dlog_core_flush(&d, 1, 8) == 1);
    CHECK("the text is on the card", memcmp(sec(0), "      1.234 hello\r\n", 19) == 0);
    CHECK("followed by padding", sec(0)[19] == DLOG_PAD && sec(0)[511] == DLOG_PAD);
    CHECK("the ring has let go of it", dlog_ring_pending(&ring) == 0);

    /* ---- the same sector is rewritten as it fills, not skipped ---- */
    put("world");
    dlog_core_flush(&d, 1, 8);
    CHECK("the next line lands in the same sector", d.sec == 0 && d.fill == 38);
    CHECK("after the first", memcmp(sec(0) + 19, "      1.234 world\r\n", 19) == 0);

    /* ---- enough to cross sector boundaries ---- */
    for(i = 0; i < 100; i++)
        put("line %03lu padding padding padding", (unsigned long)i);
    n = dlog_core_flush(&d, 0, 100);
    CHECK("full sectors go out without being forced", n >= 8 && d.sec == n);
    CHECK("and the ring keeps only the unfinished part",
          dlog_ring_pending(&ring) == d.uncommitted);
    dlog_core_flush(&d, 1, 8);
    CHECK("still nothing outside the extent", outside_untouched());

    /* ---- a write that fails forgets nothing ---- */
    put("must survive a failed write");
    n = dlog_ring_pending(&ring);
    fail_writes = 1;
    dlog_core_flush(&d, 1, 8);
    CHECK("the ring still holds the line", dlog_ring_pending(&ring) == n && d.wr_err == 1);
    fail_writes = 0;
    dlog_core_flush(&d, 1, 8);
    CHECK("and it is written once the card answers", dlog_ring_pending(&ring) == 0);

    /* ---- the next boot continues where this one stopped ---- */
    {
        uint32_t at_sec = d.sec, at_fill = d.fill;
        dlog_t e;
        dlog_core_open(&e, &ring, &io, LBA0, NSEC, 0);
        CHECK("reopening finds the same position", e.sec == at_sec && e.fill == at_fill);
        CHECK("and does not clear what is there",
              memcmp(sec(0), "      1.234 hello\r\n", 19) == 0);
        e.active = 1;
        put("second session");
        dlog_core_flush(&e, 1, 8);
        CHECK("appending rather than overwriting",
              memcmp(sec(at_sec) + at_fill, "      1.234 second session\r\n", 28) == 0);
    }

    /* ---- a sector that ended exactly full ---- */
    fresh();
    for(i = 0; i < DLOG_SECTOR; i++)
        ring.data[i] = 'x';
    ring.head = DLOG_SECTOR;
    dlog_core_flush(&d, 0, 8);
    {
        dlog_t e;
        dlog_core_open(&e, &ring, &io, LBA0, NSEC, 0);
        CHECK("resumes at the start of the next one", e.sec == 1 && e.fill == 0);
    }

    /* ---- a nearly full file is cleared at power-up ---- */
    fresh();
    for(i = 0; i < 50; i++) { /* 50 of 64 sectors: under a quarter left */
        uint32_t k;
        for(k = 0; k < DLOG_SECTOR; k++)
            ring.data[(ring.head + k) & (DLOG_RING_SIZE - 1u)] = 'y';
        ring.head += DLOG_SECTOR;
        dlog_core_flush(&d, 0, 8);
    }
    {
        dlog_t e;
        dlog_core_open(&e, &ring, &io, LBA0, NSEC, 0);
        CHECK("under a quarter left: starts over from the top", e.sec == 0 && e.fill == 0);
        CHECK("with the old text gone", sec(0)[0] == DLOG_PAD && sec(49)[0] == DLOG_PAD);
        CHECK("and nothing outside the extent touched", outside_untouched());
    }

    /* ---- running into the end ---- */
    fresh();
    for(i = 0; i < NSEC + 4; i++) {
        uint32_t k;
        for(k = 0; k < DLOG_SECTOR; k++)
            ring.data[(ring.head + k) & (DLOG_RING_SIZE - 1u)] = 'z';
        ring.head += DLOG_SECTOR;
        dlog_core_flush(&d, 0, 8);
    }
    CHECK("stops at the end of the file", d.full == 1);
    CHECK("says so in the last sector", memcmp(sec(NSEC - 1), "*** log full", 12) == 0);
    CHECK("and never writes past it", outside_untouched());
    writes = 0;
    put("after full");
    CHECK("then writes nothing more", dlog_core_flush(&d, 1, 8) == 0 && writes == 0);

    /* ---- the host takes the space back ---- */
    fresh();
    dlog_core_host_wrote(&d, LBA0 - 10, 10);
    CHECK("a host write just below the extent changes nothing", d.active == 1);
    dlog_core_host_wrote(&d, LBA0 + NSEC, 8);
    CHECK("nor one just above it", d.active == 1);
    dlog_core_host_wrote(&d, LBA0 - 4, 8);
    CHECK("one that reaches into it ends logging", d.abandoned == 1 && d.active == 0);
    writes = 0;
    put("must not be written");
    CHECK("and nothing is written afterwards", dlog_core_flush(&d, 1, 8) == 0 && writes == 0);

    /* ---- the ring ---- */
    memset(&ring, 0, sizeof ring);
    dlog_ring_recover(&ring);
    memset(line, 'q', sizeof line);
    for(i = 0; i < DLOG_RING_SIZE / 100u + 5u; i++)
        dlog_ring_put(&ring, (const uint8_t*)line, 100);
    CHECK("a full ring drops whole lines and counts them",
          ring.lost == 5 && dlog_ring_pending(&ring) == (DLOG_RING_SIZE / 100u) * 100u);
    CHECK("what survives a reset is kept", dlog_ring_recover(&ring) == (DLOG_RING_SIZE / 100u) * 100u);
    ring.magic = 0x12345678;
    CHECK("memory that is not a ring is reset", dlog_ring_recover(&ring) == 0 && ring.head == 0);
    ring.head = 5;
    ring.tail = 90000;
    CHECK("so are indices that cannot be right",
          dlog_ring_recover(&ring) == 0 && ring.tail == 0);

    if(fails) {
        printf("test_dlog: %d FAILURES\n", fails);
        return 1;
    }
    printf("test_dlog: all tests pass\n");
    return 0;
}
