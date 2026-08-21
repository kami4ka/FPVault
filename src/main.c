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
#include "capture.h"
#include "recorder.h"
#include "pipeline.h"
#include "fclink.h"
#include "usbmsc.h"
#include "arm32.h"
#include "f1c100s_gpio.h"
#include "f1c100s_intc.h"
#include "f1c100s_timer.h"

#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

static uint32_t uptime_s = 0;

uint32_t sys_uptime_s(void) { return uptime_s; }

/* 1 kHz pipeline heartbeat: capture ring + VE encode live here, so a
 * blocking SD write in the main loop can never cost a captured frame. */
static void tick_irq(void) {
    tim_clear_irq(TIM1);
    pipeline_tick();
}

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

    /* TIM1 @ 1 kHz drives the capture/encode pipeline from IRQ context. */
    tim_init(TIM1, TIM_MODE_CONT, TIM_SRC_HOSC, TIM_PSC_1);
    tim_set_period(TIM1, 24000); /* 24 MHz / 24000 = 1 kHz */
    intc_set_irq_handler(IRQ_TIMER1, tick_irq);
    intc_enable_irq(IRQ_TIMER1);
    tim_int_enable(TIM1);
    tim_start(TIM1);

    /* DVR product behavior: capture+encode from boot, recorder state
     * machine arms itself (mount retry -> wait for signal -> record),
     * flight-controller link listening. The console can override all of
     * it (:R manual toggle, :A auto on/off). */
    pipeline_toggle();
    fclink_init();
    usbmsc_init();

    /* Mode fork: USB is this board's power source, so a host can only be
     * present from power-on. Give enumeration a short window; if a host
     * configures us, the recorder yields before ever mounting the card
     * and the board is a pure card reader until reboot. In the air no
     * host exists, the window expires, and recording starts ~2 s later
     * than it otherwise would - the only cost of the feature. */
    {
        uint32_t t0 = tim_get_cnt(TIM0);
        while((uint32_t)(t0 - tim_get_cnt(TIM0)) < TICKS_PER_SEC * 5u / 2u) {
            wdg_feed();
            if(usbmsc_host_present()) break;
        }
    }

    {
        uint32_t t_sec = tim_get_cnt(TIM0);
        int led = 0;

        uint32_t t_led = t_sec;
        while(1) {
            wdg_feed();
            console_poll();
            fclink_poll();
            recorder_task();

            /* Drain encoded frames to the recorder (may block on SD -
             * the IRQ pipeline keeps capturing regardless). */
            pipeline_consume();

            /* LED: 8-bit state pattern, one bit per 125 ms. */
            if((uint32_t)(t_led - tim_get_cnt(TIM0)) >= TICKS_PER_SEC / 8u) {
                t_led -= TICKS_PER_SEC / 8u;
                led = (led + 1) & 7;
                if((recorder_led_pattern() >> (7 - led)) & 1)
                    gpio_pin_set(LED_PORT, LED_PIN);
                else
                    gpio_pin_clear(LED_PORT, LED_PIN);
            }

            /* 1 Hz housekeeping. */
            if((uint32_t)(t_sec - tim_get_cnt(TIM0)) >= TICKS_PER_SEC) {
                t_sec -= TICKS_PER_SEC;
                uptime_s++;
                pipeline_stats();
                recorder_stats();
                fclink_stats();
                usbmsc_stats();
            }
        }
    }
    return 0;
}
