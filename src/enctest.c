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

/* ---- display-engine frontend probe: what does each write-back format do? -
 *
 * The frontend is the only scaler on this part, and the ISP probe above
 * showed the VE will read nothing but semi-planar. So the whole question of
 * hardware scaling now rests on one field: OUTPUT_FMT's 3-bit data_fmt.
 * Allwinner's enum names seven of its eight values - 0,1,2 for RGB and
 * 4,5,6,7 for planar YUV 444/420/422/411 - and leaves 3 unnamed. If 3 is a
 * UV-combined write-back the frontend can feed the VE directly and the
 * peripherals-only path opens. If it is not, nothing else in the field is.
 *
 * Reading the value is not enough; the register is write-only in effect, so
 * the test is what lands in DRAM. Each pass runs at 1:1 with the FIR
 * bypassed, so the output is a pure format conversion and can be compared
 * byte for byte against layouts we construct ourselves. Bypassing the
 * filter also separates the two questions: this probe asks what shape the
 * output is, not how good the resampling looks.
 *
 * The destination is wiped with a sentinel before every pass, so a format
 * that writes nothing is distinguishable from one that writes garbage - the
 * same trick that sorted out first light on the VE.
 *
 * 320x240 rather than 720p: a format is a format at any size, and small
 * keeps the whole thing inside the bench region and fast enough to sweep.
 */
#include "defe.h"

#define DFE_W 320u
#define DFE_H 240u
/* Sub-buffers inside the ISP probe's chroma slot. The two probes are both
 * bench commands and never run at the same time, so sharing costs nothing
 * and keeps the bench region within the size the _Static_assert guards. */
#define DFE_SY (PRB_C + 0x00000u) /* source luma,   320x240 */
#define DFE_SC (PRB_C + 0x14000u) /* source chroma, NV12    */
#define DFE_D0 (PRB_C + 0x20000u) /* write-back plane 0     */
#define DFE_D1 (PRB_C + 0x34000u) /* write-back plane 1     */
#define DFE_D2 (PRB_C + 0x40000u) /* write-back plane 2     */
#define DFE_YLEN (DFE_W * DFE_H)
#define DFE_CLEN (DFE_YLEN / 2u)  /* NV12 chroma plane      */
#define DFE_PLEN (DFE_YLEN / 4u)  /* one planar chroma      */
#define DFE_SENT 0xC7u
/* Twice the luma plane: wide enough to catch a write-back that carries
 * chroma along behind the luma instead of stopping at one plane. */
#define DFE_WIN (DFE_YLEN * 2u)

/* How many leading bytes of a wiped buffer the engine actually touched. */
static uint32_t touched(uint32_t base, uint32_t n) {
    const uint8_t* p = (const uint8_t*)base;
    uint32_t i, last = 0;
    for(i = 0; i < n; i++)
        if(p[i] != DFE_SENT) last = i + 1;
    return last;
}

