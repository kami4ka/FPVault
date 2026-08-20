/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * testpat.h - deterministic NV12/NV16 test frames, generated directly in
 * the encoder's input format. The predecessor project proved the camera is
 * an unreliable measurement input (dropouts, lock loss), so every encoder
 * benchmark and bring-up step runs against these instead.
 *
 * The pattern: 8 full-saturation BT.601 color bars over the top 3/4
 * (unambiguous chroma-order diagnosis: a U/V swap turns red magenta-ish
 * and blue green-ish), a horizontal luma ramp across the bottom 1/4
 * (quality/banding eyeball check).
 */
#pragma once

#include <stdint.h>

/* NV12: c plane is h/2 rows of interleaved UV. */
void testpat_bars_nv12(uint8_t* y, uint8_t* c, uint16_t w, uint16_t h);

/* NV16: c plane is h rows of interleaved UV. */
void testpat_bars_nv16(uint8_t* y, uint8_t* c, uint16_t w, uint16_t h);
