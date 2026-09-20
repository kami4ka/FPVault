/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbuvc.c - the USB video interface, and the pump that feeds it.
 *
 * There is no mode to switch into. UVC streaming is host-initiated: the
 * class calls usbd_video_open() when something on the computer opens the
 * camera and usbd_video_close() when it lets go, so the board is a camera
 * exactly while a camera is being watched, and a card reader the whole time
 * either way. Both at once, since they are separate interfaces.
 *
 * The frames cost nothing to produce. main.c starts the pipeline before the
 * host fork and never stops it, so in card-reader mode the VE is already
 * encoding JPEGs that recorder.c throws away at its early return. This file
 * takes those instead of the floor.
 *
 * Why bulk makes the pump this short: UVC puts one payload header at the
 * front of each payload *transfer*, not each packet, and for bulk a whole
 * frame can be one transfer. So a frame goes out as a 2-byte header
 * immediately followed by the JPEG, in a single usbd_ep_start_write, with no
 * per-packet work and no copy of the payload. An isochronous design would
 * have to chop the frame into 512-byte packets and stamp a header on every
 * one of them.
 */
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "usbd_core.h"
#include "usbd_video.h"
#include "usbuvc.h"
#include "pipeline.h"
#include "f1c100s_timer.h"

/* The payload header has to abut the JPEG, so it lives in the slot's own
 * slack immediately before it. The recorder's AVI chunk header wants the
 * same bytes for the same reason, and they overlap - which is safe only
 * because the two sinks are mutually exclusive in practice: a host that can
 * open the camera has already put the recorder into REC_USB_MODE, where it
 * never writes. Each sink rewrites its own header every frame regardless. */
#define UVC_HDR_LEN 2u
#define UVC_HDR_OFF (BSRING_PREFIX_OFF - UVC_HDR_LEN)

/* bmHeaderInfo bits. EOF is set on every transfer because one transfer is
 * one whole frame; FID toggles so the host can tell frames apart. */
#define UVC_BM_EOH 0x80u
#define UVC_BM_EOF 0x02u
#define UVC_BM_FID 0x01u

static volatile uint8_t streaming = 0;
static volatile uint8_t busy = 0;
static uint8_t fid = 0;

static uint32_t frames_sent = 0, frames_dropped = 0, bytes_sent = 0;
static uint32_t last_sent = 0;
/* TIM0 when the in-flight transfer was queued. TIM0 counts down. */
static uint32_t busy_at = 0;

/*
 * How long to wait for a transfer before deciding the host has gone.
 *
 * With one alternate setting there is no stop signal: macOS sends
 * SET_INTERFACE alt 0 as part of *starting* - after commit - and says
 * nothing at all when an application closes the camera. So the only
 * evidence that nobody is watching any more is a transfer that never
 * completes, and without this the stream wedges permanently the first time
 * an app exits. A second is far longer than a frame and far shorter than a
 * person notices.
 */
#define UVC_STALL_TICKS TICKS_PER_SEC

int usbuvc_streaming(void) {
    return streaming;
}

/* Transfer complete. Nothing to do but let the next frame in: the payload
 * was sent straight out of the ring slot, so there is no buffer to free. */
static void uvc_in_done(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    bytes_sent += nbytes;
    frames_sent++;
    busy = 0;
}

static struct usbd_endpoint uvc_in_ep = {
    .ep_addr = UVC_IN_EP,
    .ep_cb = uvc_in_done,
};

/*
 * Probe and commit, for the VideoStreaming interface.
 *
 * CherryUSB's class answers these too, but it starts streaming only when the
 * host selects alternate setting 1, and macOS never does: with a bulk
 * endpoint it selects interface 3 alt 0 and nothing else. Observed on the
 * wire. So the stream's own requests are answered here, and commit is what
 * starts it - which is the signal that actually means "begin" for a bulk
 * device, since after committing a format the host simply starts reading.
 *
 * The negotiated structure is UVC 1.0's 26 bytes, matching the bcdUVC 0x0100
 * in the descriptors. Only one format and one frame size are offered, so
 * every GET returns the same thing and a SET is accepted without argument:
 * there is nothing for the host to choose.
 */
