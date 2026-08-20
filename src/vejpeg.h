/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * vejpeg.h - one-frame hardware JPEG encode session on the Cedar VE.
 *
 * Ported from jepoc (LGPL-2.1+, (c) 2014 Manuel Braga): sub-engine 0xB in
 * JPEG mode, ISP input stage fed with semi-planar YUV, VLE bitstream output
 * to a physical buffer. The CPU's total work per frame is ~40 register
 * writes; the pixels never pass through it.
 *
 * ISP input format field (VE_ISP_CTRL[31:28]) - CAUTION: the two reference
 * codebases disagree. jepoc says NV12=0/NV16=2/TILE=4; sunxi-tvin2jpeg says
 * NV12=0/NV16=1/TILE=2. NV12=0 is agreed and is what both actually shipped
 * with, so NV12 is our primary path (TVD_FMT_420_PL) and the NV16 values
 * are silicon experiments, selectable at runtime.
 *
 * Stream anatomy: the CPU-built file prefix (SOI+DQT+DHT, jpegtab_prefix())
 * is NOT part of the hardware stream. SOF0 and SOS are pushed bit-by-bit
 * into the VLE buffer before launch; the hardware appends the entropy-coded
 * scan. File = prefix + VLE bytes + EOI.
 */
#pragma once

#include <stdint.h>

typedef struct vejpeg_cfg {
    uint16_t w, h;
    uint8_t isp_fmt;   /* VE_ISP_CTRL[31:28]: 0=NV12; 1 or 2 = NV16 (experiment) */
    uint8_t samp_2x2;  /* SOF0 luma sampling: 1 = 2x2 (4:2:0), 0 = 2x1 (4:2:2) */
    uint8_t quality;   /* 1..100, must match the prefix the file gets */
} vejpeg_cfg_t;

/* Program the whole encode and trigger it. Non-blocking. phy_out gets the
 * SOF0+SOS+scan bitstream; out_size bounds it in hardware (VLE_END). */
void vejpeg_start(const vejpeg_cfg_t* cfg, uint32_t phy_y, uint32_t phy_c,
                  uint32_t phy_out, uint32_t out_size);

/* Poll for completion. Returns bitstream length in bytes, or
 * VEJPEG_ERR_TIMEOUT / VEJPEG_ERR_FAILED. Clears the status either way. */
#define VEJPEG_ERR_TIMEOUT (-1)
#define VEJPEG_ERR_FAILED  (-2)
int32_t vejpeg_wait(uint32_t timeout_us);

/* Raw AVC status nibble, for diagnostics (0 none, 1 done, 2 failed). */
uint32_t vejpeg_status(void);
