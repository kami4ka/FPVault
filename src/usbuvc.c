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
#include <string.h>
#include "dlog.h"
#include "board.h"
#include "usbd_core.h"
#include "usbd_video.h"
#include "usbuvc.h"
#include "pipeline.h"
#include "f1c100s_timer.h"
#include "capture.h"
#include "usbmsc.h"

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
 * Keep-alive: what the host is sent while there is nothing new to send.
 *
 * A stream that goes quiet is not neutral. Watched on a Windows host with
 * the pipeline paused: 3.5 s into the silence it cleared the endpoint halt,
 * which is how Windows stops a bulk stream, and then negotiated a new one.
 * That application restarts itself. One that does not is left showing its
 * last frame until someone closes and reopens it - which is what a thermal
 * camera's shutter was doing to the testers, each time it interrupted the
 * signal for longer than their software was willing to wait.
 *
 * So while the signal is away the last good frame is sent again, ten times
 * a second. The host sees a camera whose picture has stopped moving rather
 * than a camera that has stopped, and needs no restarting when the picture
 * moves again. The frame goes out of the same ring slot it was first sent
 * from: with no new frames the producer is not advancing, so the slot is
 * not going anywhere.
 */
#define UVC_KEEPALIVE_TICKS (TICKS_PER_SEC / 10u)
/* Frames that may arrive after a slot was sent before it stops being safe
 * to send again. The ring holds 40; half of that leaves no doubt. */
#define UVC_KEEPALIVE_MAX_AGE 20u

static uint32_t last_base = 0, last_total = 0; /* last frame queued */
static uint32_t last_queue_at = 0;             /* TIM0 when it was */
static uint32_t seen_since = 0;                /* frames arrived since */
static uint8_t last_valid = 0;
static uint32_t keepalives = 0;
#ifdef FPV_DEBUG_LOG
static uint32_t ka_run = 0;    /* repeats in the episode now running */
static uint32_t flush_run = 0; /* frames flushed uncollected, likewise */
static uint32_t flushes = 0;
static uint32_t refused = 0; /* frames the endpoint would not take, likewise */
/* frames_sent counts every completed transfer, and a flush completes one
 * with nothing in it. These count only what the host actually took. */
static uint32_t delivered = 0;
static uint32_t delivered_at_ms = 0; /* dlog clock, last frame taken */
static uint32_t queued = 0, wrong_height = 0, wrong_run = 0;
static uint32_t refused_total = 0;
static uint32_t frame_max = 0; /* largest frame queued since last line */
extern uint32_t musb_dlog_requests;

/* How long since the host last took a frame, for the lines that mark a
 * host deciding something: "it reopened" means much more next to "and it
 * had taken nothing for 9 s". Into the caller's buffer: this is called from
 * the USB interrupt as well as the main loop. */
static const char* taken_ago(char* b, uint32_t size) {
    if(!delivered) return "never";
    snprintf(b, size, "%lu ms ago", (unsigned long)(dlog_now_ms() - delivered_at_ms));
    return b;
}
#endif

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
#ifdef FPV_DEBUG_LOG
    if(nbytes) {
        delivered++;
        delivered_at_ms = dlog_now_ms();
    }
    /* Flushing completes the transfer too, with nothing in it. Only bytes
     * that left say the host is reading. */
    if(nbytes && flush_run) {
        DLOG("uvc host is collecting frames again, after %lu went uncollected",
             (unsigned long)flush_run);
        flush_run = 0;
    }
#endif
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

/* The frame the descriptor offers: written once, when the board enumerates,
 * and true until the next power-up. */
static uint16_t adv_h = 480;
static uint8_t adv_pal = 0;

uint16_t usbuvc_frame_height(void) {
    return adv_h;
}

/*
 * Fill the negotiated structure from what was advertised.
 *
 * One frame is offered, so there is one answer, and it has to be the
 * advertised one. This used to answer from the live signal instead, and with
 * a PAL camera that meant a frame interval of 40 ms against a descriptor
 * that listed only 33.4: an answer from outside the list the host had been
 * given to choose from. What the host is told here and what it is sent must
 * both come from that list.
 */