#define UVC_SET_CUR  0x01
#define UVC_GET_CUR  0x81
#define UVC_GET_MIN  0x82
#define UVC_GET_MAX  0x83
#define UVC_GET_RES  0x84
#define UVC_GET_LEN  0x85
#define UVC_GET_INFO 0x86
#define UVC_GET_DEF  0x87

#define UVC_VS_PROBE_CONTROL  0x01
#define UVC_VS_COMMIT_CONTROL 0x02

#define UVC_PROBE_LEN 26u

static uint8_t probe_ctrl[UVC_PROBE_LEN];

static void probe_defaults(void) {
    uint32_t interval = UVC_INTERVAL_NTSC;
    uint32_t maxframe = UVC_MAX_FRAME_SIZE;

    for(unsigned i = 0; i < UVC_PROBE_LEN; i++) probe_ctrl[i] = 0;
    probe_ctrl[0] = 0x01; /* bmHint: hold dwFrameInterval */
    probe_ctrl[2] = 0x01; /* bFormatIndex: the only format, MJPEG */
    probe_ctrl[3] = 0x01; /* bFrameIndex: 720x480; see usbuvc_set_standard */
    probe_ctrl[4] = (uint8_t)(interval);
    probe_ctrl[5] = (uint8_t)(interval >> 8);
    probe_ctrl[6] = (uint8_t)(interval >> 16);
    probe_ctrl[7] = (uint8_t)(interval >> 24);
    probe_ctrl[18] = (uint8_t)(maxframe);
    probe_ctrl[19] = (uint8_t)(maxframe >> 8);
    probe_ctrl[20] = (uint8_t)(maxframe >> 16);
    probe_ctrl[21] = (uint8_t)(maxframe >> 24);
    /* dwMaxPayloadTransferSize: a whole frame goes out as one bulk transfer,
     * so that is what a payload transfer is here. */
    probe_ctrl[22] = (uint8_t)(maxframe);
    probe_ctrl[23] = (uint8_t)(maxframe >> 8);
    probe_ctrl[24] = (uint8_t)(maxframe >> 16);
    probe_ctrl[25] = (uint8_t)(maxframe >> 24);
}

static int uvc_vs_request(uint8_t busid, struct usb_setup_packet* setup,
                          uint8_t** data, uint32_t* len) {
    uint8_t cs = (uint8_t)(setup->wValue >> 8);
    static uint8_t scratch[2];

    (void)busid;
    if(cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) return -1;

    switch(setup->bRequest) {
    case UVC_GET_CUR:
    case UVC_GET_MIN:
    case UVC_GET_MAX:
    case UVC_GET_DEF:
        *data = probe_ctrl;
        *len = UVC_PROBE_LEN;
        return 0;
    case UVC_GET_RES:
        /* Nothing is adjustable, so the resolution of every field is zero. */
        for(unsigned i = 0; i < UVC_PROBE_LEN; i++) (*data)[i] = 0;
        *len = UVC_PROBE_LEN;
        return 0;
    case UVC_GET_LEN:
        scratch[0] = UVC_PROBE_LEN;
        scratch[1] = 0;
        *data = scratch;
        *len = 2;
        return 0;
    case UVC_GET_INFO:
        scratch[0] = 0x03; /* supports GET and SET */
        *data = scratch;
        *len = 1;
        return 0;
    case UVC_SET_CUR:
        /* The host echoes back what it intends to use. There is only one
         * choice, so it is accepted and ignored - except that a commit is
         * the moment streaming begins. */
        if(cs == UVC_VS_COMMIT_CONTROL) {
            usbd_video_open(0, 0);
        }
        return 0;
    default:
        return -1;
    }
}

/*
 * Our own notify handler, replacing the class's after it has been set up.
 *
 * The class's version starts streaming only on bAlternateSetting == 1 and
 * says nothing about what it saw, which is a bad place to be when the host
 * is choosing an alternate setting and the device is not streaming. This
 * reports every transition and keeps the same open/close behaviour, so it
 * can answer the question the class cannot: what did the host actually ask
 * for? It does not touch class_interface_handler, so probe and commit are
 * still entirely the class's.
 */
