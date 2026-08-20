/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * diskio.c - FatFs glue for SDC0. Based on F1C100s_projects sd_card_test
 * (GPL-3), with the two throughput handbrakes released: all six PF0-PF5
 * pins muxed (the original wired only CMD/CLK/D0) and the card brought up
 * in 4-bit mode (the board is wired for it; the U-Boot DT already declared
 * bus-width = 4).
 *
 * Clock: 25 MHz for now - Default Speed's spec ceiling. 50 MHz requires
 * the CMD6 High-Speed switch first; that lands when the M2 benchmark says
 * the workload needs it (MJPEG D1 wants ~2 MB/s; 4-bit @ 25 MHz raw is
 * 12.5 MB/s).
 */
#include "ff.h"
#include "diskio.h"
#include "sdcard.h"
#include "f1c100s_gpio.h"
#include "f1c100s_clock.h"
#include "f1c100s_sdc.h"

#define DEV_MMC 0

static sdcard_t card;
static DSTATUS dstat = STA_NOINIT;
static uint32_t want_width = MMC_BUS_WIDTH_1; /* start on the proven width */

/* Raw access for the M2 bench harness (boot-sector scrub, benchmarks). */
sdcard_t* disk_card(void) {
    return &card;
}

/* Runtime width selection: the first mount stays 1-bit (the only mode ever
 * proven on this board) so the boot-region scrub cannot be blocked by a
 * 4-bit bring-up problem; the bench then switches and remounts. Returns
 * the width that will be used on the next disk_initialize. */
uint32_t disk_toggle_width(void) {
    want_width = (want_width == MMC_BUS_WIDTH_1) ? MMC_BUS_WIDTH_4 : MMC_BUS_WIDTH_1;
    dstat = STA_NOINIT;
    return want_width;
}

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == DEV_MMC) ? dstat : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if(pdrv != DEV_MMC) return STA_NOINIT;

    clk_reset_set(CCU_BUS_SOFT_RST0, 8);
    clk_enable(CCU_BUS_CLK_GATE0, 8);
    clk_reset_clear(CCU_BUS_SOFT_RST0, 8);

    gpio_init(GPIOF, PIN0 | PIN1 | PIN2 | PIN3 | PIN4 | PIN5, GPIO_MODE_AF2,
              GPIO_PULL_UP, GPIO_DRV_2);

    card.sdc_base = SDC0_BASE;
    card.voltage = MMC_VDD_27_36;
    card.width = want_width;
    card.clock = 25000000;

    dstat = (sdcard_detect(&card) == 1) ? 0 : STA_NOINIT;
    return dstat;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if(pdrv != DEV_MMC) return RES_PARERR;
    if(dstat & STA_NOINIT) return RES_NOTRDY;
    return (sdcard_read(&card, buff, sector, count) == count) ? RES_OK : RES_ERROR;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if(pdrv != DEV_MMC) return RES_PARERR;
    if(dstat & STA_NOINIT) return RES_NOTRDY;
    return (sdcard_write(&card, (uint8_t*)buff, sector, count) == count) ? RES_OK
                                                                         : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if(pdrv != DEV_MMC) return RES_PARERR;
    switch(cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(DWORD*)buff = card.blk_cnt; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)buff = card.read_bl_len; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
