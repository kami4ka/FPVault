/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * capture.c - see capture.h. The ring discipline and every magic ordering
 * rule here were extracted verbatim from f1c200-video-board's passthru
 * firmware, where each was measured (latch_test) rather than inferred.
 * Display-side code is gone: this firmware records and nothing else.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "capture.h"
#include "io.h"
#include "f1c100s_periph.h"
#include "f1c100s_tvd.h"

#define NBUF CAP_NBUF

/* Fixed plane addresses, 4 MB apart - see board.h for why the spacing is
 * load-bearing. The planes are in the non-cacheable window, so the sentinel
 * stamp/check needs no cache maintenance (the passthru original, running
 * with cacheable buffers, had to clean/invalidate around every row). */
static uint8_t* const CAPY[NBUF] = {
    (uint8_t*)(CAPTURE_BASE + 0x000000u),
    (uint8_t*)(CAPTURE_BASE + 0x800000u),
    (uint8_t*)(CAPTURE_BASE + 0x1000000u),
};
static uint8_t* const CAPC[NBUF] = {
    (uint8_t*)(CAPTURE_BASE + 0x400000u),
    (uint8_t*)(CAPTURE_BASE + 0xC00000u),
    (uint8_t*)(CAPTURE_BASE + 0x1400000u),
};

static vid_std_e vid_std = VID_NTSC;
static cap_fmt_e fmt = CAP_FMT_422;
static uint16_t FH = 480u;

static int wr = 0;    /* buffer the TVD DMA is filling */
static int done = -1; /* just completed - the TVD may still touch it if the
                       * addr latch race was lost; NOT safe to read */
static int safe = -1; /* completed one full cycle ago - the TVD has provably
                       * moved on; the only buffer consumers may read */

static uint32_t frames = 0, std_switches = 0;

/* ---- sentinel ------------------------------------------------------------
 * Stamp 0x5A across two Y rows of the buffer being filled:
 *   ARM  ~80% down, forced odd (lands late in the second field either way);
 *   DONE the last row - the DMA fills top to bottom, so its erasure means
 *        the buffer is genuinely complete.
 * The final passthru design advances on DONE (arm-on-completion) and hands
 * consumers `prev`; ARM stays stamped as a cheap diagnostic of partial
 * fills. */
#define SENTINEL 0x5Au
#define SENT_ARM_ROW ((uint32_t)(((FH * 4u) / 5u) | 1u))
#define SENT_DONE_ROW ((uint32_t)(FH - 1u))

static void row_stamp(int b, uint32_t r) {
    uint8_t* row = CAPY[b] + r * CAP_FW;
    uint32_t i;
    for(i = 0; i < CAP_FW; i++)
        row[i] = SENTINEL;
}

static int row_erased(int b, uint32_t r) {
    const uint8_t* row = CAPY[b] + r * CAP_FW;
    uint32_t i, same = 0;
    for(i = 0; i < CAP_FW; i += 8)
        if(row[i] == SENTINEL) same++;
    /* Erased once fewer than a handful of marks survive: a real picture row
     * matching 0x5A at every eighth pixel is not plausible. */
    return same < (CAP_FW / 8u) / 4u;
}

static void sentinel_stamp(int b) {
    row_stamp(b, SENT_ARM_ROW);
    row_stamp(b, SENT_DONE_ROW);
}

/* ---- standard / format --------------------------------------------------- */
void capture_set_standard(vid_std_e s) {
    tvd_disable();
    vid_std = s;
    FH = (s == VID_PAL) ? 576u : 480u;

    /* Ring reset: FH just changed, every stamped row is at the old height
     * and would never read as erased. */
    wr = 0;
    done = -1;
    safe = -1;

    tvd_set_mode(s == VID_PAL ? TVD_MODE_PAL_B : TVD_MODE_NTSC);
    tvd_set_out_fmt(fmt == CAP_FMT_420 ? TVD_FMT_420_PL : TVD_FMT_422_PL);
    tvd_set_bluescreen_mode(TVD_BLUE_OFF);
    /* tvd_set_out_size() programs h/2, so passing 2*FH asks for FH lines -
     * the vendor default leaves it at 240 while consumers expect 480. */
    tvd_set_out_size((uint16_t)CAP_FW, (uint16_t)(2u * FH));
    set32(TVD_BASE + TVD_DMA_CFG, (1u << 26)); /* size/stride valid */
    tvd_set_out_buf(CAPY[wr], CAPC[wr]);
    tvd_enable(); /* DMA started last */

    /* Stamp here, not only at init: set_standard() is also reached from
     * auto-follow, and an unstamped write buffer reads as "already erased"
     * on its first test - the ring then wedges by arming instantly. */
    sentinel_stamp(wr);
}

