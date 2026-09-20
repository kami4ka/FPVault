/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * defe.h - the display engine frontend, driven as a memory-to-memory engine.
 *
 * The frontend exists to scale a video layer on its way to the display
 * backend, but it can also write its result straight back to DRAM: set
 * wb_en in FRM_CTRL and it reads planes from BUF_ADDR*, scales through a
 * polyphase FIR, and writes to WB_ADDR*. Nothing downstream has to be
 * clocked - no backend, no TCON, no encoder - which is what makes it usable
 * on a record-only board that has no display path at all.
 *
 * Bit definitions come from three places that agree with each other:
 * mainline's sun4i_frontend.h (the register layout), Allwinner's own
 * de_fe.h from their U-Boot (the write-back registers mainline never uses),
 * and pecostm32's Lichee Nano image player (the same block driven bare-metal
 * on an F1C100s, which is the only proof that write-back works on this
 * silicon rather than merely on its bigger relatives).
 *
 * Register offsets not already in vendor f1c100s_de.h are defined here
 * rather than added there, to keep the vendor tree unmodified.
 */
#pragma once

#include <stdint.h>
/* The bulk of the register offsets - DEFE_EN, DEFE_FRM_CTRL, the addresses,
 * strides, sizes and factors - already live in the vendor header. Only the
 * ones it lacks are added below. */
#include "f1c100s_de.h"

/* --- registers f1c100s_de.h is missing ---------------------------------- */
#define DEFE_WB_ADDR0     0x050
#define DEFE_WB_ADDR1     0x054
#define DEFE_WB_ADDR2     0x058
#define DEFE_WB_STRD_EN   0x0d0
#define DEFE_WB_STRD0     0x0d4
#define DEFE_WB_STRD1     0x0d8
#define DEFE_WB_STRD2     0x0dc
#define DEFE_CH1_IN_SIZE  0x200
#define DEFE_CH1_OUT_SIZE 0x204
#define DEFE_CH1_H_FACT   0x208
#define DEFE_CH1_V_FACT   0x20c
#define DEFE_CH0_H_COEF1  0x480
#define DEFE_CH1_H_COEF1  0x680
#define DEFE_CH0_H_PHASE  0x110
#define DEFE_CH0_V_PHASE0 0x114
#define DEFE_CH0_V_PHASE1 0x118
#define DEFE_CH1_H_PHASE  0x210
#define DEFE_CH1_V_PHASE0 0x214
#define DEFE_CH1_V_PHASE1 0x218
/* Initial FIR phase, per channel. Mainline carries these per SoC and sun4i
 * - the closest documented relative of suniv - uses 0 for luma and 0xfc000
 * for chroma, which is the half-sample siting offset 4:2:0 chroma needs. */
#define DEFE_PHASE_LUMA   0x00000u
#define DEFE_PHASE_CHROMA 0xfc000u

/* --- control bits -------------------------------------------------------- */
#define DEFE_EN_ENABLE    (1u << 0)
/* Arbitration of the coefficient RAM. Set = the CPU owns it, clear = the
 * engine does. The Lichee player sets it to load coefficients and clears it
 * before every frame start; the name is Allwinner's, the meaning is not. */
#define DEFE_EN_BIST      (1u << 31)

#define DEFE_FRM_REG_RDY  (1u << 0)
#define DEFE_FRM_COEF_RDY (1u << 1)
#define DEFE_FRM_WB_EN    (1u << 2)
#define DEFE_FRM_OUT_CTRL (1u << 11) /* do not drive the backend */
#define DEFE_FRM_OUT_PORT(x) (((x) & 3u) << 8) /* which sink consumes us */
#define DEFE_FRM_START    (1u << 16)
#define DEFE_FRM_COEF_ACC (1u << 23)

#define DEFE_BYPASS_SCALER (1u << 0) /* pass pixels through unfiltered */
#define DEFE_BYPASS_CSC    (1u << 1)

/* INPUT_FMT: data_mod [10:8], data_fmt [6:4], pixel sequence [1:0] */
#define DEFE_IN_MOD_PLANAR     (0u << 8)
#define DEFE_IN_MOD_INTERLEAVE (1u << 8)
#define DEFE_IN_MOD_UVCOMBINED (2u << 8) /* semi-planar: NV12 / NV16 */
#define DEFE_IN_FMT_YUV444     (0u << 4)
#define DEFE_IN_FMT_YUV422     (1u << 4)
#define DEFE_IN_FMT_YUV420     (2u << 4)
#define DEFE_IN_PS_UV          0u /* U first  - NV12 */
#define DEFE_IN_PS_VU          1u /* V first  - NV21 */
#define DEFE_IN_NV12 (DEFE_IN_MOD_UVCOMBINED | DEFE_IN_FMT_YUV420 | DEFE_IN_PS_UV)

/* OUTPUT_FMT data_fmt [2:0]. Allwinner's enum names 0,1,2 (RGB) and 4,5,6,7
 * (planar YUV 444/420/422/411) and leaves 3 unnamed - the one value in the
 * field with no documented meaning, and the only remaining candidate for a
 * UV-combined write-back. Every named YUV value is three-plane, which is
 * what makes the frontend unable to feed the VE. */
#define DEFE_OUT_PRGB888  0u
#define DEFE_OUT_IRGB888  1u
#define DEFE_OUT_I1RGB888 2u
#define DEFE_OUT_UNNAMED3 3u
#define DEFE_OUT_PYUV444  4u
#define DEFE_OUT_PYUV420  5u
#define DEFE_OUT_PYUV422  6u
#define DEFE_OUT_PYUV411  7u

#define DEFE_STATUS_WB_BUSY (1u << 1)
#define DEFE_INT_WB_DONE    (1u << 7)

typedef struct defe_cfg {
    uint32_t src_y, src_c;     /* NV12 input planes                    */
    uint16_t sw, sh;           /* input size in luma samples           */
    uint32_t src_strd_y;       /* bytes per luma row; 0 means sw       */
    uint32_t dst0, dst1, dst2; /* write-back planes                    */
    uint32_t dst_strd0, dst_strd1, dst_strd2;
    uint16_t dw, dh;    /* output size in luma samples          */
    uint8_t out_fmt;    /* OUTPUT_FMT data_fmt [2:0]            */
    uint8_t bypass;     /* 1: skip the FIR entirely (1:1 only)  */
    uint8_t out_port;   /* FRM_CTRL out_port_sel [9:8]          */
    uint8_t out_ctrl;   /* 1: set OUT_CTRL (keep off the backend) */
    uint32_t in_fmt;    /* INPUT_FMT as a whole; 0 means DEFE_IN_NV12 */
} defe_cfg_t;

void defe_init(void);
/* Full gate/reset cycle plus coefficient reload. A pass that stalls leaves
 * the write-back engine latched busy, and nothing short of this clears it -
 * so every probe pass starts from here rather than inheriting the wreckage
 * of the one before. */
void defe_reset(void);
void defe_start(const defe_cfg_t* c);

/* 0 on completion, -1 on timeout. Fills *status and *intst for diagnosis;
 * either may be NULL. */
int defe_wait(uint32_t timeout_us, uint32_t* status, uint32_t* intst);

uint32_t defe_r(uint32_t off);
void defe_w(uint32_t off, uint32_t val);

/* First word of the horizontal coefficient bank, read back through the same
 * access gating the loader uses. Should be 0x40000000 - phase 0, a single
 * unit tap. Zero means the RAM is not taking writes. */
uint32_t defe_coef_readback(void);
