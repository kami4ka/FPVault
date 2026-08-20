/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_runcam.c - host-side unit test for the RunCam Device Protocol parser.
 *
 * The parser is a pure byte machine, so the whole protocol is testable here:
 * feed RX bytes, check returned events, capture replies through the injected
 * tx callback. Reference frames (toggle CC 01 01 E7, start CC 01 03 98,
 * stop CC 01 04 CC) are field-verified against Betaflight/ArduPilot.
 *
 * Ends with a deterministic fuzz run (xorshift32, fixed seed) mixing intact
 * frames, line noise and single-bit corruptions. Invariants: every intact
 * frame is delivered, corrupt/noise input never produces a spurious
 * TOGGLE/START/STOP, and CAMERA_CONTROL never triggers a tx call (a reply
 * to command 0x01 desyncs Betaflight's parser).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "runcam.h"

/* ------------------------------------------------------------------ */
/* tx capture: every reply the parser sends lands here                 */

struct txcap {
    uint8_t buf[64];
    int len;
    int calls;
};

static void tx_cb(void* ctx, const uint8_t* d, uint8_t len)
{
    struct txcap* t = ctx;
    assert(t->len + len <= (int)sizeof(t->buf));
    memcpy(t->buf + t->len, d, len);
    t->len += len;
    t->calls++;
}

static uint8_t crc_of(const uint8_t* d, int n)
{
    uint8_t c = 0;
    for (int i = 0; i < n; i++)
        c = rcam_crc8_dvb_s2(c, d[i]);
    return c;
}

/* Feed a byte string; return the last non-NONE event seen (or NONE). */
static rcam_event_e feed_all(rcam_t* r, const uint8_t* d, int n)
{
    rcam_event_e last = RCAM_EVT_NONE;
    for (int i = 0; i < n; i++) {
        rcam_event_e e = rcam_feed(r, d[i]);
        if (e != RCAM_EVT_NONE)
            last = e;
    }
    return last;
}

/* ------------------------------------------------------------------ */
/* 1. CRC vectors from field-verified reference frames                 */

static void test_crc_vectors(void)
{
    const uint8_t start[]  = { 0xCC, 0x01, 0x03 };
    const uint8_t stop[]   = { 0xCC, 0x01, 0x04 };
    const uint8_t toggle[] = { 0xCC, 0x01, 0x01 };
    assert(crc_of(start, 3) == 0x98);
    assert(crc_of(stop, 3) == 0xCC);
    assert(crc_of(toggle, 3) == 0xE7);
}

/* 2. Toggle frame byte-by-byte: event only on the final (CRC) byte,
 * no reply ever for CAMERA_CONTROL. */
static void test_toggle_byte_by_byte(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    assert(rcam_feed(&r, 0xCC) == RCAM_EVT_NONE);
    assert(rcam_feed(&r, 0x01) == RCAM_EVT_NONE);
    assert(rcam_feed(&r, 0x01) == RCAM_EVT_NONE);
    assert(rcam_feed(&r, 0xE7) == RCAM_EVT_TOGGLE);
    assert(cap.calls == 0);
    assert(r.rx_frames == 1);
    assert(r.crc_errors == 0);
}

/* 3. Explicit start/stop (ArduPilot path via feature bits 6/7). */
static void test_start_stop(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    const uint8_t start[] = { 0xCC, 0x01, 0x03, 0x98 };
    const uint8_t stop[]  = { 0xCC, 0x01, 0x04, 0xCC };
    assert(feed_all(&r, start, 4) == RCAM_EVT_START);
    assert(feed_all(&r, stop, 4) == RCAM_EVT_STOP);
    assert(cap.calls == 0);
    assert(r.rx_frames == 2);
}

/* 4. GET_DEVICE_INFO: exactly one 5-byte reply, correct feature word,
 * correct CRC, no event. */
static void test_device_info(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    uint8_t req[3] = { 0xCC, 0x00, 0x00 };
    req[2] = crc_of(req, 2);
    assert(feed_all(&r, req, 3) == RCAM_EVT_NONE);

    assert(cap.calls == 1);
    assert(cap.len == 5);
    assert(cap.buf[0] == 0xCC);
    assert(cap.buf[1] == RCAM_PROTO_VERSION);          /* 0x01 */
    assert(cap.buf[2] == (RCAM_FEATURES & 0xFF));      /* 0xC5 */
    assert(cap.buf[3] == (RCAM_FEATURES >> 8));        /* 0x00 */
    assert(cap.buf[4] == crc_of(cap.buf, 4));
    assert(r.rx_frames == 1);
}

/* 5. Wifi button (0x00) and change-mode (0x02) actions: parsed, counted,
 * ignored - no event, no reply. */
static void test_ignored_actions(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    uint8_t wifi[4] = { 0xCC, 0x01, 0x00, 0x00 };
    wifi[3] = crc_of(wifi, 3);
    uint8_t mode[4] = { 0xCC, 0x01, 0x02, 0x00 };
    mode[3] = crc_of(mode, 3);

    assert(feed_all(&r, wifi, 4) == RCAM_EVT_NONE);
    assert(feed_all(&r, mode, 4) == RCAM_EVT_NONE);
    assert(cap.calls == 0);
    assert(r.rx_frames == 2);
}

/* 5b. 5KEY frames: parsed and counted but ignored, and never replied to
 * (we don't advertise the 5KEY feature bits). */
static void test_5key_ignored(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    uint8_t press[4] = { 0xCC, 0x02, 0x01, 0x00 };
    press[3] = crc_of(press, 3);
    uint8_t release[3] = { 0xCC, 0x03, 0x00 };
    release[2] = crc_of(release, 2);
    uint8_t conn[4] = { 0xCC, 0x04, 0x01, 0x00 };
    conn[3] = crc_of(conn, 3);

    assert(feed_all(&r, press, 4) == RCAM_EVT_NONE);
    assert(feed_all(&r, release, 3) == RCAM_EVT_NONE);
    assert(feed_all(&r, conn, 4) == RCAM_EVT_NONE);
    assert(cap.calls == 0);
    assert(r.rx_frames == 3);
    assert(r.crc_errors == 0);
}

/* 6. Line noise before a frame: non-0xCC bytes while idle are discarded
 * silently (no crc_errors, no resyncs). */
static void test_noise_tolerance(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    const uint8_t noise[] = { 0x00, 0xFF, 0x37, 0x12, 0x7E };
    assert(feed_all(&r, noise, sizeof(noise)) == RCAM_EVT_NONE);
    assert(r.crc_errors == 0);

    const uint8_t toggle[] = { 0xCC, 0x01, 0x01, 0xE7 };
    assert(feed_all(&r, toggle, 4) == RCAM_EVT_TOGGLE);
    assert(cap.calls == 0);
    assert(r.rx_frames == 1);
    assert(r.crc_errors == 0);
}

/* 7. Corrupt CRC: counted, no event; parser recovers for the next frame. */
static void test_corrupt_crc(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    const uint8_t bad[] = { 0xCC, 0x01, 0x01, 0x00 }; /* CRC should be E7 */
    assert(feed_all(&r, bad, 4) == RCAM_EVT_NONE);
    assert(r.crc_errors == 1);
    assert(r.rx_frames == 0);

    const uint8_t good[] = { 0xCC, 0x01, 0x03, 0x98 };
    assert(feed_all(&r, good, 4) == RCAM_EVT_START);
    assert(r.rx_frames == 1);
    assert(cap.calls == 0);
}

/* 8. Resync with an embedded header: a truncated CAMERA_CONTROL frame
 * swallows the 0xCC of a following GET_DEVICE_INFO. The stream is
 *   CC 01 | CC 00 <crc(CC,00)>
 * which the parser first sees as the bad 4-byte frame CC 01 CC 00.
 * After the CRC failure it must drop only the leading byte, find the
 * embedded 0xCC, and still answer the device-info request. */
static void test_resync_embedded_header(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    uint8_t info[3] = { 0xCC, 0x00, 0x00 };
    info[2] = crc_of(info, 2);

    /* Precondition for the scenario: the mis-framed CC 01 CC 00 really
     * does fail its CRC (otherwise the test would prove nothing). */
    const uint8_t misframe[3] = { 0xCC, 0x01, 0xCC };
    assert(crc_of(misframe, 3) != 0x00);

    assert(rcam_feed(&r, 0xCC) == RCAM_EVT_NONE); /* truncated frame... */
    assert(rcam_feed(&r, 0x01) == RCAM_EVT_NONE);
    assert(feed_all(&r, info, 3) == RCAM_EVT_NONE);

    assert(cap.calls == 1); /* recovered and replied */
    assert(cap.len == 5);
    assert(r.rx_frames == 1);
    assert(r.crc_errors == 1);
    assert(r.resyncs > 0);
}

/* 9. rcam_idle() mid-frame abandons the partial; next frame parses. */
static void test_idle_midframe(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    assert(rcam_feed(&r, 0xCC) == RCAM_EVT_NONE);
    assert(rcam_feed(&r, 0x01) == RCAM_EVT_NONE);
    rcam_idle(&r);
    assert(r.resyncs == 1);

    rcam_idle(&r); /* idle while already idle: no-op, no counter bump */
    assert(r.resyncs == 1);

    const uint8_t toggle[] = { 0xCC, 0x01, 0x01, 0xE7 };
    assert(feed_all(&r, toggle, 4) == RCAM_EVT_TOGGLE);
    assert(r.rx_frames == 1);
    assert(r.crc_errors == 0);
}

/* ------------------------------------------------------------------ */
/* 10. Deterministic fuzz                                              */

static uint32_t prng_state = 0x1234ABCDu;

static uint32_t prng(void)
{
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return x;
}

/* Build one of the 5 valid frame types; returns length, sets *want to the
 * event an intact delivery must produce. */
static int build_frame(uint32_t rnd, uint8_t out[4], rcam_event_e* want)
{
    int len;
    *want = RCAM_EVT_NONE;
    switch (rnd % 5) {
    case 0: /* GET_DEVICE_INFO */
        out[0] = 0xCC;
        out[1] = 0x00;
        len = 3;
        break;
    case 1: { /* CAMERA_CONTROL, action 0..4 */
        uint8_t action = (uint8_t)((rnd >> 8) % 5);
        out[0] = 0xCC;
        out[1] = 0x01;
        out[2] = action;
        len = 4;
        if (action == 0x01)
            *want = RCAM_EVT_TOGGLE;
        else if (action == 0x03)
            *want = RCAM_EVT_START;
        else if (action == 0x04)
            *want = RCAM_EVT_STOP;
        break;
    }
    case 2: /* 5KEY press */
        out[0] = 0xCC;
        out[1] = 0x02;
        out[2] = (uint8_t)(1 + (rnd >> 8) % 5);
        len = 4;
        break;
    case 3: /* 5KEY release */
        out[0] = 0xCC;
        out[1] = 0x03;
        len = 3;
        break;
    default: /* 5KEY connection open/close */
        out[0] = 0xCC;
        out[1] = 0x04;
        out[2] = (uint8_t)((rnd >> 8) & 0x03);
        len = 4;
        break;
    }
    out[len - 1] = crc_of(out, len - 1);
    return len;
}

static void test_fuzz(void)
{
    struct txcap cap = { 0 };
    rcam_t r;
    rcam_init(&r, tx_cb, &cap);

    uint32_t expect_events = 0, got_events = 0;
    uint32_t expect_frames = 0, expect_info = 0;
    uint32_t spurious = 0;
    uint32_t n_valid = 0, n_junk = 0, n_corrupt = 0;

    for (int iter = 0; iter < 10000; iter++) {
        uint32_t rnd = prng();
        uint8_t f[4];
        rcam_event_e want;

        switch (rnd % 3) {
        case 0: { /* intact valid frame after a UART idle gap */
            rcam_idle(&r); /* known-idle so the frame must be delivered */
            int len = build_frame(prng(), f, &want);
            cap.calls = 0;
            cap.len = 0;
            rcam_event_e got = RCAM_EVT_NONE;
            for (int i = 0; i < len; i++) {
                rcam_event_e e = rcam_feed(&r, f[i]);
                if (e != RCAM_EVT_NONE) {
                    assert(got == RCAM_EVT_NONE); /* at most one event */
                    got = e;
                }
            }
            assert(got == want);
            if (want != RCAM_EVT_NONE) {
                expect_events++;
                got_events++;
            }
            expect_frames++;
            if (f[1] == 0x00) { /* GET_DEVICE_INFO must be answered */
                assert(cap.calls == 1 && cap.len == 5);
                expect_info++;
            } else {
                assert(cap.calls == 0); /* nothing else ever replies */
            }
            n_valid++;
            break;
        }
        case 1: { /* random junk, 1..8 bytes */
            int n = 1 + (int)((rnd >> 8) & 0x07);
            for (int i = 0; i < n; i++) {
                rcam_event_e e = rcam_feed(&r, (uint8_t)prng());
                if (e == RCAM_EVT_TOGGLE || e == RCAM_EVT_START ||
                    e == RCAM_EVT_STOP)
                    spurious++;
            }
            rcam_idle(&r);
            n_junk++;
            break;
        }
        default: { /* single-bit corruption of a valid frame */
            rcam_idle(&r);
            int len = build_frame(prng(), f, &want);
            uint32_t bit = prng() % (uint32_t)(len * 8);
            f[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            for (int i = 0; i < len; i++) {
                rcam_event_e e = rcam_feed(&r, f[i]);
                /* CRC8 catches every single-bit error, so no control
                 * event may ever surface from a corrupted frame */
                if (e == RCAM_EVT_TOGGLE || e == RCAM_EVT_START ||
                    e == RCAM_EVT_STOP)
                    spurious++;
            }
            rcam_idle(&r);
            n_corrupt++;
            break;
        }
        }
    }

    assert(spurious == 0);
    assert(got_events == expect_events);
    /* junk can accidentally contain valid-CRC frames, so >= not == */
    assert(r.rx_frames >= expect_frames);

    printf("fuzz: %u valid, %u junk, %u corrupt | frames=%u (expected>=%u) "
           "events=%u/%u info_replies=%u crc_errors=%u resyncs=%u "
           "spurious=%u\n",
           n_valid, n_junk, n_corrupt, r.rx_frames, expect_frames,
           got_events, expect_events, expect_info, r.crc_errors, r.resyncs,
           spurious);
}

int main(void)
{
    test_crc_vectors();
    test_toggle_byte_by_byte();
    test_start_stop();
    test_device_info();
    test_ignored_actions();
    test_5key_ignored();
    test_noise_tolerance();
    test_corrupt_crc();
    test_resync_embedded_header();
    test_idle_midframe();
    test_fuzz();
    printf("test_runcam: all tests pass\n");
    return 0;
}