static void probe_defaults(void) {
    uint32_t interval = adv_pal ? UVC_INTERVAL_PAL : UVC_INTERVAL_NTSC;
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
        DLOG("uvc host asked for stream control %02x (request %02x), not supported: refused",
             (unsigned)cs, (unsigned)setup->bRequest);
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
        /* What the host proposes is noted and then answered with the one
         * frame on offer, which is also what it will be sent. */
        if(*len >= 26) {
            const uint8_t* q = *data;
            uint32_t iv = (uint32_t)q[4] | ((uint32_t)q[5] << 8) |
                          ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
            uint32_t fs = (uint32_t)q[18] | ((uint32_t)q[19] << 8) |
                          ((uint32_t)q[20] << 16) | ((uint32_t)q[21] << 24);
            uint32_t pl = (uint32_t)q[22] | ((uint32_t)q[23] << 8) |
                          ((uint32_t)q[24] << 16) | ((uint32_t)q[25] << 24);
            DLOG("uvc %s asked: fmt %u frame %u interval %lu frame_sz %lu payload %lu",
                 cs == UVC_VS_COMMIT_CONTROL ? "commit" : "probe", (unsigned)q[2],
                 (unsigned)q[3], (unsigned long)iv, (unsigned long)fs, (unsigned long)pl);
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
#ifdef FPV_DEBUG_LOG
        {
            char ago[24];
            DLOG("uvc host selected interface %u alt %u (stream was %s, host last took a "
                 "frame %s)",
                 (unsigned)intf->bInterfaceNumber, (unsigned)intf->bAlternateSetting,
                 streaming ? "on" : "off", taken_ago(ago, sizeof ago));
        }
#endif
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
#ifdef FPV_DEBUG_LOG
        if(streaming) {
            char ago[24];
            DLOG("uvc stream ended by a bus reset, host last took a frame %s",
                 taken_ago(ago, sizeof ago));
        }
#endif
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
 * The input can still change later, or appear only after the board has
 * enumerated. What was advertised cannot change without a replug, so the
 * frames do instead: from then on they are made to the advertised size,
 * cropped or padded - see the encode start in pipeline.c. main() waits for
 * the input's standard before calling this, so with a camera connected at
 * power-up the two agree and none of that is needed.
 */
void usbuvc_apply_standard(uint8_t* desc, uint32_t len) {
    int pal = (capture_standard() == VID_PAL);
    uint16_t h = pal ? 576 : 480;
    uint32_t iv = pal ? UVC_INTERVAL_PAL : UVC_INTERVAL_NTSC;
    uint32_t i;

    adv_h = h;
    adv_pal = (uint8_t)pal;
    DLOG("uvc advertising 720x%u at %s", (unsigned)h, pal ? "25" : "29.97");

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
    DLOG("uvc streaming on: sending 720x%u at %s, input is %s 720x%u", (unsigned)adv_h,
         adv_pal ? "25" : "29.97", capture_standard() == VID_PAL ? "PAL" : "NTSC",
         (unsigned)capture_height());
}

void usbd_video_close(uint8_t busid, uint8_t intf) {
    (void)busid;
    (void)intf;
    streaming = 0;
    printf("[uvc] streaming off after %lu frames\r\n", (unsigned long)frames_sent);
#ifdef FPV_DEBUG_LOG
    {
        char ago[24];
        DLOG("uvc streaming off, host last took a frame %s", taken_ago(ago, sizeof ago));
    }
#endif
}

/*
 * Is the endpoint free to take a frame? Deals with everything that can be
 * in the way first: a flush owed to a new session, and a transfer the host
 * never collected. Called from the main loop only, never the IRQ.
 */
static int uvc_ready(void) {
    /* Start each session from a known-empty endpoint. A frame left queued by
     * a host that closed without warning would otherwise be collected first
     * by the next one, pushing every payload header after it out of place
     * for good: the stream opens black and stays black until replug. */
    if(need_flush) {
        need_flush = 0;
        usbd_ep_flush(0, UVC_IN_EP);
        busy = 0;
    }
    if(!busy) return 1;

    if((uint32_t)(busy_at - tim_get_cnt(TIM0)) > UVC_STALL_TICKS) {
        /* The abandoned frame must not be left for the next host to
         * collect: it would arrive before that session's first frame and
         * push every payload header out of place for good. */
        usbd_ep_flush(0, UVC_IN_EP);
        busy = 0;
#ifdef FPV_DEBUG_LOG
        /* Once per episode: a camera nobody has open does this every second
         * for as long as it is plugged in. */
        flushes++;
        if(flush_run++ == 0)
            DLOG("uvc host is not collecting frames: one waited 1 s and was "
                 "discarded (host has taken %lu frames, %lu KB)", (unsigned long)delivered,
                 (unsigned long)(bytes_sent / 1024u));
#endif
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
            DLOG("uvc GAVE UP: host collected nothing in three tries, stream off "
                 "after %lu frames", (unsigned long)frames_sent);
            streaming = 0;
            stall_strikes = 0;
        }
    }
    return 0;
}

/* Put the payload header in front of a finished frame and send it. Every
 * frame the host receives is a new frame to the host, a repeated one
 * included, so the frame id toggles on all of them. */
static void uvc_queue(uint32_t slot_base, uint32_t total) {
    uint8_t* slot = (uint8_t*)slot_base;

    slot[UVC_HDR_OFF] = UVC_HDR_LEN;
    slot[UVC_HDR_OFF + 1] = UVC_BM_EOH | UVC_BM_EOF | (fid ? UVC_BM_FID : 0u);
    fid ^= 1u;

    /* Sent straight out of the ring slot. It cannot be overwritten under us:
     * the producer needs all 40 slots to come back around, about 1.3 s,
     * against a transfer measured in tens of milliseconds. */
    busy = 1;
    busy_at = tim_get_cnt(TIM0);
    last_queue_at = busy_at;
#ifdef FPV_DEBUG_LOG
    queued++;
    if(total > frame_max) frame_max = total;
#endif
    if(usbd_ep_start_write(0, UVC_IN_EP, slot + UVC_HDR_OFF, total) != 0) {
        busy = 0;
        frames_dropped++;
#ifdef FPV_DEBUG_LOG
        refused_total++;
        if(refused++ == 0)
            DLOG("uvc endpoint would not take a frame (sent %lu): still holding "
                 "one the host has not collected, or not enabled",
                 (unsigned long)frames_sent);
    } else if(refused) {
        DLOG("uvc endpoint taking frames again after refusing %lu",
             (unsigned long)refused);
        refused = 0;
#endif
    }
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

    if(!streaming) {
        last_valid = 0;
        return;
    }

    /* The last gate: nothing leaves that is not the advertised frame. In
     * USB mode every frame is already encoded to that height, so this only
     * catches the few still in the ring from before the mode was entered. */
    if(pipeline_slot_height(slot_base) != adv_h) {
        frames_dropped++;
#ifdef FPV_DEBUG_LOG
        wrong_height++;
        if(wrong_run++ == 0)
            DLOG("uvc frame of 720x%u held back: the stream is 720x%u",
                 (unsigned)pipeline_slot_height(slot_base), (unsigned)adv_h);
#endif
        return;
    }
#ifdef FPV_DEBUG_LOG
    if(wrong_run) {
        DLOG("uvc frames of the right size again after %lu held back", (unsigned long)wrong_run);
        wrong_run = 0;
    }
#endif
    {
        /* Say so when the input and the stream stop or start agreeing. */
        static uint8_t told = 0xFF;
        uint8_t now = (uint8_t)(capture_standard() == VID_PAL);
        if(now != told) {
            told = now;
            if(now == adv_pal)
                DLOG("uvc input is %s, same as the stream: frames go out as captured",
                     now ? "PAL 720x576" : "NTSC 720x480");
            else
                DLOG("uvc input is %s but the stream is 720x%u: %s", now ? "PAL 720x576"
                                                                           : "NTSC 720x480",
                     (unsigned)adv_h,
                     now ? "sending the middle 480 lines"
                         : "sending it with black below, 5 frames in 6");
        }
    }
    /* No faster than advertised either: 29.97 into a 25 fps stream. */
    if(adv_pal && capture_standard() != VID_PAL) {
        static uint32_t n = 0;
        if((++n % 6u) == 0u) return;
    }
    seen_since++;

    if(!uvc_ready()) {
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

#ifdef FPV_DEBUG_LOG
    if(ka_run) {
        DLOG("uvc live video again after %lu repeated frames", (unsigned long)ka_run);
        ka_run = 0;
    }
#endif
    last_base = slot_base;
    last_total = total;
    last_valid = 1;
    seen_since = 0;
    uvc_queue(slot_base, total);
}

/*
 * Main-loop poll, for the times no frame arrives to drive the pump: keeps
 * the stream alive through a signal dropout, and keeps the stall detector
 * running when nothing else would call it.
 */
void usbuvc_poll(void) {
    if(!streaming || !last_valid) return;
    if((uint32_t)(last_queue_at - tim_get_cnt(TIM0)) < UVC_KEEPALIVE_TICKS) return;
    /* A slot this many frames old may already be the producer's again. */
    if(seen_since >= UVC_KEEPALIVE_MAX_AGE) {
        last_valid = 0;
#ifdef FPV_DEBUG_LOG
        {
            /* Once until the host takes a frame again: a host that is not
             * reading at all brings this about every second. */
            static uint32_t told_at = 0xFFFFFFFFu;
            if(told_at != delivered) {
                told_at = delivered;
                DLOG("uvc stopped repeating the last frame: %lu newer ones arrived and none "
                     "could be sent", (unsigned long)seen_since);
            }
        }
#endif
        return;
    }
    if(!uvc_ready()) return;
#ifdef FPV_DEBUG_LOG
    if(ka_run++ == 0) DLOG("uvc no new frame for 100 ms, repeating the last one");
#endif
    keepalives++;
    uvc_queue(last_base, last_total);
}

#ifdef FPV_DEBUG_LOG
/*
 * One line a second while the camera streams. Read across, it says where a
 * frame stopped on its way out:
 *   in      frames the decoder completed      (0: no signal, or capture stuck)
 *   enc     frames encoded                    (< in: encoder failing or ring full)
 *   q       frames handed to the endpoint     (< enc with skip > 0: host slow)
 *   took    frames the host actually collected (0 while q > 0: host not reading)
 *   kb/max  bytes collected, largest frame queued
 *   skip    frames not queued, endpoint still busy with the one before
 *   flush   frames given up on after waiting 1 s for the host
 *   refuse  frames the endpoint would not accept
 *   rep     repeats of the last frame while no new one came
 *   held    frames of the wrong size held back
 *   nosig   ms of that second without signal lock
 *   ctl     control requests from the host
 *   card    KB the host read / wrote, and its slowest card command in ms
 *   logms   longest the log itself kept USB waiting
 */
void usbuvc_dlog_rate(void) {
    static uint32_t p_in, p_enc, p_q, p_took, p_kb, p_skip, p_flush, p_ref, p_rep, p_held;
    static uint32_t p_nosig, p_ctl, p_rd, p_wr;
    uint32_t in = capture_frames(), enc = pipeline_enc_count(), kb = bytes_sent / 1024u;
    uint32_t nosig = capture_unlocked_ms(), ctl = musb_dlog_requests;
    uint32_t rd, wr, op_ms, hold_ms = dlog_hold_max_ms(), fmax = frame_max;
    uint32_t skip = frames_dropped - refused_total - wrong_height;

    static char prev[200];
    static uint32_t same = 0;
    char line[200];

    usbmsc_dlog_counts(&rd, &wr, &op_ms);
    frame_max = 0;
    if(streaming) {
        snprintf(line, sizeof line,
                 "rate in=%lu enc=%lu q=%lu took=%lu kb=%lu max=%luk skip=%lu flush=%lu "
                 "refuse=%lu rep=%lu held=%lu nosig=%lu ctl=%lu card=%lu/%lu/%lums",
                 (unsigned long)(in - p_in), (unsigned long)(enc - p_enc),
                 (unsigned long)(queued - p_q), (unsigned long)(delivered - p_took),
                 (unsigned long)(kb - p_kb), (unsigned long)(fmax / 1024u),
                 (unsigned long)(skip - p_skip), (unsigned long)(flushes - p_flush),
                 (unsigned long)(refused_total - p_ref), (unsigned long)(keepalives - p_rep),
                 (unsigned long)(wrong_height - p_held), (unsigned long)(nosig - p_nosig),
                 (unsigned long)(ctl - p_ctl), (unsigned long)((rd - p_rd) / 2u),
                 (unsigned long)((wr - p_wr) / 2u), (unsigned long)op_ms);
        /* A stream with nothing happening - no signal, or a host that has
         * the camera selected and is not reading - says the same thing
         * every second. Said once, then counted. The log's own hold time is
         * left out of the comparison: it wobbles by a millisecond. */
        if(strcmp(line, prev) == 0) {
            same++;
        } else {
            if(same) DLOG("rate (the line before held for %lu more s)", (unsigned long)same);
            same = 0;
            DLOG("%s logms=%lu", line, (unsigned long)hold_ms);
            strcpy(prev, line);
        }
    } else if(same) {
        DLOG("rate (the line before held for %lu more s)", (unsigned long)same);
        same = 0;
        prev[0] = 0;
    }
    p_in = in, p_enc = enc, p_q = queued, p_took = delivered, p_kb = kb;
    p_skip = skip, p_flush = flushes, p_ref = refused_total, p_rep = keepalives;
    p_held = wrong_height, p_nosig = nosig, p_ctl = ctl, p_rd = rd, p_wr = wr;
}

void usbuvc_dlog(void) {
    char ago[24];
    DLOG("uvc stream=%u busy=%u taken=%lu queued=%lu uncollected=%lu repeated=%lu kb=%lu "
         "last taken %s",
         streaming, busy, (unsigned long)delivered, (unsigned long)queued,
         (unsigned long)flushes, (unsigned long)keepalives,
         (unsigned long)(bytes_sent / 1024u), taken_ago(ago, sizeof ago));
}
#endif

void usbuvc_stats(void) {
    uint32_t fps;
    if(!streaming) return;
    fps = frames_sent - last_sent;
    last_sent = frames_sent;
    printf("[uvc] %lu fps, %lu sent, %lu dropped, %lu repeated, %lu KB total\r\n",
           (unsigned long)fps, (unsigned long)frames_sent,
           (unsigned long)frames_dropped, (unsigned long)keepalives,
           (unsigned long)(bytes_sent / 1024u));
}
