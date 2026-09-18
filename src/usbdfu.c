/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbdfu.c - DFU 1.1 download target, the plain (non-DfuSe) dialect that
 * `dfu-util -D file` speaks: blocks arrive as DNLOAD control writes with
 * an increasing block number, a zero-length DNLOAD ends the image, and the
 * host polls GETSTATUS through manifestation. Everything here runs in the
 * USB interrupt and only touches DRAM; usbdfu_poll() in the main loop
 * does the NOR burn where the watchdog is fed and the SD/USB paths keep
 * running.
 */
#include <stdio.h>
#include <string.h>
#include "usbdfu.h"
#include "board.h"
#include "spinor.h"
#include "f1c100s_timer.h"

/* DFU requests */
#define DFU_DETACH    0
#define DFU_DNLOAD    1
#define DFU_UPLOAD    2
#define DFU_GETSTATUS 3
#define DFU_CLRSTATUS 4
#define DFU_GETSTATE  5
#define DFU_ABORT     6

/* DFU states */
#define ST_IDLE           2
#define ST_DNLOAD_SYNC    3
#define ST_DNBUSY         4
#define ST_DNLOAD_IDLE    5
#define ST_MANIFEST_SYNC  6
#define ST_MANIFEST       7
#define ST_MANIFEST_WAIT  8
#define ST_ERROR          10

/* DFU status codes */
#define OK            0x00
#define ERR_FILE      0x02
#define ERR_ADDRESS   0x08
#define ERR_NOTDONE   0x09
#define ERR_FIRMWARE  0x0A
#define ERR_STALLEDPKT 0x0F

static volatile uint8_t  state = ST_IDLE;
static volatile uint8_t  status = OK;
static volatile uint32_t received = 0;    /* bytes staged so far */
static volatile uint32_t expected_block = 0;
static volatile uint8_t  image_ready = 0; /* validated, waiting for the burn */

static uint8_t* const stage = (uint8_t*)DFU_STAGE_BASE;

static void fail(uint8_t code) {
    status = code;
    state = ST_ERROR;
}

/* A firmware image built by this repo starts with the LOAD_HEADER branch
 * `b _start` (ARM: top byte 0xEA) and fits U-Boot's 256 KB read. */
static int image_valid(void) {
    return received >= 4096u && received <= SPINOR_FW_SIZE && stage[3] == 0xEA;
}

static int dfu_class_handler(uint8_t busid, struct usb_setup_packet* setup,
                             uint8_t** data, uint32_t* len) {
    (void)busid;
    switch(setup->bRequest) {
    case DFU_DNLOAD:
        if(setup->wLength) {
            if(state != ST_IDLE && state != ST_DNLOAD_IDLE) { fail(ERR_STALLEDPKT); return -1; }
            if(state == ST_IDLE) { received = 0; expected_block = 0; image_ready = 0; }
            if(setup->wValue != expected_block) { fail(ERR_ADDRESS); return -1; }
            if(received + setup->wLength > SPINOR_FW_SIZE) { fail(ERR_ADDRESS); return -1; }
            memcpy(stage + received, *data, setup->wLength);
            received += setup->wLength;
            expected_block++;
            state = ST_DNLOAD_SYNC;
        } else {
            /* End of transfer. */
            if(state != ST_DNLOAD_IDLE) { fail(ERR_NOTDONE); return -1; }
            state = ST_MANIFEST_SYNC;
        }
        *len = 0;
        return 0;

    case DFU_GETSTATUS: {
        static uint8_t rep[6];
        uint32_t poll_ms = 0;
        /* State transitions the spec ties to GETSTATUS. */
        if(state == ST_DNLOAD_SYNC) {
            state = ST_DNLOAD_IDLE;                 /* staged instantly */
        } else if(state == ST_MANIFEST_SYNC) {
            if(image_valid()) {
                image_ready = 1;                    /* main loop burns it */
                state = ST_MANIFEST;
                poll_ms = 500;
            } else {
                fail(ERR_FIRMWARE);
            }
        } else if(state == ST_MANIFEST) {
            state = ST_MANIFEST_WAIT;               /* not manifestation-tolerant */
        }
        rep[0] = status;
        rep[1] = poll_ms & 0xFF; rep[2] = (poll_ms >> 8) & 0xFF; rep[3] = (poll_ms >> 16) & 0xFF;
        rep[4] = state;
        rep[5] = 0;
        *data = rep; *len = 6;
        return 0;
    }

    case DFU_GETSTATE: {
        static uint8_t st;
        st = state;
        *data = &st; *len = 1;
        return 0;
    }

    case DFU_CLRSTATUS:
        if(state == ST_ERROR) { status = OK; state = ST_IDLE; }
        *len = 0;
        return 0;

    case DFU_ABORT:
        if(state != ST_MANIFEST && state != ST_MANIFEST_WAIT) {
            state = ST_IDLE; received = 0; expected_block = 0;
        }
        *len = 0;
        return 0;

    case DFU_DETACH:
        *len = 0;
        return 0;

    case DFU_UPLOAD:
    default:
        return -1; /* stall: no upload, no vendor requests */
    }
}

