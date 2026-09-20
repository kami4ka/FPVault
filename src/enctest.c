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
/* Bench sub-buffers, all inside TESTPAT_BASE..TESTPAT_SIZE. Generous power-
 * of-two slots rather than exact plane sizes, so a geometry change cannot
 * silently walk one buffer into the next. _Static_assert at the bottom of
 * the probe keeps the whole set inside the region. */
#define PAT_Y (TESTPAT_BASE + 0x000000u) /* <= 720x576 luma   */
#define PAT_C (TESTPAT_BASE + 0x080000u) /* <= 720x576 chroma */
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

/* ---- ISP input-format probe: is there a planar 4:2:0 code? ---------------
 *
 * The display-engine frontend can scale DRAM to DRAM, which is what a 720p
 * path needs. But its write-back emits three separate planes
 * (DE_SCAL_OUTPYUV420, and de_fe.c programs wb_linestrd0/1/2 to prove it),
 * while every documented VE input format is semi-planar - NV12 or NV16,
 * chroma interleaved. jemk's encoder writes input_color_format << 29 with
 * exactly two values, and nothing public says the field holds anything
 * else. The field cannot be read back, so it is swept here instead of
 * assumed. This is the go/no-go for scaling in hardware.
 *
 * The discriminator is exactness, not eyeballing. One picture is laid out
 * twice - once as NV12, once as planar - and encoded from each. A format
 * code that reads the planar buffer correctly hands the DCT the identical
 * sample array, so it must emit a byte-identical bitstream. Matching length
 * AND hash is therefore proof; anything else is not a near miss, it is a
 * different picture. The planar buffer is de-interleaved from the NV12 one
 * rather than generated a second time, so the two cannot drift.
 *
 * Four layouts are tried per code, because a planar mode would need to say
 * things NV12 never has to: chroma plane order (I420 puts U first, YV12 puts
 * V first) and a chroma stride, which for a half-width plane cannot be the
 * luma one. VE_ISP_PIC_STRIDE[15:0] is the only field the driver leaves at
 * zero, so that is where a second stride would live.
 *
 * Geometry is 1280x720, the real target, so a hit is immediately the thing
 * we want - and the reference encode doubles as this project's first 720p
 * timing measurement.
 */
#define PRB_W 1280u
#define PRB_H 720u
#define PRB_Y   (TESTPAT_BASE + 0x100000u) /* luma, shared by both layouts */
#define PRB_NVC (TESTPAT_BASE + 0x1f0000u) /* NV12 interleaved chroma       */
#define PRB_C   (TESTPAT_BASE + 0x270000u) /* I420: U plane then V plane    */
#define PRB_CB  (TESTPAT_BASE + 0x2f0000u) /* YV12: V plane then U plane    */
#define PRB_END (TESTPAT_BASE + 0x370000u)
_Static_assert(PRB_END <= TESTPAT_BASE + TESTPAT_SIZE,
               "bench buffers overflow the region - they would land in the breadcrumbs");

/* FNV-1a. Only ever compared against one reference, so a 32-bit hash is
 * ample: the question is "are these the same bytes", not "find a collision". */
