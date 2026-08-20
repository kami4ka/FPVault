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
#include "enctest.h"
#include "sdtest.h"
#include "capture.h"
#include "recorder.h"
#include "pipeline.h"

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
    case 'j':
        if(pipeline_active())
            enctest_dump_pipeline();
        else
            enctest_encode(1);
        break;
    case 'J': enctest_encode(0); break;
    case 'q': enctest_cycle_quality(); break;
    case 'm': enctest_cycle_fmt(); break;
    case 'y': enctest_toggle_samp(); break;
    case 'u': enctest_toggle_uniform(); break;
    case 'b': enctest_toggle_hdr(); break;
    case 'w': enctest_wipe_slot(); break;
    case 'd': enctest_scan_slot(); break;
    case 'M': sdtest_mount(); break;
    case 'Z': sdtest_scrub(); break;
    case 'B': sdtest_benchmark(); break;
    case 'W': sdtest_toggle_width(); break;
    case 'c': pipeline_toggle(); break;
    case 'f': pipeline_fmt_toggle(); break;
    case 'p': enctest_rawdump(); break;
    case 'x': enctest_copy_encode(); break;
    case 'X':
        capture_stop();
        printf("[cap] TVD DMA disabled\r\n");
        break;
    case 'R': recorder_toggle(); break;
    case 'A': recorder_toggle_auto(); break;
    case 'S': {
        /* M7 fault injection: stall the SD consumer for 500 ms. The IRQ
         * pipeline must keep capturing and the ring must absorb it with
         * zero drops (watch the next stats line's ring hi-water). */
        uint32_t t0 = tim_get_cnt(TIM0);
        printf("[test] stalling main loop 500 ms\r\n");
        while((uint32_t)(t0 - tim_get_cnt(TIM0)) < TICKS_PER_SEC / 2u)
            wdg_feed();
        break;
    }
    case '8':
        capture_force_colormode(0);
        printf("[cap] forced NTSC color\r\n");
        break;
    case '9':
        capture_force_colormode(1);
        printf("[cap] forced PAL-M color\r\n");
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': enctest_set_fmt((uint8_t)(c - '0')); break;
    case 'v': enctest_info(); break;
    default:
        printf("? s state, r reset | VE: j enc+dump, J enc, q quality, m fmt, v info\r\n");
        break;
    }
}

/* Commands are two bytes: ':' then the command character. The in-line
 * ESP32 USB-serial bridge reboots every time the host opens the port, and
 * its TX line glitches random bytes into this console as it does - bare
 * single-character commands got executed by line noise (a phantom 'x'
 * overwrote the test pattern, a phantom '5' reprogrammed the ISP format,
 * and hours went into chasing the resulting ghosts). A ':' prefix makes
 * phantom commands improbable; anything unarmed is dropped silently. */
void console_poll(void) {
    static uint8_t armed = 0;
    char c;
    if(!(read32(UART0 + UART_LSR) & UART_LSR_DR)) return;
    c = (char)(read32(UART0 + UART_RBR) & 0xFF);
    if(armed) {
        armed = 0;
        dispatch(c);
    } else if(c == ':') {
        armed = 1;
    }
}
