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
#include "capture.h"
#include "recorder.h"
#include "armv5_cache.h"
#include "f1c100s_timer.h"

#define PAT_W 720u
#define PAT_H 480u
#define PAT_Y (TESTPAT_BASE)
#define PAT_C (TESTPAT_BASE + 0x200000u)
#define OUT_PHYS (BSRING_BASE + BSRING_DATA_OFF)
#define OUT_MAX (BSRING_SLOT_SIZE - BSRING_DATA_OFF)

static vejpeg_cfg_t cfg = {
    .w = PAT_W, .h = PAT_H, .isp_fmt = 0 /* NV12 */, .samp_2x2 = 1, .quality = 75,
    .no_hdr = 0};
static uint8_t pat_uniform = 0;

static void pattern_fill(void) {
    if(pat_uniform) {
        /* Solid frame: Y=128, U=90, V=200 - invariant under any geometry
         * scrambling, so it separates input-mapping bugs from scan-layout
         * bugs: a wrong-geometry read of a uniform image still encodes the
         * right solid color. */
        uint8_t* y = (uint8_t*)PAT_Y;
        uint8_t* c = (uint8_t*)PAT_C;
        uint32_t i;
        for(i = 0; i < (uint32_t)PAT_W * PAT_H; i++)
            y[i] = 128;
        for(i = 0; i < (uint32_t)PAT_W * PAT_H; i += 2) {
            c[i] = 90;
            c[i + 1] = 200;
        }
    } else if(cfg.isp_fmt == 0) {
        testpat_bars_nv12((uint8_t*)PAT_Y, (uint8_t*)PAT_C, PAT_W, PAT_H);
    } else {
        testpat_bars_nv16((uint8_t*)PAT_Y, (uint8_t*)PAT_C, PAT_W, PAT_H);
    }
    /* TESTPAT region is cacheable and the VE reads through DRAM. */
    cache_clean_range(PAT_Y, PAT_Y + (uint32_t)PAT_W * PAT_H);
    cache_clean_range(PAT_C, PAT_C + (uint32_t)PAT_W * PAT_H);
}

void enctest_toggle_uniform(void) {
    pat_uniform ^= 1;
    pattern_fill();
    printf("[enc] pattern: %s\r\n", pat_uniform ? "uniform Y128 U90 V200" : "bars");
}

void enctest_toggle_hdr(void) {
    cfg.no_hdr ^= 1;
    printf("[enc] header push: %s\r\n", cfg.no_hdr ? "OFF" : "on");
}

/* Where does the hardware REALLY write? Wipe the whole slot with a
 * sentinel, encode once, scan for the touched extent. DRAM persists across
 * watchdog resets, so without the wipe the buffer shows stale streams from
 * previous runs - which is exactly what confused first light. */