static uint32_t fnv1a(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    while(n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

/* One encode. Returns the bitstream length, or -1; fills *hash on success.
 * A format code that wedges the engine must not poison the codes after it,
 * so a timeout re-inits the VE before returning. */
static int32_t probe_one(vejpeg_cfg_t* c, uint32_t y, uint32_t ch, uint32_t* hash) {
    int32_t blen;
    vejpeg_start(c, y, ch, OUT_PHYS, OUT_MAX);
    blen = vejpeg_wait(200000);
    if(blen < 0) {
        if(blen == VEJPEG_ERR_TIMEOUT) ve_init();
        return -1;
    }
    *hash = fnv1a((const uint8_t*)OUT_PHYS, (uint32_t)blen);
    return blen;
}

void enctest_probe_planar(void) {
    /* Variant 4 is a positive control, not a candidate: the reference buffer
     * re-encoded through the identical comparison path. Its fmt=0 entry MUST
     * read MATCH. If it does not, the probe is measuring itself wrong and a
     * clean sweep of the other four means nothing. */
    static const char* vname[5] = {"I420 clo=0 ", "I420 clo=cs", "YV12 clo=0 ",
                                   "YV12 clo=cs", "NV12 CONTROL"};
    vejpeg_cfg_t c = {
        .w = PRB_W, .h = PRB_H, .isp_fmt = 0, .samp_2x2 = 1, .quality = 75,
        .no_hdr = 0, .isp_stride_lo = 0};
    /* Chroma plane of a planar 4:2:0 frame is half as wide; the luma stride
     * is written in macroblocks, so this is too. 640/16 = 40, exact. */
    const uint16_t cs = (uint16_t)(((PRB_W / 2u) + 15u) / 16u);
    uint32_t ylen = PRB_W * PRB_H, clen = ylen / 4u; /* per chroma plane */
    uint32_t ref_hash = 0, t0, us;
    int32_t ref_len;
    int hits = 0, ctrl_ok = 0;
    uint32_t v, f;

    /* The probe encodes into slot 0 and owns the VE for the duration, so the
     * live path has to stand still. Freezing rather than refusing keeps this
     * to one keystroke: it lets any in-flight encode finish, then stops new
     * ones while capture keeps running. A recorder mid-clip simply gets no
     * frames for the second this takes; the ring absorbs longer stalls than
     * that every time the card hiccups. */
    pipeline_freeze(1);

    /* The reference picture, in the layout the VE is known to read. */
    testpat_bars_nv12((uint8_t*)PRB_Y, (uint8_t*)PRB_NVC, PRB_W, PRB_H);
    cache_clean_range(PRB_Y, PRB_Y + ylen);
    cache_clean_range(PRB_NVC, PRB_NVC + ylen / 2u);

    t0 = tim_get_cnt(TIM0);
    ref_len = probe_one(&c, PRB_Y, PRB_NVC, &ref_hash);
    us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;
    if(ref_len < 0) {
        printf("[isp] reference encode FAILED - probe aborted\r\n");
        pipeline_freeze(0);
        return;
    }
    printf("[isp] ref %ux%u NV12 fmt=0: %ld B, %lu us, hash %08lx\r\n",
           (unsigned)PRB_W, (unsigned)PRB_H, (long)ref_len, (unsigned long)us,
           (unsigned long)ref_hash);

    /* Same picture, de-interleaved. Every layout encodes from the one luma
     * plane - it is byte-identical in NV12 and in planar, so sharing it
     * removes a copy and removes any chance of the two drifting apart.
     * Only the chroma plane changes shape, which is what is under test. */
    {
        const uint8_t* src = (const uint8_t*)PRB_NVC;
        uint8_t* u = (uint8_t*)PRB_C;
        uint8_t* vv = u + clen;
        uint8_t* u2 = (uint8_t*)PRB_CB + clen; /* YV12: V first, then U */
        uint8_t* v2 = (uint8_t*)PRB_CB;
        uint32_t row, col;
        for(row = 0; row < PRB_H / 2u; row++) {
            const uint8_t* s = src + row * PRB_W;
            uint32_t o = row * (PRB_W / 2u);
            for(col = 0; col < PRB_W / 2u; col++) {
                u[o + col] = u2[o + col] = s[2 * col];
                vv[o + col] = v2[o + col] = s[2 * col + 1];
            }
        }
        cache_clean_range(PRB_C, PRB_C + 2u * clen);
        cache_clean_range(PRB_CB, PRB_CB + 2u * clen);
    }

    printf("[isp] sweeping 16 format codes x 4 planar layouts (cs=%u MB)\r\n",
           (unsigned)cs);

    for(v = 0; v < 5; v++) {
        int ctrl = (v == 4);
        uint32_t cbase = ctrl ? PRB_NVC : (v < 2) ? PRB_C : PRB_CB;
        c.isp_stride_lo = (!ctrl && (v & 1u)) ? cs : 0u;
        printf("  %s:", vname[v]);
        for(f = 0; f < 16; f++) {
            uint32_t h = 0;
            int32_t n;
            c.isp_fmt = (uint8_t)f;
            n = probe_one(&c, PRB_Y, cbase, &h);
            if((f & 3u) == 0u && f) printf("\r\n              ");
            if(n < 0)
                printf(" %2lu:ERR   ", (unsigned long)f);
            else if(n == ref_len && h == ref_hash) {
                printf(" %2lu:MATCH ", (unsigned long)f);
                if(ctrl)
                    ctrl_ok = 1;
                else
                    hits++;
            } else
                printf(" %2lu:%-6ld", (unsigned long)f, (long)n);
        }
        printf("\r\n");
    }

    if(!ctrl_ok) {
        printf("[isp] CONTROL FAILED - the reference did not reproduce itself.\r\n");
        printf("[isp] the sweep below it proves nothing; fix the probe first.\r\n");
        pipeline_freeze(0);
        return;
    }
    if(hits)
        printf("[isp] control ok, %d MATCH - planar 4:2:0 reads; the DEFE path is open\r\n",
               hits);
    else
        printf("[isp] control ok, no match in 64 - the VE will not read planar\r\n");
    pipeline_freeze(0);
}
