/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * spinor.c - polled SPI0 master for the W25Q128 boot NOR.
 *
 * The F1C's SPI block is the sun6i design: one transfer = MBC bytes on the
 * clock, of which the first MTC come from the TX FIFO and, with DHB set,
 * the RX FIFO only keeps what arrives after them. So a flash command is a
 * single transfer: MTC = command+address(+data), MBC = that plus the
 * response length. FIFOs are 64 bytes deep; both directions are serviced
 * while the transfer runs, so a 256-byte page program or a 4 KB read is
 * still one transfer.
 */
#include "spinor.h"
#include "io.h"
#include "f1c100s_periph.h"
#include "f1c100s_clock.h"
#include "f1c100s_gpio.h"
#include "f1c100s_timer.h"

#define SPI_GCR   (SPI0_BASE + 0x04)
#define SPI_TCR   (SPI0_BASE + 0x08)
#define SPI_IER   (SPI0_BASE + 0x10)
#define SPI_ISR   (SPI0_BASE + 0x14)
#define SPI_FCR   (SPI0_BASE + 0x18)
#define SPI_FSR   (SPI0_BASE + 0x1C)
#define SPI_CCR   (SPI0_BASE + 0x24)
#define SPI_MBC   (SPI0_BASE + 0x30)
#define SPI_MTC   (SPI0_BASE + 0x34)
#define SPI_BCC   (SPI0_BASE + 0x38)
#define SPI_TXD   (SPI0_BASE + 0x200)
#define SPI_RXD   (SPI0_BASE + 0x300)

#define GCR_SRST     (1u << 31)
#define GCR_TP_EN    (1u << 7)
#define GCR_MASTER   (1u << 1)
#define GCR_EN       (1u << 0)
#define TCR_XCH      (1u << 31)
#define TCR_SDM      (1u << 13)
#define TCR_DHB      (1u << 8)
#define TCR_SS_LEVEL (1u << 7)
#define TCR_SS_OWNER (1u << 6)
#define TCR_SPOL     (1u << 2)
#define FCR_TF_RST   (1u << 31)
#define FCR_RF_RST   (1u << 15)
#define FIFO_DEPTH   64u

#define CCU_SPI0_CLK (CCU_BASE + 0xA0)

#define CMD_WREN   0x06
#define CMD_RDSR   0x05
#define CMD_READ   0x03
#define CMD_PP     0x02
#define CMD_SE4K   0x20
#define CMD_RDID   0x9F
#define SR_WIP     0x01

static uint32_t tx_fifo_used(void) { return (read32(SPI_FSR) >> 16) & 0xFF; }
static uint32_t rx_fifo_used(void) { return read32(SPI_FSR) & 0xFF; }

/* One transfer: clock out tx[0..txlen), then keep clocking rxlen more
 * bytes and collect them. Both FIFOs are pumped as the shift register
 * runs; the loop exits when XCH clears (transfer done). */
static void xfer(const uint8_t* tx, uint32_t txlen, uint8_t* rx, uint32_t rxlen) {
    uint32_t total = txlen + rxlen, ti = 0, ri = 0;

    write32(SPI_FCR, read32(SPI_FCR) | FCR_TF_RST | FCR_RF_RST);
    while(read32(SPI_FCR) & (FCR_TF_RST | FCR_RF_RST))
        ;
    write32(SPI_MBC, total);
    write32(SPI_MTC, txlen);
    write32(SPI_BCC, txlen);

    /* Prime the TX FIFO, assert CS (software-owned, active low), go. */
    while(ti < txlen && tx_fifo_used() < FIFO_DEPTH)
        write8(SPI_TXD, tx[ti++]);
    write32(SPI_TCR, (read32(SPI_TCR) & ~TCR_SS_LEVEL) | TCR_XCH);

    while(read32(SPI_TCR) & TCR_XCH) {
        wdg_feed();
        while(ti < txlen && tx_fifo_used() < FIFO_DEPTH)
            write8(SPI_TXD, tx[ti++]);
        while(ri < rxlen && rx_fifo_used())
            rx[ri++] = read8(SPI_RXD);
    }
    while(ri < rxlen && rx_fifo_used())
        rx[ri++] = read8(SPI_RXD);

    write32(SPI_TCR, read32(SPI_TCR) | TCR_SS_LEVEL); /* deassert CS */
}

