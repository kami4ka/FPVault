/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * defe.c - see defe.h.
 */
#include "defe.h"
#include "io.h"
#include "f1c100s_periph.h"
#include "f1c100s_de.h"
#include "f1c100s_clock.h"
#include "f1c100s_timer.h"

/* Clock plumbing, all four pieces, taken from mainline's suniv CCU driver
 * (ccu-suniv-f1c100s.c) rather than guessed: AHB gate 0x064 bit 14, bus
 * reset 0x2c4 bit 14, DRAM port gate 0x100 bit 24, module clock 0x10c with
 * the mux at [26:24] and the gate at bit 31. The DRAM gate is the one that
 * is easy to miss and impossible to debug: without it the engine runs,
 * reports completion, and writes nothing. */
#define DEFE_BUS_GATE_BIT  14
#define DEFE_BUS_RESET_BIT 14
#define DEFE_DRAM_GATE_BIT 24

/* From mainline sun4i_frontend.c, which took them from Allwinner's A33 BSP.
 * GPL-2.0+, so usable here. 32 phases; the horizontal filter keeps two
 * words per phase in two banks, the vertical one word. */
static const uint32_t horz_coef[64] = {
    0x40000000, 0x00000000, 0x40fe0000, 0x0000ff03, 0x3ffd0000, 0x0000ff05,
    0x3ffc0000, 0x0000ff06, 0x3efb0000, 0x0000ff08, 0x3dfb0000, 0x0000ff09,
    0x3bfa0000, 0x0000fe0d, 0x39fa0000, 0x0000fe0f, 0x38fa0000, 0x0000fe10,
    0x36fa0000, 0x0000fe12, 0x33fa0000, 0x0000fd16, 0x31fa0000, 0x0000fd18,
    0x2ffa0000, 0x0000fd1a, 0x2cfa0000, 0x0000fc1e, 0x29fa0000, 0x0000fc21,
    0x27fb0000, 0x0000fb23, 0x24fb0000, 0x0000fb26, 0x21fb0000, 0x0000fb29,
    0x1ffc0000, 0x0000fa2b, 0x1cfc0000, 0x0000fa2e, 0x19fd0000, 0x0000fa30,
    0x16fd0000, 0x0000fa33, 0x14fd0000, 0x0000fa35, 0x11fe0000, 0x0000fa37,
    0x0ffe0000, 0x0000fa39, 0x0dfe0000, 0x0000fa3b, 0x0afe0000, 0x0000fa3e,
    0x08ff0000, 0x0000fb3e, 0x06ff0000, 0x0000fb40, 0x05ff0000, 0x0000fc40,
    0x03ff0000, 0x0000fd41, 0x01ff0000, 0x0000fe42,
};
static const uint32_t vert_coef[32] = {
    0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc, 0xff083dfc,
    0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa, 0xfe1433fb, 0xfe1631fb,
    0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb, 0xfc2127fc, 0xfc2424fc, 0xfc2721fc,
    0xfb291ffd, 0xfb2c1cfd, 0xfb2f19fd, 0xfb3116fe, 0xfb3314fe, 0xfa3611ff,
    0xfb370fff, 0xfb390dff, 0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400,
    0xfe3f0300, 0xff400100,
};

void defe_w(uint32_t off, uint32_t val) {
    write32(DEFE_BASE + off, val);
}
uint32_t defe_r(uint32_t off) {
    return read32(DEFE_BASE + off);
}

static inline void sdelay(volatile int loops) {
    while(loops--)
        ;
}

static void defe_load_coef(void) {
    int i;
    /* Hand the coefficient RAM to the CPU, fill both channels, hand it
     * back. Two separate gates guard that RAM and the references each
     * mention only one: mainline uses FRM_CTRL's COEF_ACCESS_CTRL, the
     * Lichee player uses EN's BIST bit. Both are set here, because getting
     * it wrong is silent - the writes are simply dropped, the filter runs
     * with zero taps, and every output sample comes out zero. That is
     * exactly what this board did until the coefficient self-test caught
     * it. Luma and chroma get the same taps; the chroma channel runs at
     * half the sample rate but the filter phases are the same shape. */
    defe_w(DEFE_EN, DEFE_EN_ENABLE | DEFE_EN_BIST);
    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) | DEFE_FRM_COEF_ACC);
    for(i = 0; i < 32; i++) {
        defe_w(DEFE_CH0_H_COEF + i * 4, horz_coef[2 * i]);
        defe_w(DEFE_CH1_H_COEF + i * 4, horz_coef[2 * i]);
        defe_w(DEFE_CH0_H_COEF1 + i * 4, horz_coef[2 * i + 1]);
        defe_w(DEFE_CH1_H_COEF1 + i * 4, horz_coef[2 * i + 1]);
        defe_w(DEFE_CH0_V_COEF + i * 4, vert_coef[i]);
        defe_w(DEFE_CH1_V_COEF + i * 4, vert_coef[i]);
    }
    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) & ~DEFE_FRM_COEF_ACC);
    defe_w(DEFE_EN, DEFE_EN_ENABLE);
    /* Tell the engine a new coefficient set is ready to latch. */
    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) | DEFE_FRM_COEF_RDY);
}

