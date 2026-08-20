/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "testpat.h"

/* 100% bars, BT.601 studio range. Order: white yellow cyan green magenta
 * red blue black. */
static const uint8_t bar_y[8] = {235, 210, 170, 145, 106, 81, 41, 16};
static const uint8_t bar_u[8] = {128, 16, 166, 54, 202, 90, 240, 128};
static const uint8_t bar_v[8] = {128, 146, 16, 34, 222, 240, 110, 128};

static void fill(uint8_t* y, uint8_t* c, uint16_t w, uint16_t h, int c_rows) {
    uint16_t row, col;
    uint16_t bars_h = (uint16_t)(h - h / 4);
    for(row = 0; row < h; row++) {
        uint8_t* py = y + (uint32_t)row * w;
        if(row < bars_h) {
            for(col = 0; col < w; col++)
                py[col] = bar_y[col * 8 / w];
        } else {
            for(col = 0; col < w; col++)
                py[col] = (uint8_t)(16 + col * 220 / (w - 1));
        }
    }
    for(row = 0; row < c_rows; row++) {
        uint8_t* pc = c + (uint32_t)row * w;
        /* chroma row index in luma space (NV12 rows cover 2 luma rows) */
        uint16_t lrow = (uint16_t)((uint32_t)row * h / c_rows);
        for(col = 0; col < w; col += 2) {
            if(lrow < bars_h) {
                int b = col * 8 / w;
                pc[col] = bar_u[b];
                pc[col + 1] = bar_v[b];
            } else {
                pc[col] = 128;
                pc[col + 1] = 128;
            }
        }
    }
}

void testpat_bars_nv12(uint8_t* y, uint8_t* c, uint16_t w, uint16_t h) {
    fill(y, c, w, h, h / 2);
}

void testpat_bars_nv16(uint8_t* y, uint8_t* c, uint16_t w, uint16_t h) {
    fill(y, c, w, h, h);
}
