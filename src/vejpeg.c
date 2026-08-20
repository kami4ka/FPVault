/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * vejpeg.c - see vejpeg.h. Register sequence follows jepoc's main.c order
 * exactly (select -> ISP -> VLE -> ctrl -> params -> SOF0/SOS bits -> quant
 * upload -> launch); deviations from that order were never validated on
 * silicon anywhere, so none are made here.
 */
#include "vejpeg.h"
#include "ve.h"
#include "jpegtab.h"
#include "f1c100s_timer.h"

/* Push raw bits into the VLE bitstream through the basic-bits port. */
static void put_bits(uint8_t nbits, uint32_t data) {
    ve_w(VE_AVC_BASIC_BITS, data);
    ve_w(VE_AVC_TRIGGER, (1u << 16) | ((uint32_t)(nbits & 0x3f) << 8) | 1u);
}

/* SOF0: baseline DCT frame header. Y sampling 2x2 (4:2:0) or 2x1 (4:2:2);
 * chroma always 1x1, quant table 0 for Y, 1 for C. 19 bytes, so the
 * following SOS stays byte-aligned. */
static void put_sof0(uint16_t w, uint16_t h, int samp_2x2) {
    put_bits(16, 0xffc0);
    put_bits(16, 2 + 1 + 2 + 2 + 1 + 3 * 3);
    put_bits(8, 8); /* precision */
    put_bits(16, h);
    put_bits(16, w);
    put_bits(8, 3); /* components */
    put_bits(8, 1); /* Y  */
    put_bits(4, 2);
    put_bits(4, samp_2x2 ? 2 : 1);
    put_bits(8, 0);
    put_bits(8, 2); /* Cb */
    put_bits(4, 1);
    put_bits(4, 1);
    put_bits(8, 1);
    put_bits(8, 3); /* Cr */
    put_bits(4, 1);
    put_bits(4, 1);
    put_bits(8, 1);
}

/* SOS: 14 bytes. Proper baseline spectral bounds (Ss=0, Se=63) - jepoc
 * emitted zeros there because libjpeg left them uninitialized; the hardware
 * treats these bits as opaque payload either way. */
static void put_sos(void) {
    put_bits(16, 0xffda);
    put_bits(16, 2 + 1 + 2 * 3 + 3);
    put_bits(8, 3);
    put_bits(8, 1);
    put_bits(4, 0);
    put_bits(4, 0);
    put_bits(8, 2);
    put_bits(4, 1);
    put_bits(4, 1);
    put_bits(8, 3);
    put_bits(4, 1);
    put_bits(4, 1);
    put_bits(8, 0);  /* Ss */
    put_bits(8, 63); /* Se */
    put_bits(4, 0);  /* Ah */
    put_bits(4, 0);  /* Al */
}

void vejpeg_start(const vejpeg_cfg_t* cfg, uint32_t phy_y, uint32_t phy_c,
                  uint32_t phy_out, uint32_t out_size) {
    uint16_t qY[64], qC[64];
    uint32_t wmb = ((uint32_t)cfg->w + 15) / 16;
    uint32_t hmb = ((uint32_t)cfg->h + 15) / 16;
    uint32_t maxbits, st, data;
    int i;

    ve_select_avc();

    /* ISP input */
    ve_w(VE_ISP_PIC_LUMA, phy_y);
    ve_w(VE_ISP_PIC_CHROMA, phy_c);
    ve_w(VE_ISP_PIC_SIZE, ((wmb & 0x3ff) << 16) | (hmb & 0x3ff));
    ve_w(VE_ISP_PIC_STRIDE, (wmb & 0x3ff) << 16);
    ve_w(VE_ISP_CTRL, ((uint32_t)(cfg->isp_fmt & 0xf)) << 28);

    /* VLE output */
    maxbits = (out_size * 8 + 0xffffu) & ~0xffffu;
    if(maxbits > 0x0fff0000u) maxbits = 0x0fff0000u;
    ve_w(VE_AVC_VLE_ADDR, phy_out);
    ve_w(VE_AVC_VLE_END, phy_out + out_size - 1);
    ve_w(VE_AVC_VLE_OFFSET, 0);
    ve_w(VE_AVC_VLE_MAX, maxbits);

    /* ctrl: JPEG mode, clear stale status */
    ve_w(VE_AVC_CTRL, 0x0000000f);
    ve_w(VE_AVC_TRIGGER, 1u << 16);
    st = ve_r(VE_AVC_STATUS);
    ve_w(VE_AVC_STATUS, st | 0xf);

    /* headers into the bitstream */
    put_sof0(cfg->w, cfg->h, cfg->samp_2x2);
    put_sos();

    /* quantizer: bias from the DC steps, tables pre-reciprocated, natural
     * order, Y then C (must be the same tables the file prefix carries) */
    jpegtab_quant(cfg->quality, qY, qC);
    ve_w(VE_AVC_PARAM, (1u << 31) | (1u << 30) |
                           (((0x400u / qC[0]) & 0x7ff) << 16) |
                           ((0x400u / qY[0]) & 0x7ff));
    ve_w(VE_AVC_SDRAM_INDEX, 0);
    for(i = 0; i < 64; i++) {
        data = 0x0000ffff & (0xffffu / qY[i]);
        data |= 0x00ff0000 & (((uint32_t)(qY[i] + 1) / 2) << 16);
        ve_w(VE_AVC_SDRAM_DATA, data);
    }
    for(i = 0; i < 64; i++) {
        data = 0x0000ffff & (0xffffu / qC[i]);
        data |= 0x00ff0000 & (((uint32_t)(qC[i] + 1) / 2) << 16);
        ve_w(VE_AVC_SDRAM_DATA, data);
    }

    /* launch */
    ve_w(VE_AVC_TRIGGER, (1u << 16) | 8u);
}

uint32_t vejpeg_status(void) {
    return ve_r(VE_AVC_STATUS) & 0xf;
}

int32_t vejpeg_wait(uint32_t timeout_us) {
    uint32_t t0 = tim_get_cnt(TIM0); /* down-counter at 24 MHz */
    uint32_t st;
    for(;;) {
        st = vejpeg_status();
        if(st & 1) {
            uint32_t len = ve_r(VE_AVC_VLE_LENGTH) / 8;
            ve_w(VE_AVC_STATUS, ve_r(VE_AVC_STATUS) | 0xf);
            return (int32_t)len;
        }
        if(st & 2) {
            ve_w(VE_AVC_STATUS, ve_r(VE_AVC_STATUS) | 0xf);
            return VEJPEG_ERR_FAILED;
        }
        if((uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u > timeout_us)
            return VEJPEG_ERR_TIMEOUT;
    }
}
