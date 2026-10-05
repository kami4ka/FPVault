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
#include "dlog.h"
#include "board.h"
#include "usbmsc.h"
#include "usbdfu.h"
#include "usbuvc.h"
#include "usbd_video.h"
#include "spinor.h"
#include "usbphy.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include "sdcard.h"
#include "f1c100s_intc.h"
#include "f1c100s_timer.h"

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
#define USB_CONFIG_SIZE (9 + MSC_DESCRIPTOR_LEN + DFU_DESCRIPTOR_LEN + UVC_DESCRIPTOR_LEN)

extern sdcard_t* disk_card(void);
extern void USBD_IRQHandler(uint8_t busid);

static struct usbd_interface intf0, intf1, intf2, intf3;
static volatile uint8_t host_present = 0;
static volatile uint8_t card_present = 0; /* init found a card in the slot */
static volatile uint8_t card_ready = 0;   /* recorder released it to the host */
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
/* Not const: usbuvc_apply_standard() writes the live video geometry into
 * the frame descriptor before this is registered. */
static uint8_t msc_descriptor[] = {
    /* bcdDevice carries the firmware version (board.h FW_VERSION_BCD).
     * It is the only way a host can tell which firmware a board runs - the
     * boot banner goes to UART0, DFU refuses uploads, and the card holds
     * nothing but video. Releases up to v0.9.2 hardcoded 0x0100 here, so a
     * host that reads that value should report the version as unknown
     * rather than as 1.0.0; the presence of the DFU interface below tells
     * the two apart if it ever matters. */
    /* 0xEF/0x02/0x01 is Miscellaneous / Common Class / Interface
     * Association. A device carrying an IAD must say so here, or hosts are
     * entitled to ignore the association and treat the two video interfaces
     * as unrelated functions. This replaced 0x00/0x00/0x00, which is why the
     * card reader and DFU had to be re-verified when video landed. */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID,
                               FW_VERSION_BCD, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x04, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    MSC_DESCRIPTOR_INIT(0x00, MSC_OUT_EP, MSC_IN_EP, MSC_MAX_MPS, 0x02),
    DFU_DESCRIPTOR_INIT(0x01, 0x04),
    /* Video last, so mass storage stays interface 0 and DFU stays 1. The
     * desktop app's Windows probe finds DFU by its interface index. */
    UVC_DESCRIPTOR_INIT(0x02, 0x03, 0x05),
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
    /* string4: DFU interface "FPVault firmware" */
    0x22, USB_DESCRIPTOR_TYPE_STRING,
    'F',0, 'P',0, 'V',0, 'a',0, 'u',0, 'l',0, 't',0, ' ',0,
    'f',0, 'i',0, 'r',0, 'm',0, 'w',0, 'a',0, 'r',0, 'e',0,
    /* string5: video function "FPVault Camera" */
    0x1E, USB_DESCRIPTOR_TYPE_STRING,
    'F',0, 'P',0, 'V',0, 'a',0, 'u',0, 'l',0, 't',0, ' ',0,
    'C',0, 'a',0, 'm',0, 'e',0, 'r',0, 'a',0,
    0x00
};
/* clang-format on */

static void usbd_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;
    switch(event) {
    case USBD_EVENT_CONFIGURED:
        host_present = 1;
        DLOG("usb configured");
        break;
    case USBD_EVENT_RESET:
        DLOG("usb bus reset");
        break;
    case USBD_EVENT_CONNECTED:
        DLOG("usb connected");
        break;
    case USBD_EVENT_DISCONNECTED:
        DLOG("usb disconnected");
        break;
    case USBD_EVENT_SUSPEND:
        DLOG("usb suspended by the host");
        break;
    case USBD_EVENT_RESUME:
        DLOG("usb resumed");
        break;
    default:
        break;
    }
}

void usbd_msc_get_cap(uint8_t busid, uint8_t lun, uint32_t* block_num,
                      uint32_t* block_size) {
    (void)busid;
    (void)lun;
    /* Asked exactly once, when the interface is registered, and cached for
     * the rest of the boot. 0 blocks is an empty slot: the MSC class
     * answers NOT READY / MEDIUM NOT PRESENT for it, and the host shows a
     * reader with nothing in it. Only the data path waits for the release. */
    *block_size = 512;
    *block_num = disk_card()->blk_cnt;
    printf("[usb] get_cap -> %lu blocks (%s)\r\n",
           (unsigned long)disk_card()->blk_cnt,
           disk_card()->blk_cnt ? "card present" : "empty slot");
}

#ifdef FPV_DEBUG_LOG
/*
 * Card commands are served inside the USB interrupt, so for as long as one
 * takes, the camera's transfers and every control request wait. A computer
 * that reads the card hard - indexing it, scanning it, making thumbnails of
 * the clips - is therefore a way for the picture to stall, and these are
 * the numbers that would show it.
 */
static uint32_t op_max_ticks = 0;
static uint32_t slow_logged_at = 0;

void usbmsc_dlog_counts(uint32_t* rd, uint32_t* wr, uint32_t* op_max_ms) {
    *rd = rd_sectors;
    *wr = wr_sectors;
    *op_max_ms = op_max_ticks / (TICKS_PER_SEC / 1000u);
    op_max_ticks = 0;
}

