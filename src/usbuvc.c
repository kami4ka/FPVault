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
#include "capture.h"

/* Added to the MUSB port: discard a queued, uncollected transfer. */
extern void usbd_ep_flush(uint8_t busid, const uint8_t ep);

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

/* The VideoStreaming interface number, learned at registration. Only that
 * interface's SET_INTERFACE means anything for the stream; the
 * VideoControl one shares this handler. */
static uint8_t vs_intf_num = 0xff;

static volatile uint8_t streaming = 0;
static volatile uint8_t busy = 0;
static uint8_t fid = 0;
/* Set when a session begins; acted on by the pump, never in the IRQ. */
static volatile uint8_t need_flush = 0;

static uint32_t frames_sent = 0, frames_dropped = 0, bytes_sent = 0;
static uint32_t last_sent = 0;
/* TIM0 when the in-flight transfer was queued. TIM0 counts down. */
static uint32_t busy_at = 0;
/* Stall bookkeeping: frames_sent as of the last stall, and how many stalls
 * in a row have passed without it moving. */
static uint32_t stall_mark = 0;
static uint8_t stall_strikes = 0;

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
/* Consecutive stalls with no frame collected in between before the stream
 * is declared dead. Three seconds is far longer than any re-open takes and
 * still well inside a person's patience. */
#define UVC_STALL_STRIKES 3

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

/*
 * UVC 1.1's 34-byte negotiation, not 1.0's 26.
 *
 * The extra fields are the point. bmFramingInfo tells the host that the
 * payload headers carry meaningful frame-id and end-of-frame bits, and for a
 * bulk device that is the only thing marking where one frame stops and the
 * next begins - an isochronous device gets that for free from the packet
 * structure. Without it a host may receive every byte correctly and still
 * have no idea how to cut the stream into frames, which looks exactly like a
 * camera that is connected, streaming, and black.
 */
#define UVC_PROBE_LEN 34u

static uint8_t probe_ctrl[UVC_PROBE_LEN];

/*
 * Fill the negotiated structure from what the board is actually capturing.
 *
 * Two frame descriptors are advertised, 720x480 at 29.97 and 720x576 at 25,
 * because the input can be either and capture_follow_input() switches at
 * runtime. But only one of them is true at any moment, and the board cannot
 * produce the other. So probe answers with the one that matches the live
 * signal rather than with whatever the host asked for. That is exactly what
 * probe negotiation is for: the host proposes, the device answers with what
 * it will really do, and the host is expected to take that answer.
 */
static void probe_defaults(void) {
    int pal = (capture_standard() == VID_PAL);
    uint32_t interval = pal ? UVC_INTERVAL_PAL : UVC_INTERVAL_NTSC;
    uint32_t maxframe = UVC_MAX_FRAME_SIZE;

    for(unsigned i = 0; i < UVC_PROBE_LEN; i++) probe_ctrl[i] = 0;
    probe_ctrl[0] = 0x01; /* bmHint: hold dwFrameInterval */
    probe_ctrl[2] = 0x01; /* bFormatIndex: the only format, MJPEG */
    probe_ctrl[3] = 0x01; /* the only frame; its geometry follows the signal */
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
    /* dwClockFrequency, matching the VideoControl header. */
    probe_ctrl[26] = (uint8_t)(24000000u);
    probe_ctrl[27] = (uint8_t)(24000000u >> 8);
    probe_ctrl[28] = (uint8_t)(24000000u >> 16);
    probe_ctrl[29] = (uint8_t)(24000000u >> 24);
    /* bmFramingInfo bit 0: the payload header's frame id and end-of-frame
     * bits are valid. Bit 1: end-of-slice is not used. */
    probe_ctrl[30] = 0x01;
    probe_ctrl[31] = 0x01; /* bPreferedVersion */
    probe_ctrl[32] = 0x01; /* bMinVersion */
    probe_ctrl[33] = 0x01; /* bMaxVersion */
}