static void uvc_notify(uint8_t busid, uint8_t event, void* arg) {
    struct usb_interface_descriptor* intf;

    (void)busid;
    if(event == USBD_EVENT_SET_INTERFACE) {
        intf = (struct usb_interface_descriptor*)arg;
        printf("[uvc] host selected interface %u alt %u\r\n",
               (unsigned)intf->bInterfaceNumber, (unsigned)intf->bAlternateSetting);
    } else if(event == USBD_EVENT_RESET) {
        streaming = 0;
        busy = 0;
        probe_defaults();
    }
}

void usbuvc_register(void) {
    usbd_add_endpoint(0, &uvc_in_ep);
}

void usbuvc_hook_notify(struct usbd_interface* vc, struct usbd_interface* vs) {
    probe_defaults();
    vc->notify_handler = uvc_notify;
    /* The streaming interface is ours entirely: the class's handler starts
     * on an alternate setting this host never selects. */
    vs->notify_handler = uvc_notify;
    vs->class_interface_handler = uvc_vs_request;
}

void usbd_video_open(uint8_t busid, uint8_t intf) {
    (void)busid;
    (void)intf;
    busy = 0;
    fid = 0;
    streaming = 1;
    printf("[uvc] streaming on\r\n");
}

void usbd_video_close(uint8_t busid, uint8_t intf) {
    (void)busid;
    (void)intf;
    streaming = 0;
    printf("[uvc] streaming off after %lu frames\r\n", (unsigned long)frames_sent);
}

/*
 * Hand one encoded frame to the host, if it is watching and the last one has
 * gone. Called from the main loop, never the IRQ, so a stalled host costs
 * frames and nothing else - capture keeps running in the 1 kHz tick.
 *
 * Dropping rather than queueing is the right behaviour for a camera: the
 * host wants the newest frame, not a backlog. The ring's own back-pressure
 * still applies behind this, and it drops at the cheapest point there is, by
 * skipping the encode entirely.
 */
void usbuvc_on_frame(uint32_t slot_base, uint32_t bitstream_len, int quality) {
    uint8_t* slot = (uint8_t*)slot_base;
    uint32_t jpeg_len, total;

    if(!streaming) return;
    if(busy) {
        if((uint32_t)(busy_at - tim_get_cnt(TIM0)) > UVC_STALL_TICKS) {
            printf("[uvc] host stopped reading, stream idle after %lu frames\r\n",
                   (unsigned long)frames_sent);
            streaming = 0;
            busy = 0;
        }
        frames_dropped++;
        return;
    }

    jpeg_len = pipeline_finish_jpeg(slot_base, bitstream_len, quality);

    /* A bulk transfer ends on a short packet. One that happens to be an
     * exact multiple of the packet size would leave the host waiting and
     * then merge the next frame into this one, so a byte is appended past
     * EOI to break it. Decoders ignore anything after EOI. */
    total = UVC_HDR_LEN + jpeg_len;
    if((total % UVC_MAX_MPS) == 0u) {
        slot[BSRING_DATA_OFF + bitstream_len + 2u] = 0x00;
        jpeg_len++;
        total++;
    }

    slot[UVC_HDR_OFF] = UVC_HDR_LEN;
    slot[UVC_HDR_OFF + 1] = UVC_BM_EOH | UVC_BM_EOF | (fid ? UVC_BM_FID : 0u);
    fid ^= 1u;

    /* Sent straight out of the ring slot. It cannot be overwritten under us:
     * the producer needs all 40 slots to come back around, about 1.3 s,
     * against a transfer measured in tens of milliseconds. */
    busy = 1;
    busy_at = tim_get_cnt(TIM0);
    if(usbd_ep_start_write(0, UVC_IN_EP, slot + UVC_HDR_OFF, total) != 0) {
        busy = 0;
        frames_dropped++;
    }
}

void usbuvc_stats(void) {
    uint32_t fps;
    if(!streaming) return;
    fps = frames_sent - last_sent;
    last_sent = frames_sent;
    printf("[uvc] %lu fps, %lu sent, %lu dropped, %lu KB total\r\n",
           (unsigned long)fps, (unsigned long)frames_sent,
           (unsigned long)frames_dropped, (unsigned long)(bytes_sent / 1024u));
}