void enctest_probe_defe(void) {
    static uint8_t u[DFE_PLEN], v[DFE_PLEN];
    uint32_t row, col, st, is, wy, w1, w2;
    uint32_t m;
    int hits = 0;

    pipeline_freeze(1);
    defe_init();
    printf("[defe] en=%08lx coef=%08lx\r\n", (unsigned long)defe_r(DEFE_EN),
           (unsigned long)defe_coef_readback());

    testpat_bars_nv12((uint8_t*)DFE_SY, (uint8_t*)DFE_SC, DFE_W, DFE_H);
    for(row = 0; row < DFE_H / 2u; row++) {
        const uint8_t* s2 = (const uint8_t*)DFE_SC + row * DFE_W;
        for(col = 0; col < DFE_W / 2u; col++) {
            u[row * (DFE_W / 2u) + col] = s2[2 * col];
            v[row * (DFE_W / 2u) + col] = s2[2 * col + 1];
        }
    }
    cache_clean_range(DFE_SY, DFE_SY + DFE_YLEN);
    cache_clean_range(DFE_SC, DFE_SC + DFE_CLEN);

    /* Output never matched input, and it did not match with the FIR
     * bypassed either - which rules the filter out and leaves the stage
     * beside it. BYPASS holds two bits and only one of them has ever been
     * varied here: the colour converter has been left bypassed on the
     * strength of mainline's naming, never tested. Both bits are swept
     * against every output format, 1:1, and an exact round trip is the only
     * thing that counts. */
    /* Does the engine read the source at all?
     *
     * Every sweep so far has asked "is the output the input", and the
     * answer has always been no - which cannot distinguish an engine that
     * transforms the data wrongly from one that never reads it. A constant
     * source settles that in one step: if the output is uniform and its
     * value tracks the input value, the read path works and only the
     * transform is wrong. If the output is noise regardless of what the
     * source holds, the engine is not reading our buffer.
     */
    /* Calibrate the colour matrix against the part.
     *
     * The constant-source test showed the engine does read our buffer - the
     * output tracks the input - but every non-zero sample came out zero,
     * which is what a unity coefficient that is not actually unity does.
     * Mainline documents 10 fractional bits, making 1024 unity; that is
     * evidently not the scale here. So the diagonal is swept and the value
     * that carries a constant through unchanged is the right one. */
    printf("[defe] CSC diagonal sweep, constant source 0x80, fmt 4\r\n");
    for(m = 0; m < 8u; m++) {
        static const uint32_t diag[8] = {0x20,  0x40,  0x80,  0x100,
                                         0x200, 0x400, 0x800, 0x1000};
        defe_cfg_t c = {0};
        uint32_t i2;
        int rc, uniform = 1;
        const uint8_t* o = (const uint8_t*)DFE_D0;

        for(i2 = 0; i2 < DFE_YLEN; i2++) ((uint8_t*)DFE_SY)[i2] = 0x80;
        for(i2 = 0; i2 < DFE_CLEN; i2++) ((uint8_t*)DFE_SC)[i2] = 0x80;
        cache_clean_range(DFE_SY, DFE_SY + DFE_YLEN);
        cache_clean_range(DFE_SC, DFE_SC + DFE_CLEN);

        defe_set_csc_diag(diag[m]);
        defe_reset();
        for(i2 = 0; i2 < DFE_WIN; i2++) ((uint8_t*)DFE_D0)[i2] = DFE_SENT;
        cache_flush_range(DFE_D0, DFE_D0 + DFE_WIN);

        c.src_y = DFE_SY;
        c.src_c = DFE_SC;
        c.sw = c.dw = DFE_W;
        c.sh = c.dh = DFE_H;
        c.dst0 = DFE_D0;
        c.dst_strd0 = DFE_W;
        c.out_fmt = 4;
        c.out_ctrl = 1;
        c.use_bits = 1;
        c.bypass_bits = 0;
        defe_start(&c);
        rc = defe_wait(20000, &st, &is);
        cache_inv_range(DFE_D0, DFE_D0 + DFE_WIN);
        wy = touched(DFE_D0, DFE_WIN);
        for(i2 = 1; i2 < DFE_YLEN; i2++)
            if(o[i2] != o[0]) { uniform = 0; break; }
        printf("  diag %04lx -> %s wrote %6lu  out[0]=%02x %s%s\r\n",
               (unsigned long)diag[m], rc ? "TO  " : "done",
               (unsigned long)wy, (unsigned)o[0],
               uniform ? "UNIFORM" : "varies",
               (uniform && o[0] == 0x80) ? "  <== UNITY" : "");
        if(uniform && o[0] == 0x80) hits++;
    }
    defe_set_csc_diag(1024);

    (void)w1;
    (void)w2;
    (void)u;
    (void)v;
    if(hits)
        printf("[defe] %d exact - the frontend is a usable single-plane "
               "scaler\r\n", hits);
    else
        printf("[defe] nothing round-trips; no usable configuration found\r\n");
    pipeline_freeze(0);
}

/* ---- which frontend registers does THIS silicon actually implement? -----
 *
 * The register map was taken from mainline's sun4i frontend, Allwinner's
 * sun7i BSP and a Lichee Nano player, on the assumption that suniv carries
 * the same block. Mostly it does - the input side reads back every value
 * written to it. The write-back side does not: WB_ADDR1, WB_STRD_EN and
 * WB_STRD0 all read zero straight after being written, which is what an
 * unimplemented register looks like.
 *
 * That distinction decides the whole question. Three write-back addresses
 * with three strides is a planar sink; one address is a packed one. So this
 * walks the register file and reports which bits are actually storage,
 * rather than inferring it from a relative's documentation.
 *
 * EN and FRM_CTRL are excluded: writing ones to those starts a frame, and a
 * frame started with a scan pattern in the address registers would DMA over
 * whatever the addresses happen to point at.
 */