static void dfu_notify(uint8_t busid, uint8_t event, void* arg) {
    (void)busid; (void)arg;
    if(event == USBD_EVENT_RESET && !image_ready) {
        state = ST_IDLE; status = OK; received = 0; expected_block = 0;
    }
}

struct usbd_interface* usbdfu_init_intf(struct usbd_interface* intf) {
    intf->class_interface_handler = dfu_class_handler;
    intf->class_endpoint_handler = NULL;
    intf->vendor_handler = NULL;
    intf->notify_handler = dfu_notify;
    return intf;
}

static int burn(uint32_t size) {
    uint32_t sectors = (size + SPINOR_SECTOR - 1) / SPINOR_SECTOR;
    static uint8_t rb[SPINOR_SECTOR];

    for(uint32_t i = 0; i < sectors; i++) {
        wdg_feed();
        if(spinor_erase_4k(SPINOR_FW_OFF + i * SPINOR_SECTOR)) return -1;
    }
    if(spinor_write(SPINOR_FW_OFF, stage, size)) return -2;
    for(uint32_t off = 0; off < size; off += SPINOR_SECTOR) {
        uint32_t n = size - off < SPINOR_SECTOR ? size - off : SPINOR_SECTOR;
        wdg_feed();
        spinor_read(SPINOR_FW_OFF + off, rb, n);
        if(memcmp(rb, stage + off, n)) return -3;
    }
    return 0;
}

void usbdfu_poll(void) {
    static uint8_t done = 0;
    if(!image_ready || done) return;
    done = 1;

    uint32_t size = received;
    printf("[dfu] image %lu bytes staged, burning NOR @%08x\r\n",
           (unsigned long)size, (unsigned)SPINOR_FW_OFF);
    int r = burn(size);
    if(r) {
        printf("[dfu] burn failed (%d), retrying once\r\n", r);
        r = burn(size);
    }
    if(r) {
        printf("[dfu] FAILED (%d) - NOR slot is now suspect, reflash over FEL\r\n", r);
        return;
    }
    /* Let the host finish its manifest polling (dfu-util wants to see
     * dfuMANIFEST-WAIT-RESET and then issues a bus reset) before we drop
     * off the bus, so the tool exits clean instead of "device gone". */
    {
        uint32_t t0 = tim_get_cnt(TIM0);
        while(state != ST_MANIFEST_WAIT && (uint32_t)(t0 - tim_get_cnt(TIM0)) < 3u * TICKS_PER_SEC)
            wdg_feed();
        t0 = tim_get_cnt(TIM0);
        while((uint32_t)(t0 - tim_get_cnt(TIM0)) < TICKS_PER_SEC / 2u)
            wdg_feed();
    }
    printf("[dfu] verified, rebooting into the new firmware\r\n");
    ((volatile uint32_t*)BREADCRUMB_BASE)[6] = BC_REBOOT_MAGIC;
    wdg_init(WDG_MODE_RESET, WDG_INTV_500MS);
    while(1)
        ;
}