static int uvc_vs_request(uint8_t busid, struct usb_setup_packet* setup,
                          uint8_t** data, uint32_t* len) {
    uint8_t cs = (uint8_t)(setup->wValue >> 8);
    static uint8_t scratch[2];

    (void)busid;
    if(cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) {
        printf("[uvc] unknown control selector %02x, stalling\r\n", (unsigned)cs);
        return -1;
    }

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
        /* What the host proposes is noted and then answered with what the
         * board will really send, which probe_defaults() takes from the live
         * signal. A host that asks for the PAL frame while the input is NTSC
         * would otherwise expect 576 lines and be handed 480. */
        if(*len >= 26) {
            const uint8_t* q = *data;
            uint32_t iv = (uint32_t)q[4] | ((uint32_t)q[5] << 8) |
                          ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
            uint32_t fs = (uint32_t)q[18] | ((uint32_t)q[19] << 8) |
                          ((uint32_t)q[20] << 16) | ((uint32_t)q[21] << 24);
            uint32_t pl = (uint32_t)q[22] | ((uint32_t)q[23] << 8) |
                          ((uint32_t)q[24] << 16) | ((uint32_t)q[25] << 24);
            printf("[uvc] %s: fmt %u frame %u interval %lu frame_sz %lu payload %lu\r\n",
                   cs == UVC_VS_COMMIT_CONTROL ? "commit" : "probe",
                   (unsigned)q[2], (unsigned)q[3], (unsigned long)iv,
                   (unsigned long)fs, (unsigned long)pl);
        }
        probe_defaults();
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
        /*
         * Selecting the streaming interface starts a session, and it is not
         * always preceded by a commit.
         *
         * A host that opens the camera a second time re-selects this
         * interface and clears the endpoint halt rather than negotiating
         * again. Clearing the halt resets the endpoint's data toggle
         * (usbd_ep_clear_stall in the MUSB port) but leaves this driver's
         * state alone, so a transfer queued for the previous session is
         * orphaned: it never completes, busy never clears, and a second
         * later the stall detector concludes the host has gone and shuts
         * the stream off. The host is not gone - it is waiting for the
         * frames that are no longer coming, and shows a blank picture.
         *
         * Google Meet does exactly this, opening once for the preview and
         * again for the call. Applications that open the camera once and
         * keep it - QuickTime, FaceTime, ffmpeg - never reach this path,
         * which is why it survived until a two-open host tried it.
         *
         * So the session is restarted here on the same terms as a commit.
         * The flush is deferred to the pump rather than done here: it
         * touches indexed endpoint registers, and doing that inside the USB
         * interrupt moves the register window out from under the endpoint-0
         * state machine and drops the device off the bus.
         */
        if(intf->bInterfaceNumber == vs_intf_num) {
            need_flush = 1;
            busy = 0;
            fid = 0;
            stall_strikes = 0;
            streaming = 1;
        }
    } else if(event == USBD_EVENT_RESET) {
        streaming = 0;
        busy = 0;
        probe_defaults();
    }
}

/*
 * Write the live signal's geometry into the frame descriptor.
 *
 * Found the hard way. Two frames were advertised, NTSC and PAL, because the
 * input can be either. But the board can only ever send the one it is
 * actually receiving, and a host is entitled to pick any frame offered:
 * QuickTime always asked for the largest, 720x576, then laid out for 576
 * lines and was handed 480. It received every frame at full rate and showed
 * black. ffmpeg only escaped this because it was told -video_size 720x480.
 *
 * So exactly one frame is offered and it is the true one. The descriptor is
 * patched in place rather than built per standard, which keeps every length
 * in it constant - and those lengths are checked against the bytes by
 * tests/host/test_uvcdesc.c.
 *
 * The signal can still change later; capture_follow_input() switches at
 * runtime. That would need a replug to re-advertise, which is worth a note
 * rather than machinery, since the board's mode already works that way.
 */
