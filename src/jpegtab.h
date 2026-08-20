/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * jpegtab.h - JPEG quantization + Huffman tables, precomputed.
 *
 * jepoc used libjpeg at runtime purely to generate the table markers and
 * scale the quantization values; the pixel data never touches software.
 * Here the same artifacts are computed from the ITU-T T.81 Annex K tables
 * directly, which removes the libjpeg dependency entirely.
 *
 * Ordering rules (the classic bug source):
 *   - the VE's SDRAM quantizer upload wants the tables in NATURAL (row)
 *     order - jepoc passed libjpeg's quantval[] straight through, and
 *     libjpeg stores natural order;
 *   - the DQT marker in the file must be in ZIGZAG order (T.81 B.2.4.1).
 */
#pragma once

#include <stdint.h>

/* Quality-scaled quantization tables, NATURAL order, values 1..255.
 * Scaling is the IJG formula (quality 1..100). */
void jpegtab_quant(int quality, uint16_t qY[64], uint16_t qC[64]);

/* Emit the JFIF stream prefix into dst: SOI + DQT(Y) + DQT(C) + the four
 * standard DHT segments. Returns the byte count (fixed:
 * 2 + 2*69 + 33 + 183 + 33 + 183 = 572). The hardware bitstream (SOF0, SOS,
 * entropy data) follows this, and EOI closes the file. */
uint32_t jpegtab_prefix(uint8_t* dst, const uint16_t qY[64], const uint16_t qC[64]);

/* Emit the COMPLETE header set: prefix + SOF0 + SOS (605 bytes). On VE
 * 1663 the basic-bits port routes pushed bytes through the JPEG byte
 * stuffer (FF C0 arrives in the stream as FF 00 C0 - a corrupted marker),
 * so in-band header pushing is impossible and every header is CPU-side.
 * The hardware stream is then the pure entropy scan. */
uint32_t jpegtab_headers(uint8_t* dst, const uint16_t qY[64], const uint16_t qC[64],
                         uint16_t w, uint16_t h, int samp_2x2);

#define JPEGTAB_PREFIX_MAX 576
#define JPEGTAB_HDR_LEN 605u /* prefix 572 + SOF0 19 + SOS 14 */
