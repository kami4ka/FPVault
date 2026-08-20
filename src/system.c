/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * system.c - clocks, MMU, caches, console UART.
 *
 * Derived from the f1c200-video-board experiments (same author, same board).
 * Differences from that project's bring-up:
 *
 *   - PLL_VIDEO stays even though this firmware drives no display: the TVD
 *     derives its 27 MHz decoder clock from PLL_VIDEO/11 = 297/11 MHz
 *     (f1c100s_tvd.c). The 24 MHz oscillator is not a 27 MHz multiple, so
 *     the PLL is not optional.
 *   - No TCON/TVE/DEBE/DEFE clocks or resets: record-only tap, no video out.
 *   - The capture planes and the JPEG bitstream ring are mapped NON-cacheable
 *     (see board.h for why), everything else in DRAM stays cacheable.
 */
#include "system.h"
#include <stdint.h>
#include "board.h"
#include "armv5_mmu.h"
#include "arm32.h"
#include "sizes.h"
#include "f1c100s_clock.h"
#include "f1c100s_intc.h"
#include "f1c100s_gpio.h"
#include "f1c100s_uart.h"
#include "io.h"

static void sys_takeover_from_uboot(void);
static void sys_clk_init(void);
static void sys_uart_init(void);
static void sys_mmu_cache_init(void);

static inline void sdelay(int loops);

// MMU translation table
uint32_t mmu_l1_tbl[4096] __attribute__((section(".mmu_tbl")));

void system_init(void) {
    sys_takeover_from_uboot();
    sys_clk_init();
    sys_uart_init();
    sys_mmu_cache_init();
    intc_init();
}

/* U-Boot hands over with its own MMU and caches live. Clean and drain the
 * D-cache, then turn the lot off, so our own tables start from a known state. */
static void sys_takeover_from_uboot(void) {
    __asm__ volatile(
        "1: mrc p15, 0, r15, c7, c14, 3\n"
        "   bne 1b\n"
        "   mov r0, #0\n"
        "   mcr p15, 0, r0, c7, c10, 4\n"
        "   mcr p15, 0, r0, c7, c5, 0\n"
        "   mrc p15, 0, r0, c1, c0, 0\n"
        "   bic r0, r0, #0x1000\n"
        "   bic r0, r0, #0x0004\n"
        "   bic r0, r0, #0x0001\n"
        "   mcr p15, 0, r0, c1, c0, 0\n"
        "   mcr p15, 0, r0, c8, c7, 0\n"
        ::: "r0", "cc", "memory");
}

static void sys_clk_init(void) {
    // Run from the 24 MHz oscillator while the PLLs are reprogrammed
    clk_cpu_config(CLK_CPU_SRC_OSC24M);
    sdelay(10);

    clk_pll_init(PLL_PERIPH, 25, 1); // PLL_PERIPH = 24M*25/1 = 600M
    clk_pll_enable(PLL_PERIPH);
    while(!clk_pll_is_locked(PLL_PERIPH))
        ;

    // Bus clocks
    clk_hclk_config(1); // HCLK = CLK_CPU
    clk_ahb_config(CLK_AHB_SRC_PLL_PERIPH_PREDIV, 3, 1); // AHB = 600/3 = 200M
    clk_apb_config(CLK_APB_DIV_2); // APB = AHB/2 = 100M
    sdelay(10);

    // PLL_VIDEO: required by the TVD (27 MHz = 297/11), not by any display
    clk_pll_init(PLL_VIDEO, 99, 8); // 24*99/8 = 297M
    clk_pll_enable(PLL_VIDEO);

    clk_pll_init(PLL_CPU, 30, 1); // PLL_CPU = 24M*30/1 = 720M
    clk_pll_enable(PLL_CPU);
    while(!clk_pll_is_locked(PLL_CPU))
        ;

    clk_cpu_config(CLK_CPU_SRC_PLL_CPU);
    sdelay(10);
}

static void sys_uart_init(void) {
    gpio_init(GPIOE, PIN0 | PIN1, GPIO_MODE_AF5, GPIO_PULL_NONE, GPIO_DRV_3);
    clk_enable(CCU_BUS_CLK_GATE2, 20); // uart0 clock gate
    clk_reset_clear(CCU_BUS_SOFT_RST2, 20); // deassert uart0 reset
    uart_init(UART0, 115200);
}

void putchar_(char c) {
    while(!(uart_get_status(UART0) & UART_LSR_THRE))
        ;
    uart_tx(UART0, c);
}

static void sys_mmu_cache_init(void) {
    /* Everything non-cacheable by default, then DRAM cacheable, then punch
     * the DMA arena back out. Later entries overwrite earlier ones. */
    mmu_map_l1_entry(mmu_l1_tbl, 0x00000000, 0x00000000, SZ_2G, SECTION_NCNB);
    mmu_map_l1_entry(mmu_l1_tbl, 0x80000000, 0x80000000, SZ_2G, SECTION_NCNB);
    /* The part is an F1C200s: 64 MB, verified byte-exact by the predecessor
     * project including a +32 MB aliasing check. */
    mmu_map_l1_entry(mmu_l1_tbl, 0x80000000, 0x80000000, SZ_1M * 64, SECTION_CB);
    /* Capture planes + bitstream ring: shared with the TVD, VE and SD IDMAC
     * DMA masters. Non-cacheable, so there is no maintenance to forget. */
    mmu_map_l1_entry(mmu_l1_tbl, NCNB_BASE, NCNB_BASE, NCNB_SIZE, SECTION_NCNB);

    arm32_ttb_set((uint32_t)(mmu_l1_tbl));
    arm32_tlb_invalidate();
    arm32_domain_set(0x3); // Domain access - manager
    arm32_mmu_enable();
    arm32_icache_enable();
    arm32_dcache_enable();
}

static inline void sdelay(int loops) {
    __asm__ __volatile__("1:\n"
                         "subs %0, %1, #1\n"
                         "bne 1b"
                         : "=r"(loops)
                         : "0"(loops));
}