void usbuvc_apply_standard(uint8_t* desc, uint32_t len) {
    int pal = (capture_standard() == VID_PAL);
    uint16_t h = pal ? 576 : 480;
    uint32_t iv = pal ? UVC_INTERVAL_PAL : UVC_INTERVAL_NTSC;
    uint32_t i;

    for(i = 0; i + 1 < len; i++) {
        /* CS_INTERFACE, VS_FRAME_MJPEG */
        if(desc[i] == 0x1E && desc[i + 1] == 0x24 && desc[i + 2] == 0x07) {
            desc[i + 7] = (uint8_t)(h);
            desc[i + 8] = (uint8_t)(h >> 8);
            desc[i + 21] = (uint8_t)(iv);
            desc[i + 22] = (uint8_t)(iv >> 8);
            desc[i + 23] = (uint8_t)(iv >> 16);
            desc[i + 24] = (uint8_t)(iv >> 24);
            desc[i + 26] = (uint8_t)(iv);
            desc[i + 27] = (uint8_t)(iv >> 8);
            desc[i + 28] = (uint8_t)(iv >> 16);
            desc[i + 29] = (uint8_t)(iv >> 24);
            printf("[uvc] advertising 720x%u at %s\r\n", (unsigned)h,
                   pal ? "25" : "29.97");
            return;
        }
    }
}

void usbuvc_register(void) {
    usbd_add_endpoint(0, &uvc_in_ep);
}

void usbuvc_hook_notify(struct usbd_interface* vc, struct usbd_interface* vs) {
    probe_defaults();
    vs_intf_num = vs->intf_num;
    vc->notify_handler = uvc_notify;
    /* The streaming interface is ours entirely: the class's handler starts
     * on an alternate setting this host never selects. */
    vs->notify_handler = uvc_notify;
    vs->class_interface_handler = uvc_vs_request;
}

void usbd_video_open(uint8_t busid, uint8_t intf) {
    (void)busid;
    (void)intf;
    /* Runs in the USB interrupt, inside the commit control transfer, so it
     * touches no endpoint registers: doing that here moves the controller's
     * indexed-register window out from under the endpoint-0 state machine
     * and the device stops answering control transfers entirely. The host
     * then drops it off the bus, which is exactly what happened. The pump
     * does the flush instead, in the main loop. */
    need_flush = 1;
    busy = 0;
    fid = 0;
    stall_strikes = 0;
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

    /* Start each session from a known-empty endpoint. A frame left queued by
     * a host that closed without warning would otherwise be collected first
     * by the next one, pushing every payload header after it out of place
     * for good: the stream opens black and stays black until replug. */
    if(need_flush) {
        need_flush = 0;
        usbd_ep_flush(0, UVC_IN_EP);
        busy = 0;
    }

    if(busy) {
        if((uint32_t)(busy_at - tim_get_cnt(TIM0)) > UVC_STALL_TICKS) {
            /* The abandoned frame must not be left for the next host to
             * collect: it would arrive before that session's first frame and
             * push every payload header out of place for good. */
            usbd_ep_flush(0, UVC_IN_EP);
            busy = 0;
            /*
             * Retry before concluding the host has gone.
             *
             * A transfer can be orphaned while the host is still very much
             * there - it re-opens the stream and clears the endpoint halt,
             * which resets the data toggle under anything already queued.
             * Latching the stream off on the first such stall turned a
             * recoverable hiccup into a camera that stayed blank until
             * replug. Flushing and trying again costs one frame and fixes
             * it, so give up only when repeated attempts get nowhere.
             *
             * "Nowhere" has to mean no progress, not just elapsed time:
             * frames_sent advancing between stalls means the host is
             * reading, however slowly, and is not gone at all.
             */
            if(frames_sent != stall_mark) {
                stall_mark = frames_sent;
                stall_strikes = 1;
            } else if(++stall_strikes >= UVC_STALL_STRIKES) {
                printf("[uvc] host stopped reading, stream idle after %lu frames\r\n",
                       (unsigned long)frames_sent);
                streaming = 0;
                stall_strikes = 0;
            }
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
