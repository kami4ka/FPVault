/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_jpegtab.c - structural checks on the precomputed JPEG tables. A typo
 * in a Huffman table produces undecodable files, so the invariants the
 * decoder relies on are asserted here: marker framing, segment lengths,
 * BITS/VALS consistency, quality-scaling monotonicity, zigzag round trip.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jpegtab.h"

static const uint8_t* expect_marker(const uint8_t* p, uint8_t code, int* seg_len) {
    assert(p[0] == 0xFF);
    assert(p[1] == code);
    *seg_len = (p[2] << 8) | p[3];
    return p + 4;
}

static const uint8_t* check_dht(const uint8_t* p, uint8_t tcth, int expect_vals) {
    int len, i, nvals = 0;
    p = expect_marker(p, 0xC4, &len);
    assert(p[0] == tcth);
    for(i = 1; i <= 16; i++)
        nvals += p[i];
    assert(nvals == expect_vals);
    assert(len == 2 + 1 + 16 + nvals);
    return p + len - 2;
}

int main(void) {
    uint16_t qY[64], qC[64], qY2[64], qC2[64];
    uint8_t buf[JPEGTAB_PREFIX_MAX];
    uint32_t n;
    const uint8_t* p;
    int len, i;

    /* quality scaling: q=50 must reproduce Annex K exactly (scale=100) */
    jpegtab_quant(50, qY, qC);
    assert(qY[0] == 16 && qY[1] == 11 && qY[63] == 99);
    assert(qC[0] == 17 && qC[63] == 99);
    /* higher quality -> smaller or equal steps, never below 1 */
    jpegtab_quant(90, qY2, qC2);
    for(i = 0; i < 64; i++) {
        assert(qY2[i] <= qY[i] && qY2[i] >= 1);
        assert(qC2[i] <= qC[i] && qC2[i] >= 1);
    }
    /* q=100 -> all ones for a table whose base*scale rounds below 1 */
    jpegtab_quant(100, qY2, qC2);
    for(i = 0; i < 64; i++)
        assert(qY2[i] >= 1 && qY2[i] <= 16);

    /* prefix structure */
    jpegtab_quant(75, qY, qC);
    n = jpegtab_prefix(buf, qY, qC);
    assert(n == 572);
    assert(n <= JPEGTAB_PREFIX_MAX);
    assert(buf[0] == 0xFF && buf[1] == 0xD8); /* SOI */

    p = buf + 2;
    p = expect_marker(p, 0xDB, &len);
    assert(len == 67 && p[0] == 0x00);
    /* DQT is zigzag: wire[0]=natural[0], wire[1]=natural[1], wire[2]=natural[8] */
    assert(p[1] == (uint8_t)qY[0] && p[2] == (uint8_t)qY[1] && p[3] == (uint8_t)qY[8]);
    p += len - 2;
    p = expect_marker(p, 0xDB, &len);
    assert(len == 67 && p[0] == 0x01);
    assert(p[1] == (uint8_t)qC[0]);
    p += len - 2;

    p = check_dht(p, 0x00, 12);  /* DC luma  */
    p = check_dht(p, 0x10, 162); /* AC luma  */
    p = check_dht(p, 0x01, 12);  /* DC chroma */
    p = check_dht(p, 0x11, 162); /* AC chroma */
    assert((uint32_t)(p - buf) == n);

    printf("test_jpegtab: OK (prefix %u bytes)\n", n);
    return 0;
}
