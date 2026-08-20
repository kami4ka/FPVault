/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * enctest.h - the M1 bring-up harness: encode a test pattern with the VE
 * and dump the finished JPEG over the console as base64. This is the
 * go/no-go experiment for the whole product, and it stays in the firmware
 * afterwards as the encoder self-test.
 */
#pragma once

void enctest_init(void);          /* ve_init + pattern + banner */
void enctest_encode(int dump);    /* 'J' encode only / 'j' encode + dump  */
void enctest_cycle_quality(void); /* 'q': 50 -> 75 -> 90 -> 50 */
void enctest_cycle_fmt(void);     /* 'm': NV12(0) -> NV16(1) -> NV16(2) */
void enctest_info(void);          /* 'v': VE version + status registers */
