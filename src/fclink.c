/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * fclink.c - the flight-controller link: RunCam Device Protocol on UART1
 * (PA2/PA3, 115200 8N1). Betaflight/INAV send the power-button action as a
 * record TOGGLE; ArduPilot sends explicit start/stop (see src/runcam.h and
 * docs/PROTOCOL.md). RX is polled from the main loop - at 115200 the FIFO
 * gives ~1.4 ms per byte of slack and the loop spins far faster.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "fclink.h"
#include "runcam.h"
#include "recorder.h"
#include "io.h"
#include "f1c100s_periph.h"
#include "f1c100s_uart.h"
#include "f1c100s_gpio.h"
#include "f1c100s_clock.h"
#include "f1c100s_timer.h"

static rcam_t rc;
static uint32_t t_last_rx;
static uint32_t evt_toggle = 0, evt_start = 0, evt_stop = 0;

static void fc_tx(void* ctx, const uint8_t* d, uint8_t n) {
    uint8_t i;
    (void)ctx;
    for(i = 0; i < n; i++) {
        while(!(uart_get_status(UART1_BASE) & UART_LSR_THRE))
            ;
        uart_tx(UART1_BASE, d[i]);
    }
}

void fclink_init(void) {
    gpio_init(GPIOA, PIN2 | PIN3, GPIO_MODE_AF5, GPIO_PULL_UP, GPIO_DRV_1);
    clk_enable(CCU_BUS_CLK_GATE2, 21); /* uart1 */
    clk_reset_clear(CCU_BUS_SOFT_RST2, 21);
    uart_init(UART1_BASE, 115200);
    rcam_init(&rc, fc_tx, 0);
    t_last_rx = tim_get_cnt(TIM0);
}

void fclink_poll(void) {
    while(read32(UART1_BASE + UART_LSR) & UART_LSR_DR) {
        uint8_t b = (uint8_t)(read32(UART1_BASE + UART_RBR) & 0xFF);
        rcam_event_e e = rcam_feed(&rc, b);
        t_last_rx = tim_get_cnt(TIM0);
        switch(e) {
        case RCAM_EVT_TOGGLE:
            evt_toggle++;
            recorder_toggle();
            break;
        case RCAM_EVT_START:
            evt_start++;
            if(!recorder_active()) recorder_toggle();
            break;
        case RCAM_EVT_STOP:
            evt_stop++;
            if(recorder_active()) recorder_toggle();
            break;
        default: break;
        }
    }
    /* abandon a half-received frame after a >20 ms gap (line noise) */
    if(rc.n && (uint32_t)(t_last_rx - tim_get_cnt(TIM0)) / 24u > 20000u)
        rcam_idle(&rc);
}

void fclink_stats(void) {
    if(rc.rx_frames || rc.crc_errors)
        printf("[fc] frames %lu, crc errs %lu, resyncs %lu, "
               "toggle/start/stop %lu/%lu/%lu\r\n",
               (unsigned long)rc.rx_frames, (unsigned long)rc.crc_errors,
               (unsigned long)rc.resyncs, (unsigned long)evt_toggle,
               (unsigned long)evt_start, (unsigned long)evt_stop);
}
