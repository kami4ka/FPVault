/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ve.c - Cedar VE bring-up for the suniv (F1C100s/F1C200s) family.
 * Sequence from Allwinner's cedar_ve (CONFIG_ARCH_SUNIVW1P1 branch).
 */
#include "ve.h"
#include "f1c100s_clock.h"

static inline void sdelay(volatile int loops) {
    while(loops--)
        ;
}

void ve_init(void) {
    /* PLL_VE = 24 MHz * 25 / 2 = 300 MHz - what Allwinner's driver picks
     * for suniv (documented range 30..600, default 210). */
    clk_pll_init(PLL_VE, 25, 2);
    clk_pll_enable(PLL_VE);
    while(!clk_pll_is_locked(PLL_VE))
        ;

    /* Module clock: source is PLL_VE, bit 31 gates it. */
    write32(CCU_BASE + CCU_VE_CLK, (1u << 31));

    /* AHB gate, then a full soft-reset cycle, then the VE's own DRAM port. */
    clk_enable(CCU_BUS_CLK_GATE1, 0);
    clk_reset_set(CCU_BUS_SOFT_RST1, 0);
    sdelay(100);
    clk_reset_clear(CCU_BUS_SOFT_RST1, 0);
    clk_enable(CCU_DRAM_CLK_GATE, 0);

    /* Map SRAM C (512 KB at 0x01D00000) to the VE - it does not work
     * without its working memory. Keep bit 31, set the rest, exactly as
     * the vendor driver does. */
    write32(SRAMC_BASE + 0x000,
            (read32(SRAMC_BASE + 0x000) & 0x80000000u) | 0x7fffffffu);

    /* Engine-level reset, then park the sub-engine select on "none". */
    ve_w(VE_CTRL, 0x00130007u);
    sdelay(100);
}

uint32_t ve_version(void) {
    return ve_r(VE_VERSION);
}

void ve_select_avc(void) {
    uint32_t ctrl = ve_r(VE_CTRL);
    ve_w(VE_CTRL, (ctrl & 0xfffffff0u) | 0xbu);
}
