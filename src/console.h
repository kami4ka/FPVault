/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Single-character UART0 console, no newline needed. Poll from the main
 * loop; returns immediately when no byte is waiting. */
void console_poll(void);