void enctest_wipe_slot(void) {
    uint8_t* p = (uint8_t*)BSRING_BASE;
    uint32_t i;
    for(i = 0; i < BSRING_SLOT_SIZE; i++)
        p[i] = 0xEE;
    printf("[enc] slot 0 wiped with 0xEE\r\n");
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

/* ---- pipeline hooks ------------------------------------------------------
 * The live path lives in pipeline.c now (IRQ-driven). The bench keeps two
 * jobs: dump the newest pipeline-encoded frame as a JPEG, and the
 * copy-encode cross-check. */
#include "pipeline.h"

void enctest_dump_pipeline(void) {
    uint16_t qY[64], qC[64];
    static uint8_t prefix[JPEGTAB_HDR_LEN];
    uint32_t phys, blen, plen;
    pipeline_freeze(1);
    if(!pipeline_last(&phys, &blen)) {
        printf("[enc] pipeline has no encoded frame yet\r\n");
        pipeline_freeze(0);
        return;
    }
    jpegtab_quant(pipeline_quality(), qY, qC);
    plen = jpegtab_headers(prefix, qY, qC, CAP_FW, capture_height(), 1);
    printf("-----BEGIN JPEG %lu-----\r\n", (unsigned long)(plen + blen + 2));
    {
        static const uint8_t eoi[2] = {0xFF, 0xD9};
        b64_t b = {0, 0, 0};
        b64_feed(&b, prefix, plen);
        b64_feed(&b, (const uint8_t*)phys, blen);
        b64_feed(&b, eoi, 2);
        b64_finish(&b);
    }
    printf("-----END JPEG-----\r\n");
    pipeline_freeze(0);
}

/* Copy the newest capture frame into the TESTPAT buffers and encode it
 * through the bench path - separates "encoder broken" from "capture-read
 * broken" (it was decisive once already). */
void enctest_copy_encode(void) {
    int p = capture_prev();
    const uint8_t* sy;
    const uint8_t* sc;
    uint8_t* dy = (uint8_t*)PAT_Y;
    uint8_t* dc = (uint8_t*)PAT_C;
    uint32_t i, ylen, clen;
    uint16_t h;

    if(p < 0) {
        printf("[copy] no completed capture frame\r\n");
        return;
    }
    h = capture_height();
    sy = capture_y(p);
    sc = capture_c(p);
    ylen = (uint32_t)CAP_FW * h;
    clen = (capture_fmt() == CAP_FMT_420) ? ylen / 2 : ylen;
    for(i = 0; i < ylen; i++)
        dy[i] = sy[i];
    for(i = 0; i < clen; i++)
        dc[i] = sc[i];
    cache_clean_range(PAT_Y, PAT_Y + ylen);
    cache_clean_range(PAT_C, PAT_C + clen);

    cfg.w = CAP_FW;
    cfg.h = h;
    cfg.isp_fmt = (capture_fmt() == CAP_FMT_420) ? 0 : 2;
    enctest_encode(1);
}

/* Raw-plane truth: dump the newest completed capture buffer decimated 8x
 * (90x60 Y samples, 90x30 UV pairs), bypassing the encoder entirely. This
 * separates "TVD wrote something wrong" from "the encoder read it wrong".
 * Uses the same streaming base64 as the JPEG dump. */
void enctest_rawdump(void) {
    extern void putchar_(char c);
    int p = capture_prev();
    const uint8_t* y;
    const uint8_t* c;
    uint16_t h, row, col;
    b64_t b = {0, 0, 0};

    if(p < 0) {
        printf("[raw] no completed capture frame\r\n");
        return;
    }
    y = capture_y(p);
    c = capture_c(p);
    h = capture_height();
    printf("-----BEGIN RAW %u %u %u-----\r\n", (unsigned)CAP_FW, (unsigned)h,
           (unsigned)capture_fmt());
    for(row = 0; row < h; row += 8)
        for(col = 0; col < CAP_FW; col += 8)
            b64_feed(&b, y + (uint32_t)row * CAP_FW + col, 1);
    {
        uint16_t crows = (capture_fmt() == CAP_FMT_420) ? (uint16_t)(h / 2) : h;
        for(row = 0; row < crows; row += 8)
            for(col = 0; col < CAP_FW; col += 16) {
                b64_feed(&b, c + (uint32_t)row * CAP_FW + col, 2);
            }
    }
    b64_finish(&b);
    printf("-----END RAW-----\r\n");
}

void enctest_scan_slot(void) {
    const uint8_t* p = (const uint8_t*)BSRING_BASE;
    uint32_t first = 0xFFFFFFFFu, last = 0, i, n = 0;
    for(i = 0; i < BSRING_SLOT_SIZE; i++) {
        if(p[i] != 0xEE) {
            if(first == 0xFFFFFFFFu) first = i;
            last = i;
            n++;
        }
    }
    if(first == 0xFFFFFFFFu) {
        printf("[enc] slot untouched\r\n");
        return;
    }
    printf("[enc] written: first=+%lu last=+%lu count=%lu (VLE base is +%lu)\r\n",
           (unsigned long)first, (unsigned long)last, (unsigned long)n,
           (unsigned long)BSRING_DATA_OFF);
    for(i = first & ~15u; i < first + 64 && i <= last; i += 16) {
        uint32_t j;
        printf("  +%06lx:", (unsigned long)i);
        for(j = 0; j < 16; j++)
            printf(" %02x", p[i + j]);
        printf("\r\n");
    }
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

/* Raw sweep hooks: VE 1663 does not match jepoc's ISP format encoding
 * (first light produced tile-scrambled geometry), so the field is swept
 * from the host instead of assumed. The pattern is refilled per format:
 * values that mean 4:2:0 read h/2 chroma rows, 4:2:2 read h. Both layouts
 * are filled (NV16 fill covers the NV12 rows with the same content). */
void enctest_set_fmt(uint8_t v) {
    cfg.isp_fmt = (uint8_t)(v & 0xf);
    testpat_bars_nv16((uint8_t*)PAT_Y, (uint8_t*)PAT_C, PAT_W, PAT_H);
    cache_clean_range(PAT_Y, PAT_Y + (uint32_t)PAT_W * PAT_H);
    cache_clean_range(PAT_C, PAT_C + (uint32_t)PAT_W * PAT_H);
    printf("[enc] isp_fmt=%u (raw)\r\n", cfg.isp_fmt);
}

void enctest_toggle_samp(void) {
    cfg.samp_2x2 ^= 1;
    printf("[enc] SOF0 %s\r\n", cfg.samp_2x2 ? "2x2" : "2x1");
}

void enctest_encode(int dump) {
    uint16_t qY[64], qC[64];
    static uint8_t prefix[JPEGTAB_HDR_LEN];
    uint32_t plen, t0, us;
    uint32_t src_y = PAT_Y, src_c = PAT_C;
    int32_t blen;


    t0 = tim_get_cnt(TIM0);
    vejpeg_start(&cfg, src_y, src_c, OUT_PHYS, OUT_MAX);
    blen = vejpeg_wait(200000); /* generous 200 ms for a first-light poll */
    us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;

    if(blen < 0) {
        printf("[enc] FAILED (%s) after %luus, status=%lx\r\n",
               blen == VEJPEG_ERR_TIMEOUT ? "timeout" : "hw error",
               (unsigned long)us, (unsigned long)vejpeg_status());
        return;
    }
    printf("[enc] ok: %ld bytes in %luus (q=%u fmt=%u hdrbits=%lu/%lu)\r\n",
           (long)blen, (unsigned long)us, cfg.quality, cfg.isp_fmt,
           (unsigned long)vejpeg_dbg_hdr_len, (unsigned long)vejpeg_dbg_hdr_off);

    if(!dump) return;

    jpegtab_quant(cfg.quality, qY, qC);
    plen = jpegtab_headers(prefix, qY, qC, cfg.w, cfg.h, cfg.samp_2x2);

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