/* Does the coefficient RAM take writes at all? Reads one word back under
 * the same access gating the loader uses. Returns the value read. */
uint32_t defe_coef_readback(void) {
    uint32_t v;
    defe_w(DEFE_EN, DEFE_EN_ENABLE | DEFE_EN_BIST);
    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) | DEFE_FRM_COEF_ACC);
    v = defe_r(DEFE_CH0_H_COEF);
    defe_w(DEFE_FRM_CTRL, defe_r(DEFE_FRM_CTRL) & ~DEFE_FRM_COEF_ACC);
    defe_w(DEFE_EN, DEFE_EN_ENABLE);
    return v;
}

/* The colour converter sits in the path whenever the engine runs at all -
 * on this part setting either BYPASS bit stops it running rather than
 * bypassing a stage - and its twelve coefficients come out of reset as
 * zero. An unprogrammed matrix turns a clean image into noise, which is
 * precisely what every pass produced until this was loaded.
 *
 * Layout confirmed against the register scan, which is also what mainline
 * describes: three rows of three multipliers plus a constant, the
 * multipliers 13-bit signed with 10 fractional bits (mask 0x1fff at 0x070,
 * 0x074, 0x078) and the constant 14-bit signed with 4 (mask 0x3fff at
 * 0x07c). Identity is therefore 1024 on the diagonal and zero elsewhere. */
void defe_load_csc_diag(uint32_t diag) {
    uint32_t id[12] = {diag, 0, 0, 0, 0, diag, 0, 0, 0, 0, diag, 0};
    int i;
    for(i = 0; i < 12; i++)
        defe_w(DEFE_CSC_COEF + i * 4, id[i]);
}

static uint32_t csc_diag = 1024;

static void defe_load_csc_identity(void) {
    defe_load_csc_diag(csc_diag);
}

void defe_set_csc_diag(uint32_t d) {
    csc_diag = d;
}

void defe_reset(void) {
    /* PLL_VIDEO is already up at 297 MHz from sys_clk_init - the TVE needs
     * it - so the module clock only has to pick it and ungate. Divider 1:
     * a 1280x720 pass is 0.92 Mpixel, and even one pixel per clock leaves
     * two orders of magnitude of headroom at 30 fps. */
    clk_de_config(CCU_DEFE_CLK, CLK_DE_SRC_PLL_VIDEO, 1);
    /* clk_de_config writes the mux and the divider and nothing else - it
     * leaves bit 31 exactly as it found it, and reset leaves it clear. So
     * the gate is set here; without this the block is addressable over the
     * AHB and simply never runs. */
    write32(CCU_BASE + CCU_DEFE_CLK, read32(CCU_BASE + CCU_DEFE_CLK) | (1u << 31));

    clk_enable(CCU_BUS_CLK_GATE1, DEFE_BUS_GATE_BIT);
    clk_reset_set(CCU_BUS_SOFT_RST1, DEFE_BUS_RESET_BIT);
    sdelay(100);
    clk_reset_clear(CCU_BUS_SOFT_RST1, DEFE_BUS_RESET_BIT);
    clk_enable(CCU_DRAM_CLK_GATE, DEFE_DRAM_GATE_BIT);
    sdelay(100);

    defe_w(DEFE_EN, DEFE_EN_ENABLE);
    defe_load_coef();
    defe_load_csc_identity();
}

void defe_init(void) {
    defe_reset();
}

