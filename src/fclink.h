/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * fclink.h - RunCam Device Protocol link to the flight controller (UART1).
 */
#pragma once

void fclink_init(void);
void fclink_poll(void);  /* main loop: drain RX, dispatch record commands */
void fclink_stats(void); /* 1 Hz line (silent until traffic seen) */
