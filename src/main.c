/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * f1c200s-dvr - open-source airborne FPV DVR on the Allwinner F1C200s.
 *
 *   CVBS camera -> TVD (its own DMA) -> DDR NV16 ring
 *                -> Cedar VE hardware JPEG encode -> DDR bitstream ring
 *                -> SD card (4-bit + IDMAC) as MJPEG-in-AVI, DCF-named clips
 *
 * The CPU never touches a pixel; it orchestrates DMA masters and writes AVI
 * bookkeeping. Record-only tap: there is no video output path in this
 * firmware. Loaded from U-Boot:  loady 0x80000000  ->  go 0x80000000
 *
 * Milestone status: M0 - skeleton, console, LED, watchdog.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "system.h"
#include "console.h"
#include "enctest.h"
#include "arm32.h"
#include "f1c100s_gpio.h"
#include "f1c100s_timer.h"

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

static uint32_t uptime_s = 0;

uint32_t sys_uptime_s(void) { return uptime_s; }

int main(void) {
    system_init();
    arm32_interrupt_enable();

    printf("\r\n[%s] up. build %s %s (%s). s state, r reset\r\n",
           BOARD_NAME, __DATE__, __TIME__, GIT_REV);

    gpio_init(LED_PORT, LED_PIN, GPIO_MODE_OUTPUT, GPIO_PULL_NONE, GPIO_DRV_3);
    wdg_init(WDG_MODE_RESET, WDG_INTV_6S);

    /* TIM0 free-runs at 24 MHz as the global timestamp source. It counts
     * DOWN, so an elapsed interval is (earlier_reading - current_reading). */
    tim_init(TIM0, TIM_MODE_CONT, TIM_SRC_HOSC, TIM_PSC_1);
    tim_set_period(TIM0, 0xFFFFFFFF);
    tim_start(TIM0);

    /* M1: Cedar VE bring-up + test-pattern encoder ('j' on the console). */
    enctest_init();

    {
        uint32_t t_sec = tim_get_cnt(TIM0);
        int led = 0;

        while(1) {
            wdg_feed();
            console_poll();

            /* 1 Hz housekeeping: LED heartbeat + uptime. */
            if((uint32_t)(t_sec - tim_get_cnt(TIM0)) >= TICKS_PER_SEC) {
                t_sec -= TICKS_PER_SEC;
                uptime_s++;
                led ^= 1;
                if(led)
                    gpio_pin_set(LED_PORT, LED_PIN);
                else
                    gpio_pin_clear(LED_PORT, LED_PIN);
            }
        }
    }
    return 0;
}