static void card_op_done(const char* what, uint32_t sector, uint32_t count, uint32_t t0,
                         int ok) {
    uint32_t dt = (uint32_t)(t0 - tim_get_cnt(TIM0));
    uint32_t now = dlog_now_ms();
    if(dt > op_max_ticks) op_max_ticks = dt;
    if(!ok) {
        DLOG("msc card %s FAILED: sector %lu, %lu sectors", what, (unsigned long)sector,
             (unsigned long)count);
    } else if(dt >= TICKS_PER_SEC / 5u && now - slow_logged_at >= 1000u) {
        /* At most one a second: a slow card makes many of these. */
        slow_logged_at = now;
        DLOG("msc card %s of %lu sectors at %lu took %lu ms, USB waited meanwhile", what,
             (unsigned long)count, (unsigned long)sector,
             (unsigned long)(dt / (TICKS_PER_SEC / 1000u)));
    }
}
#endif

int usbd_msc_sector_read(uint8_t busid, uint8_t lun, uint32_t sector,
                         uint8_t* buffer, uint32_t length) {
    int ok;
#ifdef FPV_DEBUG_LOG
    uint32_t t0 = tim_get_cnt(TIM0);
#endif
    (void)busid;
    (void)lun;
    if(!card_ready) return -1;
    ok = sdcard_read(disk_card(), buffer, sector, length / 512) == length / 512;
#ifdef FPV_DEBUG_LOG
    card_op_done("read", sector, length / 512, t0, ok);
#endif
    if(!ok) return -1;
    rd_sectors += length / 512;
    return 0;
}

int usbd_msc_sector_write(uint8_t busid, uint8_t lun, uint32_t sector,
                          uint8_t* buffer, uint32_t length) {
    int ok;
#ifdef FPV_DEBUG_LOG
    uint32_t t0 = tim_get_cnt(TIM0);
#endif
    (void)busid;
    (void)lun;
    if(!card_ready) return -1;
    dlog_host_wrote(sector, length / 512);
    ok = sdcard_write(disk_card(), buffer, sector, length / 512) == length / 512;
#ifdef FPV_DEBUG_LOG
    card_op_done("write", sector, length / 512, t0, ok);
#endif
    if(!ok) return -1;
    wr_sectors += length / 512;
    return 0;
}

void usbmsc_init(void) {
    /* CherryUSB caches the capacity ONCE, inside usbd_msc_init_intf below,
     * so the card is brought up (raw, no FS) before the interface is
     * registered: ~100 ms with a card, ~15 ms without. No card means 0
     * blocks, which the class reports as an empty slot; a card inserted
     * later needs a re-plug to be seen. */
    extern int disk_raw_init(void);
    {
        uint32_t t0 = tim_get_cnt(TIM0);
        card_present = disk_raw_init() == 0;
        uint32_t ms = (uint32_t)(t0 - tim_get_cnt(TIM0)) / (TICKS_PER_SEC / 1000u);
        if(!card_present)
            printf("[usb] no card: the reader shows an empty slot (detect took %lu ms)\r\n",
                   (unsigned long)ms);
    }

    usbuvc_apply_standard(msc_descriptor, sizeof(msc_descriptor));
    usbd_desc_register(0, msc_descriptor);
    usbd_add_interface(0, usbd_msc_init_intf(0, &intf0, MSC_OUT_EP, MSC_IN_EP));
    usbd_add_interface(0, usbdfu_init_intf(&intf1));
    /* Both video interfaces take the same handler: probe and commit arrive
     * addressed to VideoStreaming, unit and terminal requests to
     * VideoControl, and the class sorts them out by request. */
    usbd_add_interface(0, usbd_video_init_intf(0, &intf2, UVC_INTERVAL_NTSC,
                                              UVC_MAX_FRAME_SIZE, UVC_MAX_MPS));
    usbd_add_interface(0, usbd_video_init_intf(0, &intf3, UVC_INTERVAL_NTSC,
                                              UVC_MAX_FRAME_SIZE, UVC_MAX_MPS));
    usbuvc_hook_notify(&intf2, &intf3);
    usbuvc_register();
    spinor_init();
    usbd_initialize(0, USBD_BASE, usbd_event_handler);
    printf("[usb] device mode up (MSC + DFU + UVC, %s)\r\n",
           MSC_MAX_MPS == 512 ? "HS" : "FS");
}

#ifdef FPV_DEBUG_LOG
void usbmsc_dlog(void) {
    DLOG("msc host=%u card=%u ready=%u ejected=%u read=%lu written=%lu sectors", host_present,
         card_present, card_ready, usbd_msc_set_popup(0), (unsigned long)rd_sectors,
         (unsigned long)wr_sectors);
}
#endif

int usbmsc_host_present(void) {
    return host_present;
}

int usbmsc_card_present(void) {
    return card_present;
}

void usbmsc_set_ready(void) {
    card_ready = 1;
    printf("[usb] card released to host\r\n");
}

void usbmsc_card_lost(void) {
    card_ready = 0;
    usbd_msc_set_medium(0, 0, false);
    printf("[usb] card lost: the reader shows an empty slot\r\n");
}

void usbmsc_stats(void) {
    if(!host_present) return;
    if(!card_present)
        printf("[usb] host attached, no card\r\n");
    else
        printf("[usb] host attached%s%s, rd %lu wr %lu sectors\r\n",
               card_ready ? "" : " (releasing card)",
               usbd_msc_set_popup(0) ? ", ejected by host" : "", (unsigned long)rd_sectors,
               (unsigned long)wr_sectors);
}