void enctest_scan_defe(void) {
    uint32_t off;
    int n = 0;

    pipeline_freeze(1);
    defe_init();
    printf("[defe] register scan 0x008..0x2fc (EN and FRM_CTRL skipped)\r\n");
    for(off = 0x008; off <= 0x2fc; off += 4) {
        uint32_t orig = defe_r(off), a, b, mask;
        defe_w(off, 0xFFFFFFFFu);
        a = defe_r(off);
        defe_w(off, 0x00000000u);
        b = defe_r(off);
        defe_w(off, orig);
        mask = a ^ b;
        if(mask) {
            if((n & 3) == 0) printf("   ");
            printf(" %03lx:%08lx", (unsigned long)off, (unsigned long)mask);
            if((++n & 3) == 0) printf("\r\n");
        }
    }
    if(n & 3) printf("\r\n");
    printf("[defe] %d registers hold bits; anything absent above reads back "
           "zero and is not implemented here\r\n", n);
    defe_reset();
    pipeline_freeze(0);
}

/* ---- what would the CPU chroma pass actually cost? ----------------------
 *
 * The frontend has no chroma channel, so a 720p path has to build the
 * chroma plane in software: read the captured NV12 chroma (720x240 UV
 * pairs), resample it to the output geometry, and write it back still
 * interleaved, because interleaved is the only thing the VE will read.
 *
 * That cost is the one number that decides whether 30 fps survives, and it
 * is the last one in the budget that was still an estimate. So it is timed
 * here rather than argued about: the real loop, the real sizes, the real
 * buffers, averaged over enough frames to swamp the timer's granularity.
 *
 * Nearest-neighbour rather than filtered. Chroma is already at a quarter
 * resolution and the source is analog composite; a better kernel would cost
 * more and show less. If nearest neighbour does not fit, nothing does.
 */
#define CHR_SW 720u  /* source: bytes per interleaved chroma row */
#define CHR_SH 240u  /* source: chroma rows (NTSC 4:2:0)         */
#define CHR_DW 1280u /* dest:   bytes per interleaved chroma row */
#define CHR_DH 360u  /* dest:   chroma rows at 1280x720          */
#define CHR_SRC PRB_NVC
#define CHR_DST PRB_Y

void enctest_time_chroma(void) {
    const uint8_t* src = (const uint8_t*)CHR_SRC;
    uint8_t* dst = (uint8_t*)CHR_DST;
    uint32_t t0, us, n, i;
    const uint32_t reps = 30;

    pipeline_freeze(1);
    for(i = 0; i < CHR_SW * CHR_SH; i++) ((uint8_t*)CHR_SRC)[i] = (uint8_t)i;

    t0 = tim_get_cnt(TIM0);
    for(n = 0; n < reps; n++) {
        uint32_t row;
        for(row = 0; row < CHR_DH; row++) {
            /* Source row for this output row, and a fixed-point step across
             * it. The accumulator counts UV pairs, so the low bit of the
             * byte index is never disturbed and U stays U. */
            const uint8_t* s = src + ((row * CHR_SH) / CHR_DH) * CHR_SW;
            uint8_t* d = dst + row * CHR_DW;
            uint32_t acc = 0;
            const uint32_t step = ((CHR_SW / 2u) << 16) / (CHR_DW / 2u);
            uint32_t col;
            for(col = 0; col < CHR_DW; col += 2) {
                uint32_t sp = (acc >> 16) << 1;
                d[col] = s[sp];
                d[col + 1] = s[sp + 1];
                acc += step;
            }
        }
    }
    us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;

    printf("[chroma] %ux%u -> %ux%u interleaved: %lu us per frame "
           "(%lu reps, %lu us total)\r\n",
           (unsigned)(CHR_SW / 2u), (unsigned)CHR_SH, (unsigned)(CHR_DW / 2u),
           (unsigned)CHR_DH, (unsigned long)(us / reps), (unsigned long)reps,
           (unsigned long)us);
    printf("[chroma] %lu%% of a 33367 us frame, %lu KB written per frame\r\n",
           (unsigned long)((us / reps) * 100u / 33367u),
           (unsigned long)(CHR_DW * CHR_DH / 1024u));

    /* How much of that is the loop and how much is the memory? A word-wise
     * copy of the same volume is the floor: no resampling can beat it, so
     * it says whether optimising the loop is worth anything or whether the
     * pass is already bandwidth-bound and 28 ms is close to the best there
     * is. Same buffers, same cacheability, same size. */
    {
        uint32_t words = (CHR_DW * CHR_DH) / 4u;
        volatile uint32_t* d32;
        const uint32_t* s32;
        t0 = tim_get_cnt(TIM0);
        for(n = 0; n < reps; n++) {
            d32 = (volatile uint32_t*)CHR_DST;
            s32 = (const uint32_t*)CHR_SRC;
            for(i = 0; i < words; i++) d32[i] = s32[i & 0xffffu];
        }
        us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;
        printf("[chroma] word-copy floor for the same %lu KB: %lu us "
               "(%lu%% of a frame, %lu MB/s)\r\n",
               (unsigned long)(CHR_DW * CHR_DH / 1024u),
               (unsigned long)(us / reps),
               (unsigned long)((us / reps) * 100u / 33367u),
               (unsigned long)((uint64_t)CHR_DW * CHR_DH * reps /
                               (us ? us : 1u)));
    }
    pipeline_freeze(0);
}