void capture_set_fmt(cap_fmt_e f) {
    fmt = f;
    capture_set_standard(vid_std);
}

void capture_init(void) {
    tvd_init(TVD_MODE_NTSC, CAPY[0], CAPC[0], 0);
    capture_set_standard(VID_NTSC);
}

void capture_stop(void) {
    tvd_disable();
    done = -1;
    safe = -1;
}

/* ---- frame completion ---------------------------------------------------- */
int capture_poll(void) {
    if(!row_erased(wr, SENT_DONE_ROW)) return 0;
    /* Arm on completion; consumers get the buffer completed one full cycle
     * EARLIER, never the one that just finished. Losing the latch race
     * (about one frame in nine at passthru's poll rate) means the TVD keeps
     * writing the just-done buffer until the next field boundary - reading
     * it then shows a truncated frame with a stale tail. The extra cycle of
     * delay is what makes a lost race cost a repeated frame, never a torn
     * one. (This firmware initially consumed `done` and every capture had a
     * grey bottom third - measured, not theoretical.) */
    safe = done;
    done = wr;
    wr = (wr + 1) % NBUF;
    tvd_set_out_buf(CAPY[wr], CAPC[wr]);
    sentinel_stamp(wr);
    frames++;
    return safe >= 0;
}

int capture_prev(void) {
    return safe;
}
const uint8_t* capture_y(int b) {
    return CAPY[b];
}
const uint8_t* capture_c(int b) {
    return CAPC[b];
}

/* ---- input standard auto-follow ------------------------------------------
 * H_LOCK is deliberately NOT required: decoding a 625-line source with
 * 525-line timing makes H_LOCK marginal (one sample in three drops), so
 * requiring it deadlocks - the board cannot hold lock because it is in the
 * wrong standard, and will not leave the wrong standard because it cannot
 * hold lock. V_LOCK is what makes the line count meaningful. The counter
 * leaks rather than resetting, so one bad frame does not discard the
 * evidence but sustained disagreement drains it. */
#define STD_HYST 30

void capture_follow_input(void) {
    static int agree = 0;
    uint32_t st = tvd_get_state();
    vid_std_e want;

    if(st & TVD_ST_NO_SIGNAL) {
        agree = 0;
        return;
    }
    if(!(st & TVD_ST_V_LOCK)) {
        if(agree) agree--;
        return;
    }
    /* 625_LINES, NOT the PAL bit: a PAL source decoded with NTSC timing
     * reports 625 lines with the PAL bit CLEAR - exactly the case this
     * exists to handle. */
    want = (st & TVD_ST_625_LINES) ? VID_PAL : VID_NTSC;
    if(want == vid_std) {
        if(agree) agree--;
        return;
    }
    if(++agree < STD_HYST) return;
    agree = 0;
    std_switches++;
    capture_set_standard(want);
    printf("[cap] input is %s -> %ux%u\r\n", want == VID_PAL ? "PAL" : "NTSC",
           (unsigned)CAP_FW, (unsigned)FH);
}

/* ---- state --------------------------------------------------------------- */
vid_std_e capture_standard(void) {
    return vid_std;
}
uint16_t capture_height(void) {
    return FH;
}
cap_fmt_e capture_fmt(void) {
    return fmt;
}
uint32_t capture_state(void) {
    return tvd_get_state();
}
int capture_signal_ok(void) {
    uint32_t st = tvd_get_state();
    return !(st & TVD_ST_NO_SIGNAL) && (st & TVD_ST_V_LOCK);
}
uint32_t capture_frames(void) {
    return frames;
}
uint32_t capture_std_switches(void) {
    return std_switches;
}
