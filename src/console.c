/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * console.c - single-character UART0 diagnostics.
 *
 * The console is the project's only debugging surface once the display path
 * is gone (record-only tap), so every subsystem adds its own commands here
 * as it lands. Convention inherited from the predecessor project: single
 * characters, no newline, `s` is always the state page, `r` always resets.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "console.h"
#include "io.h"
#include "f1c100s_periph.h"
#include "f1c100s_uart.h"
#include "f1c100s_timer.h"

extern uint32_t sys_uptime_s(void);

static void cmd_state(void) {
    printf("[state] uptime %lus  tim0 %08lx\r\n",
           (unsigned long)sys_uptime_s(), (unsigned long)tim_get_cnt(TIM0));
    printf("  capture %08x  bsring %08x x%lu slots  idx %08x\r\n",
           (unsigned)CAPTURE_BASE, (unsigned)BSRING_BASE,
           (unsigned long)BSRING_SLOTS, (unsigned)IDX_BASE);
}

static void cmd_reset(void) {
    printf("resetting...\r\n");
    wdg_init(WDG_MODE_RESET, WDG_INTV_500MS);
    while(1)
        ;
}

static void dispatch(char c) {
    switch(c) {
    case 's': cmd_state(); break;
    case 'r': cmd_reset(); break;
    default:
        printf("? s state, r reset\r\n");
        break;
    }
}

void console_poll(void) {
    if(read32(UART0 + UART_LSR) & UART_LSR_DR)
        dispatch((char)(read32(UART0 + UART_RBR) & 0xFF));
}