/* ---- where does the coefficient RAM actually live? ----------------------
 *
 * The frontend runs, completes passes and DMAs correctly, but no
 * configuration reproduces its input - and the one clue is that reading
 * back the first horizontal coefficient returns something that was written
 * somewhere else entirely (0x00004000, which is vert_coef[0], and on
 * another run 0x0000ff03, which is horz_coef[3]). Mainline describes six
 * flat 32-word banks at 0x400, 0x480, 0x500, 0x600, 0x680 and 0x700. If
 * that were the layout here, a word written to an offset would read back
 * from that offset.
 *
 * So rather than trust the layout, each word in the region is stamped with
 * its own offset and the whole region read back. Three outcomes tell three
 * different stories: every word reading its own offset means flat RAM and
 * the fault is elsewhere; one value everywhere means a single data port
 * behind an index register; a repeating pattern means a smaller RAM
 * aliased across the window.
 */
void enctest_probe_coef(void) {
    uint32_t off, bad = 0, n = 0;
    static const uint32_t bank[] = {0x400, 0x480, 0x500, 0x600, 0x680, 0x700};
    unsigned b, k;

    pipeline_freeze(1);
    defe_init();

    /* Does the window hold bits at all, under the same gating the loader
     * uses? Without this a region that simply is not there looks identical
     * to one that is there and misbehaving. */
    defe_w(DEFE_EN, DEFE_EN_ENABLE | DEFE_EN_BIST);
    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) | DEFE_FRM_COEF_ACC);

    for(off = 0x400; off <= 0x7fc; off += 4) {
        uint32_t a, b2;
        defe_w(off, 0xFFFFFFFFu);
        a = defe_r(off);
        defe_w(off, 0x00000000u);
        b2 = defe_r(off);
        if(a ^ b2) n++;
    }
    printf("[coef] %lu of 256 words in 0x400..0x7fc hold bits\r\n",
           (unsigned long)n);

    /* Stamp every word with its own offset, then read the region back. */
    for(off = 0x400; off <= 0x7fc; off += 4)
        defe_w(off, off);
    for(off = 0x400; off <= 0x7fc; off += 4) {
        if(defe_r(off) != off) bad++;
    }
    printf("[coef] %lu of 256 words read back something other than their own "
           "offset\r\n", (unsigned long)bad);

    for(b = 0; b < sizeof bank / sizeof bank[0]; b++) {
        printf("  bank %03lx:", (unsigned long)bank[b]);
        for(k = 0; k < 6; k++)
            printf(" %08lx", (unsigned long)defe_r(bank[b] + k * 4));
        printf("\r\n");
    }

    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) & ~DEFE_FRM_COEF_ACC);
    defe_w(DEFE_EN, DEFE_EN_ENABLE);

    /* And the same read with access revoked: if the gating is real the
     * values should change or stop responding. */
    printf("  ungated  :");
    for(k = 0; k < 6; k++)
        printf(" %08lx", (unsigned long)defe_r(0x400 + k * 4));
    printf("\r\n");

    defe_reset();
    pipeline_freeze(0);
}
