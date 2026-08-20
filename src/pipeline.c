/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pipeline.c - see pipeline.h.
 *
 * SPSC ring: head is only written by the IRQ (producer), tail only by the
 * main loop (consumer); both are volatile, the payload (slot_len) is
 * written before the head advance. Single core, no further barriers.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "pipeline.h"
#include "capture.h"
#include "vejpeg.h"
#include "recorder.h"
#include "f1c100s_timer.h"

static uint8_t on = 0, cap_started = 0;
static vejpeg_cfg_t cfg = {
    .w = CAP_FW, .h = 480, .isp_fmt = 2, .samp_2x2 = 1, .quality = 75, .no_hdr = 0};

static volatile uint32_t head = 0, tail = 0;
static volatile uint32_t slot_len[BSRING_SLOTS];

static volatile uint8_t enc_busy = 0;
static volatile uint8_t frozen = 0;
static int enc_pending = -1;
static uint32_t enc_start_t;
static uint32_t follow_div = 0;

/* stats */
static volatile uint32_t enc_frames = 0, enc_fails = 0, ring_drops = 0;
static uint32_t ring_hiwater = 0;

static uint32_t slot_base(uint32_t idx) {
    return BSRING_BASE + (idx % BSRING_SLOTS) * BSRING_SLOT_SIZE;
}

static void pair_formats(void) {
    /* TVD 4:2:0 -> ISP NV12 (0); TVD 4:2:2 -> ISP NV16 (2, the H3 bit-29
     * convention). Both verified on silicon with live frames. */
    cfg.isp_fmt = (capture_fmt() == CAP_FMT_420) ? 0 : 2;
    cfg.samp_2x2 = 1;
}

void pipeline_toggle(void) {
    if(!cap_started) {
        capture_init();
        cap_started = 1;
    }
    on = (uint8_t)!on;
    if(on) pair_formats();
    printf("[pipe] %s (tvd %s, isp_fmt %u)\r\n", on ? "ON" : "off",
           capture_fmt() == CAP_FMT_420 ? "420" : "422", cfg.isp_fmt);
}

void pipeline_fmt_toggle(void) {
    if(!cap_started) {
        printf("[pipe] enable first (:c)\r\n");
        return;
    }
    capture_set_fmt(capture_fmt() == CAP_FMT_420 ? CAP_FMT_422 : CAP_FMT_420);
    pair_formats();
    printf("[pipe] tvd %s, isp_fmt %u\r\n",
           capture_fmt() == CAP_FMT_420 ? "420" : "422", cfg.isp_fmt);
}

int pipeline_active(void) {
    return on;
}
int pipeline_quality(void) {
    return cfg.quality;
}

void pipeline_tick(void) {
    uint32_t depth;

    if(!on) return;

    if(capture_tick()) enc_pending = capture_prev();

    if(enc_busy) {
        int32_t r = vejpeg_poll_done();
        if(r >= 0) {
            slot_len[head % BSRING_SLOTS] = (uint32_t)r;
            head++; /* publish AFTER the length */
            enc_busy = 0;
            enc_frames++;
        } else if(r == VEJPEG_ERR_FAILED) {
            enc_busy = 0;
            enc_fails++;
        } else if((uint32_t)(enc_start_t - tim_get_cnt(TIM0)) / 24u > 50000u) {
            enc_busy = 0;
            enc_fails++;
        }
    }

    depth = head - tail;
    if(depth > ring_hiwater) ring_hiwater = depth;

    if(!enc_busy && !frozen && enc_pending >= 0) {
        if(depth >= BSRING_SLOTS) {
            /* ring full: drop at the cheapest point - skip the encode */
            ring_drops++;
            enc_pending = -1;
        } else {
            cfg.h = capture_height();
            vejpeg_start(&cfg, (uint32_t)capture_y(enc_pending),
                         (uint32_t)capture_c(enc_pending),
                         slot_base(head) + BSRING_DATA_OFF,
                         BSRING_SLOT_SIZE - BSRING_DATA_OFF);
            enc_start_t = tim_get_cnt(TIM0);
            enc_busy = 1;
            enc_pending = -1;
        }
    }

    /* input-standard auto-follow at 50 Hz; it must run with no signal too */
    if(++follow_div >= 20u) {
        follow_div = 0;
        capture_follow_input();
    }
}

void pipeline_consume(void) {
    while(tail != head) {
        uint32_t i = tail % BSRING_SLOTS;
        recorder_on_frame(slot_base(tail), slot_len[i], cfg.quality);
        tail++;
    }
}

void pipeline_freeze(int f) {
    frozen = (uint8_t)(f != 0);
    if(f) {
        /* let an in-flight encode finish (max ~3 ms) */
        while(enc_busy)
            ;
    }
}

int pipeline_last(uint32_t* phys, uint32_t* len) {
    uint32_t h = head;
    if(h == 0) return 0;
    *phys = slot_base(h - 1) + BSRING_DATA_OFF;
    *len = slot_len[(h - 1) % BSRING_SLOTS];
    return 1;
}

void pipeline_stats(void) {
    static uint32_t last_in = 0, last_enc = 0;
    uint32_t in = capture_frames();
    if(!on) return;
    printf("[pipe] in %lu fps, enc %lu fps, fails %lu, ring %lu/%lu hi %lu, "
           "drops %lu, %s\r\n",
           (unsigned long)(in - last_in), (unsigned long)(enc_frames - last_enc),
           (unsigned long)enc_fails, (unsigned long)(head - tail),
           (unsigned long)BSRING_SLOTS, (unsigned long)ring_hiwater,
           (unsigned long)ring_drops, capture_signal_ok() ? "LOCK" : "no-signal");
    last_in = in;
    last_enc = enc_frames;
}
