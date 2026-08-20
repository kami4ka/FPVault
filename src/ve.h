/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ve.h - Cedar Video Engine core: clocks, reset, SRAM mapping, registers.
 *
 * Register names and offsets come from the linux-sunxi reverse-engineering
 * effort via jepoc (LGPL-2.1+, (c) 2014 Manuel Braga; ve.h (c) 2013 Jens
 * Kuske). The JPEG encoder is the AVC sub-engine (select 0xB) driving the
 * ISP input stage - there is no H.264 *encode* on the F1C200s, but the JPEG
 * mode of this block is documented in the datasheet as "MJPEG encode up to
 * 1280x720@30fps".
 *
 * Bring-up sequence is the suniv branch of Allwinner's own cedar_ve driver
 * (mirkerson/c600, CONFIG_ARCH_SUNIVW1P1): PLL_VE at 300 MHz, VE module
 * clock gate, AHB gate, soft reset cycle, the VE's dedicated DRAM port
 * gate, and mapping SRAM C to the VE.
 */
#pragma once

#include <stdint.h>
#include "io.h"
#include "f1c100s_periph.h"

/* All offsets relative to VE_BASE (0x01C0E000). */
#define VE_CTRL            0x000
#define VE_RESET           0x004
#define VE_VERSION         0x0f0

#define VE_ISP_PIC_SIZE    0xa00
#define VE_ISP_PIC_STRIDE  0xa04
#define VE_ISP_CTRL        0xa08
#define VE_ISP_PIC_LUMA    0xa78
#define VE_ISP_PIC_CHROMA  0xa7c

#define VE_AVC_PARAM       0xb04
#define VE_AVC_QP          0xb08
#define VE_AVC_MOTION_EST  0xb10
#define VE_AVC_CTRL        0xb14
#define VE_AVC_TRIGGER     0xb18
#define VE_AVC_STATUS      0xb1c
#define VE_AVC_BASIC_BITS  0xb20
#define VE_AVC_VLE_ADDR    0xb80
#define VE_AVC_VLE_END     0xb84
#define VE_AVC_VLE_OFFSET  0xb88
#define VE_AVC_VLE_MAX     0xb8c
#define VE_AVC_VLE_LENGTH  0xb90
#define VE_AVC_SDRAM_INDEX 0xbe0
#define VE_AVC_SDRAM_DATA  0xbe4

static inline void ve_w(uint32_t off, uint32_t val) { write32(VE_BASE + off, val); }
static inline uint32_t ve_r(uint32_t off) { return read32(VE_BASE + off); }

/* Clocks + reset + SRAM C map. Safe to call once at boot. */
void ve_init(void);

/* Contents of VE_VERSION after init (the ID lives in the top half). */
uint32_t ve_version(void);

/* Route the shared sub-engine window (0xa00/0xb00) to the AVC encoder.
 * 0xB is the pre-H3 select convention; F1C is the older IP generation.
 * If M1 bring-up disproves this, this is THE register to experiment on. */
void ve_select_avc(void);
