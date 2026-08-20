/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * enctest.c - see enctest.h.
 *
 * Buffers: pattern planes live in the cacheable TESTPAT region (the CPU
 * writes them, so they must be cache-cleaned before the VE reads); the
 * bitstream goes to slot 0 of the (non-cacheable) bitstream ring at +64,
 * exactly where the recorder will later put it - the 64-byte headroom is
 * the future AVI chunk header.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "enctest.h"
#include "ve.h"
#include "vejpeg.h"
#include "jpegtab.h"
#include "testpat.h"
#include "armv5_cache.h"
#include "f1c100s_timer.h"

#define PAT_W 720u
#define PAT_H 480u
#define PAT_Y (TESTPAT_BASE)
#define PAT_C (TESTPAT_BASE + 0x200000u)
#define OUT_PHYS (BSRING_BASE + 64u)
#define OUT_MAX (BSRING_SLOT_SIZE - 64u)

static vejpeg_cfg_t cfg = {
    .w = PAT_W, .h = PAT_H, .isp_fmt = 0 /* NV12 */, .samp_2x2 = 1, .quality = 75};

static void pattern_fill(void) {
    if(cfg.isp_fmt == 0)
        testpat_bars_nv12((uint8_t*)PAT_Y, (uint8_t*)PAT_C, PAT_W, PAT_H);
    else
        testpat_bars_nv16((uint8_t*)PAT_Y, (uint8_t*)PAT_C, PAT_W, PAT_H);
    /* TESTPAT region is cacheable and the VE reads through DRAM. */
    cache_clean_range(PAT_Y, PAT_Y + (uint32_t)PAT_W * PAT_H);
    cache_clean_range(PAT_C, PAT_C + (uint32_t)PAT_W * PAT_H);
}

void enctest_init(void) {
    ve_init();
    pattern_fill();
    printf("[ve] version %08lx (top half is the ID)\r\n",
           (unsigned long)ve_version());
}

void enctest_info(void) {
    printf("[ve] version %08lx  avc_status %lx  vle_len %lu bits\r\n",
           (unsigned long)ve_version(), (unsigned long)vejpeg_status(),
           (unsigned long)ve_r(VE_AVC_VLE_LENGTH));
    printf("    cfg %ux%u isp_fmt=%u samp=%s q=%u\r\n", cfg.w, cfg.h, cfg.isp_fmt,
           cfg.samp_2x2 ? "2x2" : "2x1", cfg.quality);
}

void enctest_cycle_quality(void) {
    cfg.quality = (cfg.quality == 50) ? 75 : (cfg.quality == 75) ? 90 : 50;
    printf("[enc] quality %u\r\n", cfg.quality);
}

void enctest_cycle_fmt(void) {
    /* NV12=0 (agreed by both references) -> NV16 candidates 1 and 2 (the
     * references disagree; silicon will vote). Sampling follows: NV12 is
     * 4:2:0, the NV16 experiments try 4:2:2 SOF0. */
    cfg.isp_fmt = (cfg.isp_fmt == 0) ? 1 : (cfg.isp_fmt == 1) ? 2 : 0;
    cfg.samp_2x2 = (cfg.isp_fmt == 0);
    pattern_fill();
    printf("[enc] isp_fmt %u (%s), SOF0 %s\r\n", cfg.isp_fmt,
           cfg.isp_fmt == 0 ? "NV12" : "NV16?", cfg.samp_2x2 ? "2x2" : "2x1");
}

/* Streaming base64: the JPEG is emitted as prefix + hardware bitstream +
 * EOI in three feeds, so the encoder carries its 3-byte group state across
 * them and pads only once at the end. */
static const char b64c[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

extern void putchar_(char c);

typedef struct {
    uint32_t acc;
    uint8_t nbytes;
    uint8_t col;
} b64_t;

static void b64_emit4(b64_t* b, char c1, char c2, char c3, char c4) {
    putchar_(c1);
    putchar_(c2);
    putchar_(c3);
    putchar_(c4);
    if((b->col += 4) >= 76) {
        b->col = 0;
        printf("\r\n");
    }
}

static void b64_feed(b64_t* b, const uint8_t* p, uint32_t n) {
    uint32_t i;
    for(i = 0; i < n; i++) {
        b->acc = (b->acc << 8) | p[i];
        if(++b->nbytes == 3) {
            b64_emit4(b, b64c[(b->acc >> 18) & 63], b64c[(b->acc >> 12) & 63],
                      b64c[(b->acc >> 6) & 63], b64c[b->acc & 63]);
            b->nbytes = 0;
            b->acc = 0;
        }
    }
}

static void b64_finish(b64_t* b) {
    if(b->nbytes == 1) {
        b->acc <<= 16;
        b64_emit4(b, b64c[(b->acc >> 18) & 63], b64c[(b->acc >> 12) & 63], '=', '=');
    } else if(b->nbytes == 2) {
        b->acc <<= 8;
        b64_emit4(b, b64c[(b->acc >> 18) & 63], b64c[(b->acc >> 12) & 63],
                  b64c[(b->acc >> 6) & 63], '=');
    }
    printf("\r\n");
}

void enctest_encode(int dump) {
    uint16_t qY[64], qC[64];
    static uint8_t prefix[JPEGTAB_PREFIX_MAX];
    uint32_t plen, t0, us;
    int32_t blen;

    t0 = tim_get_cnt(TIM0);
    vejpeg_start(&cfg, PAT_Y, PAT_C, OUT_PHYS, OUT_MAX);
    blen = vejpeg_wait(200000); /* generous 200 ms for a first-light poll */
    us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;

    if(blen < 0) {
        printf("[enc] FAILED (%s) after %luus, status=%lx\r\n",
               blen == VEJPEG_ERR_TIMEOUT ? "timeout" : "hw error",
               (unsigned long)us, (unsigned long)vejpeg_status());
        return;
    }
    printf("[enc] ok: %ld bytes in %luus (q=%u fmt=%u)\r\n", (long)blen,
           (unsigned long)us, cfg.quality, cfg.isp_fmt);

    if(!dump) return;

    jpegtab_quant(cfg.quality, qY, qC);
    plen = jpegtab_prefix(prefix, qY, qC);

    printf("-----BEGIN JPEG %lu-----\r\n", (unsigned long)(plen + blen + 2));
    {
        static const uint8_t eoi[2] = {0xFF, 0xD9};
        b64_t b = {0, 0, 0};
        b64_feed(&b, prefix, plen);
        b64_feed(&b, (const uint8_t*)OUT_PHYS, (uint32_t)blen);
        b64_feed(&b, eoi, 2);
        b64_finish(&b);
    }
    printf("-----END JPEG-----\r\n");
}
