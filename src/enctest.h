/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * enctest.h - the M1 bring-up harness: encode a test pattern with the VE
 * and dump the finished JPEG over the console as base64. This is the
 * go/no-go experiment for the whole product, and it stays in the firmware
 * afterwards as the encoder self-test.
 */
#pragma once

#include <stdint.h>

void enctest_init(void);          /* ve_init + pattern + banner */
void enctest_encode(int dump);    /* 'J' encode only / 'j' encode + dump  */
void enctest_cycle_quality(void); /* 'q': 50 -> 75 -> 90 -> 50 */
void enctest_cycle_fmt(void);     /* 'm': NV12(0) -> NV16(1) -> NV16(2) */
void enctest_set_fmt(uint8_t v);  /* '0'..'7': raw ISP format field value */
void enctest_toggle_samp(void);   /* 'y': SOF0 luma sampling 2x2 <-> 2x1 */
void enctest_toggle_uniform(void);/* 'u': bars <-> solid-color frame */
void enctest_toggle_hdr(void);    /* 'b': skip the SOF0/SOS bit push */
void enctest_wipe_slot(void);     /* 'w': fill slot 0 with 0xEE sentinel */
void enctest_scan_slot(void);     /* 'd': report the written extent */

/* M3 live capture -> encode */
void enctest_live_toggle(void); /* 'c': start/stop live capture+encode */
void enctest_live_tvdfmt(void); /* 'f': TVD 4:2:0 <-> 4:2:2 (+ISP pairing) */
void enctest_live_tick(void);   /* main loop: poll capture, encode frames */
void enctest_rawdump(void);     /* 'p': decimated raw capture planes */
void enctest_copy_encode(void); /* 'x': CPU-copy capture frame, encode via testpat path */
void enctest_live_stats(void);  /* 1 Hz stats line */
int enctest_live_active(void);
void enctest_info(void);          /* 'v': VE version + status registers */
