/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbdfu.h - firmware update over USB (DFU 1.1, plain download mode).
 *
 * Lives next to the mass-storage interface in the same configuration, so
 * `dfu-util -D fpvault.bin` works on whatever the board is plugged into,
 * with no buttons and nothing on the SD card. The image is staged in DRAM,
 * checked, and only then burnt to the NOR slot U-Boot boots from; the
 * board reboots into it by itself.
 */
#pragma once
#include <stdint.h>
#include "usbd_core.h"

/* Interface descriptor fragment for the config descriptor: DFU-mode
 * interface (class FE/01/02) + the 9-byte DFU functional descriptor. */
#define DFU_XFER_SIZE 4096u
#define DFU_DESCRIPTOR_LEN (9 + 9)
#define DFU_DESCRIPTOR_INIT(bInterfaceNumber, str_idx)                                \
    USB_INTERFACE_DESCRIPTOR_INIT(bInterfaceNumber, 0x00, 0x00, 0xFE, 0x01, 0x02, str_idx), \
    0x09, 0x21,               /* DFU functional descriptor */                             \
    0x01,                     /* bmAttributes: bitCanDnload only */                       \
    0xFF, 0x00,               /* wDetachTimeOut 255 ms */                                 \
    WBVAL(DFU_XFER_SIZE),     /* wTransferSize */                                         \
    0x10, 0x01                /* bcdDFUVersion 1.10 */

struct usbd_interface* usbdfu_init_intf(struct usbd_interface* intf);

/* Main-loop hook: burns a fully received, validated image to NOR and
 * reboots. NOR work stays out of the USB interrupt on purpose. */
void usbdfu_poll(void);
