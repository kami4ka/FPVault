/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbmsc.c - see usbmsc.h. Descriptors and bring-up sequence derived from
 * f1c200s_library's usbd_msc_config.c (MIT, lhdjply) with the RT-Thread
 * coupling replaced by this firmware's INTC; PHY/clock recipe in
 * src/usbphy.c (same origin, cross-validated against mainline Linux
 * musb_sunxi/phy-sun4i-usb and f1c_nonos).
 *
 * The MSC class runs in the USB IRQ (no thread). Until the recorder has
 * released the card (usbmsc_set_ready), sector ops fail cleanly - the
 * host retries INQUIRY/READ for several seconds, which is far longer
 * than the recorder needs to finalize a clip and unmount.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "usbmsc.h"
#include "usbphy.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include "sdcard.h"
#include "f1c100s_intc.h"

#define USBD_BASE 0x01c13000UL
#define USB_IRQ 26

#define MSC_IN_EP 0x81
#define MSC_OUT_EP 0x02
#ifdef CONFIG_USB_HS
#define MSC_MAX_MPS 512
#else
#define MSC_MAX_MPS 64
#endif

#define USBD_VID 0x34B7 /* pid.codes-style community VID space placeholder */
#define USBD_PID 0xF1C2
#define USBD_MAX_POWER 250 /* mA */
#define USBD_LANGID_STRING 1033
#define USB_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN)

extern sdcard_t* disk_card(void);
extern void USBD_IRQHandler(uint8_t busid);

static struct usbd_interface intf0;
static volatile uint8_t host_present = 0;
static volatile uint8_t card_ready = 0;
static volatile uint32_t rd_sectors = 0, wr_sectors = 0;

static void usb_irq(void) {
    USBD_IRQHandler(0);
}

void usb_dc_low_level_init(void) {
    usb_phy_open_clock();
    USBC_PhyConfig();
    USBC_ConfigFIFO_Base();
    USBC_EnableDpDmPullUp();
    USBC_EnableIdPullUp();
    USBC_ForceId(USBC_ID_TYPE_DEVICE);
    USBC_ForceVbusValid(USBC_VBUS_TYPE_HIGH);

    intc_set_irq_handler(USB_IRQ, usb_irq);
    intc_enable_irq(USB_IRQ);
}

/* clang-format off */
static const uint8_t msc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, MSC_MAX_MPS, 0x02),
    /* string0: language */
    USB_LANGID_INIT(USBD_LANGID_STRING),
    /* string1: manufacturer "FPVault" */
    0x10, USB_DESCRIPTOR_TYPE_STRING,
    'F',0, 'P',0, 'V',0, 'a',0, 'u',0, 'l',0, 't',0,
    /* string2: product "FPVault SD Card" */
    0x20, USB_DESCRIPTOR_TYPE_STRING,
    'F',0, 'P',0, 'V',0, 'a',0, 'u',0, 'l',0, 't',0, ' ',0, 'S',0, 'D',0, ' ',0, 'C',0, 'a',0, 'r',0, 'd',0,
    /* string3: serial "00000001" */
    0x12, USB_DESCRIPTOR_TYPE_STRING,
    '0',0, '0',0, '0',0, '0',0, '0',0, '0',0, '0',0, '1',0,
    0x00
};
/* clang-format on */

static void usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;
    switch(event) {
    case USBD_EVENT_CONFIGURED:
        host_present = 1;
        break;
    default:
        break;
    }
}

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t* block_num,
                      uint32_t* block_size) {
    (void)busid;
    (void)lun;
    /* Capacity is known the moment the recorder has ever mounted the card;
     * reporting 0 here makes the host cache a dead device and never retry
     * (observed with macOS). Only the data path waits for the release. */
    *block_size = 512;
    *block_num = disk_card()->blk_cnt;
    printf("[usb] get_cap -> %lu blocks (ready=%u)\r\n",
           (unsigned long)disk_card()->blk_cnt, card_ready);
}

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector,
                         uint8_t* buffer, uint32_t length) {
    (void)busid;
    (void)lun;
    if(!card_ready) return -1;
    if(sdcard_read(disk_card(), buffer, sector, length / 512) != length / 512)
        return -1;
    rd_sectors += length / 512;
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector,
                          uint8_t* buffer, uint32_t length) {
    (void)busid;
    (void)lun;
    if(!card_ready) return -1;
    if(sdcard_write(disk_card(), buffer, sector, length / 512) != length / 512)
        return -1;
    wr_sectors += length / 512;
    return 0;
}

void usbmsc_init(void) {
    /* CherryUSB caches the capacity ONCE, inside usbd_msc_init_intf below.
     * The card must therefore be up (raw, no FS) before the interface is
     * registered - otherwise the host is told "0 blocks" forever and every
     * READ(10) dies on the stack's own LBA range check. The recorder's
     * later f_mount re-runs detection; this early init costs ~100 ms. */
    extern int disk_raw_init(void);
    if(disk_raw_init() != 0)
        printf("[usb] no card at init - exporting 0 blocks\r\n");

    usbd_desc_register(0, msc_descriptor);
    usbd_add_interface(0, usbd_msc_init_intf(0, &intf0, MSC_OUT_EP, MSC_IN_EP));
    usbd_initialize(0, USBD_BASE, usbd_event_handler);
    printf("[usb] device mode up (MSC, %s)\r\n",
           MSC_MAX_MPS == 512 ? "HS" : "FS");
}

int usbmsc_host_present(void) {
    return host_present;
}

void usbmsc_set_ready(void) {
    card_ready = 1;
    printf("[usb] card released to host\r\n");
}

void usbmsc_stats(void) {
    if(host_present)
        printf("[usb] host attached%s, rd %lu wr %lu sectors\r\n",
               card_ready ? "" : " (releasing card)", (unsigned long)rd_sectors,
               (unsigned long)wr_sectors);
}