void defe_start(const defe_cfg_t* c) {
    uint32_t sy = c->src_strd_y ? c->src_strd_y : c->sw;
    uint32_t hf = ((uint32_t)c->sw << 16) / c->dw;
    uint32_t vf = ((uint32_t)c->sh << 16) / c->dh;

    /* Engine owns the coefficient RAM while a frame runs. */
    defe_w(DEFE_EN, DEFE_EN_ENABLE);

    defe_w(DEFE_BYPASS, c->use_bits
                            ? (uint32_t)c->bypass_bits
                            : (DEFE_BYPASS_CSC |
                               (c->bypass ? DEFE_BYPASS_SCALER : 0u)));
    defe_w(DEFE_AGTH_SEL, 2); /* 4:1, what the Lichee player uses */

    /* Input: NV12, two planes, chroma plane as wide as luma. */
    defe_w(DEFE_ADDR0, c->src_y);
    defe_w(DEFE_ADDR1, c->src_c);
    defe_w(DEFE_ADDR2, 0);
    defe_w(DEFE_STRIDE0, sy);
    defe_w(DEFE_STRIDE1, sy);
    defe_w(DEFE_STRIDE2, 0);
    defe_w(DEFE_IN_FMT, c->in_fmt ? c->in_fmt : (uint32_t)DEFE_IN_NV12);
    defe_w(DEFE_FIELD_CTRL, 0);
    defe_w(DEFE_TB_OFF0, 0);
    defe_w(DEFE_TB_OFF1, 0);
    defe_w(DEFE_TB_OFF2, 0);

    /* Write-back: up to three planes, each with its own stride. */
    /* One address, and no stride registers to go with it: 0x054, 0x058 and
     * 0x0d0-0x0dc hold no bits here. The write-back sink is a single
     * contiguous buffer, which is why a three-plane output format has
     * nowhere to put planes one and two. */
    defe_w(DEFE_WB_ADDR0, c->dst0);
    defe_w(DEFE_OUT_FMT, c->out_fmt & 7u);

    /* Sizes are written minus one, both channels. Chroma is half in each
     * direction for 4:2:0 in and 4:2:0 out. */
    defe_w(DEFE_IN_SIZE, (((uint32_t)c->sh - 1) << 16) | (uint32_t)(c->sw - 1));
    defe_w(DEFE_OUT_SIZE, (((uint32_t)c->dh - 1) << 16) | (uint32_t)(c->dw - 1));
    defe_w(DEFE_H_FACT, hf);
    defe_w(DEFE_V_FACT, vf);
    /* No CH1 registers and no phase registers are programmed: the register
     * scan (:F) shows 0x200-0x21c and 0x110-0x118 hold no bits on this
     * part. The sun4i frontend's second scaler channel and its FIR phase
     * controls simply are not here. */

    /* Clear any stale completion, then latch and go. OUT_CTRL keeps the
     * result off the backend, which is neither clocked nor wanted. */
    defe_w(DEFE_INT_STATUS, defe_r(DEFE_INT_STATUS));
    defe_w(DEFE_INT_EN, 0);
    defe_w(DEFE_FRM_CTRL, (defe_r(DEFE_FRM_CTRL) & ~(3u << 8)) |
                              DEFE_FRM_OUT_PORT(c->out_port) |
                              (c->out_ctrl ? DEFE_FRM_OUT_CTRL : 0u) |
                              DEFE_FRM_WB_EN | DEFE_FRM_REG_RDY |
                              DEFE_FRM_START);
}

int defe_wait(uint32_t timeout_us, uint32_t* status, uint32_t* intst) {
    uint32_t t0 = tim_get_cnt(TIM0); /* down-counter at 24 MHz */
    uint32_t st = 0, is = 0;
    int seen_busy = 0;
    for(;;) {
        st = defe_r(DEFE_STATUS);
        is = defe_r(DEFE_INT_STATUS);
        if(st & DEFE_STATUS_WB_BUSY) seen_busy = 1;
        /* Either the completion flag, or busy having come and gone. The
         * pass can be short enough that the busy bit is never observed, so
         * neither signal alone is enough to wait on. */
        if((is & DEFE_INT_WB_DONE) || (seen_busy && !(st & DEFE_STATUS_WB_BUSY)))
            break;
        if((uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u > timeout_us) {
            if(status) *status = st;
            if(intst) *intst = is;
            return -1;
        }
    }
    if(status) *status = st;
    if(intst) *intst = is;
    return 0;
}
