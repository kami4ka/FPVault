#include <stdio.h>
#include <stdint.h>
#include "arm32.h"
#include "board.h"
#include "f1c100s_timer.h"

/* Crash breadcrumb: plain uncached-safe stores into DRAM that the next boot
 * reads back (see board.h). Written AFTER the watchdog is armed and BEFORE
 * printf - the register dump goes through a UART that may itself be wedged
 * and has been observed to come out as garbage, but these four stores
 * cannot fail. */
static void leave_breadcrumb(uint32_t type, uint32_t pc, uint32_t lr) {
    volatile uint32_t* bc = (volatile uint32_t*)BREADCRUMB_BASE;
    bc[1] = pc;
    bc[2] = lr;
    bc[3] = type;
    bc[0] = BC_CRASH_MAGIC;
}

/* Fault handlers used to spin forever. A single unaligned 32-bit store during
 * development then left the board hard-hung: the console command loop never
 * runs again, so even the reset command is dead and only a physical power
 * cycle recovers it.
 *
 * The watchdog is armed FIRST, before any printing. Dumping registers is
 * useful but printf writes to a UART that may itself be wedged, and a handler
 * that hangs before arming the watchdog is no better than one that spins -
 * which is exactly what happened on the second crash. */
static void arm_watchdog(void) {
    wdg_init(WDG_MODE_RESET, WDG_INTV_500MS);
}

static void die(void) {
    while(1)
        ;
}

struct arm_regs_t {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
};

static void show_regs(struct arm_regs_t* regs) {
    int i;

    printf("pc : [<%08lx>] lr : [<%08lx>] cpsr: %08lx\r\n", regs->pc, regs->lr, regs->cpsr);
    printf("sp : %08lx\r\n", regs->sp);
    for(i = 12; i >= 0; i--) {
        printf("r%-2d: %08lx ", i, regs->r[i]);
        if(i % 2 == 0) printf("\r\n");
    }
    printf("\r\n");
}

void _undefined_instruction_(struct arm_regs_t* regs) {
    arm_watchdog();
    leave_breadcrumb(1, regs->pc, regs->lr);
    printf("\r\n!! UNDEFINED_INSTRUCTION !!\r\n");
    show_regs(regs);
    die();
}

void _software_interrupt_(struct arm_regs_t* regs) {
    printf("\r\n!! SOFT_INTERRUPT !!\r\n");
    //show_regs(regs);
    //while(1);
}

void _prefetch_abort_(struct arm_regs_t* regs) {
    arm_watchdog();
    leave_breadcrumb(2, regs->pc, regs->lr);
    printf("\r\n!! PREFETCH_ABORT !!\r\n");
    show_regs(regs);
    die();
}

void _data_abort_(struct arm_regs_t* regs) {
    arm_watchdog();
    leave_breadcrumb(3, regs->pc, regs->lr);
    printf("\r\n!! DATA_ABORT !!\r\n");
    show_regs(regs);
    die();
}