void spinor_init(void) {
    /* PC0 CLK, PC1 CS, PC2 MISO, PC3 MOSI - same mux U-Boot's sf uses. */
    gpio_init(GPIOC, PIN0 | PIN1 | PIN2 | PIN3, GPIO_MODE_AF2, GPIO_PULL_NONE, GPIO_DRV_2);

    write32(CCU_SPI0_CLK, (1u << 31)); /* gate on, source OSC24M, /1 */
    clk_enable(CCU_BUS_CLK_GATE0, 20);
    clk_reset_set(CCU_BUS_SOFT_RST0, 20);
    clk_reset_clear(CCU_BUS_SOFT_RST0, 20);

    write32(SPI_GCR, GCR_SRST);
    while(read32(SPI_GCR) & GCR_SRST)
        ;
    write32(SPI_GCR, GCR_TP_EN | GCR_MASTER | GCR_EN);
    /* Mode 0, CS software-driven and idle high, discard the command echo
     * (DHB), MSB first. */
    write32(SPI_TCR, TCR_SS_OWNER | TCR_SS_LEVEL | TCR_SPOL | TCR_DHB);
    /* SCLK = 24 MHz / (2 * (CDR2 + 1)), CDR2 = 0 -> 12 MHz. */
    write32(SPI_CCR, (1u << 12) | 0);
    write32(SPI_IER, 0);
    write32(SPI_ISR, 0xFFFFFFFF);
}

uint32_t spinor_read_id(void) {
    uint8_t cmd = CMD_RDID, id[3];
    xfer(&cmd, 1, id, 3);
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

static void write_enable(void) {
    uint8_t cmd = CMD_WREN;
    xfer(&cmd, 1, 0, 0);
}

/* Wait for WIP to clear; bounded so a dead chip cannot hang the updater
 * forever (worst legal 4 KB erase is 400 ms, page program 3 ms). */
static int wait_ready(uint32_t max_ms) {
    uint8_t cmd = CMD_RDSR, sr;
    uint32_t t0 = tim_get_cnt(TIM0);
    for(;;) {
        xfer(&cmd, 1, &sr, 1);
        if(!(sr & SR_WIP)) return 0;
        if((uint32_t)(t0 - tim_get_cnt(TIM0)) > max_ms * 24000u) return -1;
    }
}

void spinor_read(uint32_t addr, void* buf, uint32_t len) {
    uint8_t* p = buf;
    while(len) {
        uint32_t n = len > 4096u ? 4096u : len;
        uint8_t cmd[4] = { CMD_READ, addr >> 16, addr >> 8, addr };
        xfer(cmd, 4, p, n);
        addr += n; p += n; len -= n;
    }
}

int spinor_erase_4k(uint32_t addr) {
    uint8_t cmd[4] = { CMD_SE4K, addr >> 16, addr >> 8, addr };
    write_enable();
    xfer(cmd, 4, 0, 0);
    return wait_ready(500);
}

int spinor_write(uint32_t addr, const void* buf, uint32_t len) {
    const uint8_t* p = buf;
    while(len) {
        uint32_t room = SPINOR_PAGE - (addr & (SPINOR_PAGE - 1));
        uint32_t n = len < room ? len : room;
        uint8_t cmd[4 + SPINOR_PAGE] = { CMD_PP, addr >> 16, addr >> 8, addr };
        for(uint32_t i = 0; i < n; i++) cmd[4 + i] = p[i];
        write_enable();
        xfer(cmd, 4 + n, 0, 0);
        if(wait_ready(10)) return -1;
        addr += n; p += n; len -= n;
    }
    return 0;
}
