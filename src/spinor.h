/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * spinor.h - the boot NOR (W25Q128 on SPI0) from the firmware's side.
 *
 * Until the USB firmware update existed, only U-Boot ever wrote the NOR.
 * This is the minimum the updater needs: identify, read, 4 KB erase, page
 * program. Polled, single-lane, all waits watchdog-fed.
 */
#pragma once
#include <stdint.h>

#define SPINOR_SECTOR 4096u
#define SPINOR_PAGE   256u

/* Firmware slot as U-Boot's bootcmd reads it: 256 KB at 1 MB. */
#define SPINOR_FW_OFF  0x100000u
#define SPINOR_FW_SIZE 0x040000u

void     spinor_init(void);
uint32_t spinor_read_id(void);                 /* JEDEC: 0xEF4018 for W25Q128 */
void     spinor_read(uint32_t addr, void* buf, uint32_t len);
int      spinor_erase_4k(uint32_t addr);       /* addr sector-aligned; 0 ok */
int      spinor_write(uint32_t addr, const void* buf, uint32_t len); /* 0 ok */
