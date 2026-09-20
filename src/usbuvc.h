/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbuvc.h - UVC video descriptors for the FPVault composite device.
 *
 * Hand-written rather than built from CherryUSB's VIDEO_VC_DESCRIPTOR_INIT,
 * because that macro hardcodes the VideoControl interface number to 0 and so
 * assumes video comes first. Here mass storage is interface 0 and DFU is 1,
 * and neither can move: the desktop app's Windows probe finds DFU by its
 * interface index in the instance id. So video takes interfaces 2 and 3 and
 * the descriptors are written out, the same way src/usbdfu.h does.
 *
 * Entity IDs are not free either. CherryUSB's video class hardcodes them in
 * usbd_video_init_intf(): input terminal 1, processing unit 2, output
 * terminal 3. Matching that is what lets its probe/commit handling work
 * unmodified, so the chain below is IT(1) -> PU(2) -> OT(3) even though this
 * device has no processing controls to offer.
 *
 * Bulk, not isochronous. UVC permits bulk for MJPEG and hosts accept it, and
 * it avoids alternate settings entirely - which matters because the MUSB
 * FIFO allocator never reclaims (usbd_ep_close is a no-op and the running
 * offset only resets on bus reset), so an isochronous design would leak FIFO
 * space on every stream start and stop.
 */
#pragma once

#include <stdint.h>

/* EP1 IN and EP2 OUT belong to mass storage. The F1C100s MUSB has five
 * endpoints - Linux's suniv_f1c100s_musb_cfg selects the 5-endpoint table -
 * so EP3 is free, with two more spare behind it. The part does not implement
 * the capability registers (Linux sets no_configdata), which is why the
 * EPINFO register reads zero and cannot be asked. */
#define UVC_IN_EP 0x83

#ifdef CONFIG_USB_HS
#define UVC_MAX_MPS 512
#else
#define UVC_MAX_MPS 64
#endif

/* The VE's output is bounded in hardware at BSRING_SLOT_SIZE minus
 * BSRING_DATA_OFF; with the 608-byte header and EOI that is the largest
 * frame this device can emit. Hosts size their buffers from it. */
#define UVC_MAX_FRAME_SIZE 262144u

/* 100 ns units. NTSC is 30000/1001 fps exactly, so 10^7 * 1001 / 30000. */
#define UVC_INTERVAL_NTSC 333667u
#define UVC_INTERVAL_PAL  400000u

/* Measured: about 25.5 KB a frame at quality 75, so roughly 6.1 Mbit/s at
 * 29.97 fps. The ceiling allows for detailed scenes well above that. */
#define UVC_MIN_BITRATE 4000000u
#define UVC_MAX_BITRATE 30000000u

/* 8 IAD + 9 VC + 13 VC header + 18 input + 12 processing + 9 output
 * + 9 VS + 14 VS header + 11 format + 30 + 30 frames + 7 endpoint */
#define UVC_DESCRIPTOR_LEN (8 + 9 + 13 + 18 + 12 + 9 + 9 + 14 + 11 + 30 + 30 + 7)

/* Class-specific VC block length, which the VC header must declare:
 * itself + input terminal + processing unit + output terminal. */
#define UVC_VC_TOTAL (13 + 18 + 12 + 9)
/* Class-specific VS block: input header + format + both frames. */
#define UVC_VS_TOTAL (14 + 11 + 30 + 30)

#define UVC_DESCRIPTOR_INIT(vcIntf, vsIntf, strIdx)                            \
    /* Interface Association: interfaces vcIntf and vsIntf are one function */ \
    0x08, 0x0B, (vcIntf), 0x02, 0x0E, 0x03, 0x00, 0x00,                        \
                                                                               \
    /* --- VideoControl interface, no endpoints (control goes over EP0) --- */ \
    0x09, 0x04, (vcIntf), 0x00, 0x00, 0x0E, 0x01, 0x00, (strIdx),              \
                                                                               \
    /* VC header: UVC 1.00, one streaming interface, 24 MHz clock */           \
    0x0D, 0x24, 0x01, WBVAL(0x0100), WBVAL(UVC_VC_TOTAL),                      \
    DBVAL(24000000u), 0x01, (vsIntf),                                          \
                                                                               \
    /* Input terminal 1: a camera. No focal length, no controls. */            \
    0x12, 0x24, 0x02, 0x01, WBVAL(0x0201), 0x00, 0x00,                         \
    WBVAL(0x0000), WBVAL(0x0000), WBVAL(0x0000), 0x03, 0x00, 0x00, 0x00,       \
                                                                               \
    /* Processing unit 2, fed by 1. Present only because CherryUSB's class */  \
    /* expects this entity layout; it offers no controls. */                   \
    0x0C, 0x24, 0x05, 0x02, 0x01, WBVAL(0x0000), 0x02, 0x00, 0x00, 0x00, 0x00, \
                                                                               \
    /* Output terminal 3: the USB stream, fed by 2. */                         \
    0x09, 0x24, 0x03, 0x03, WBVAL(0x0101), 0x00, 0x02, 0x00,                   \
                                                                               \
    /* --- VideoStreaming interface, one bulk IN, a single alt setting --- */  \
    0x09, 0x04, (vsIntf), 0x00, 0x01, 0x0E, 0x02, 0x00, 0x00,                  \
                                                                               \
    /* VS input header: one format, linked to output terminal 3 */             \
    0x0E, 0x24, 0x01, 0x01, WBVAL(UVC_VS_TOTAL), UVC_IN_EP,                    \
    0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00,                                  \
                                                                               \
    /* MJPEG format 1, two frame descriptors, NTSC is the default */           \
    0x0B, 0x24, 0x06, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,          \
                                                                               \
    /* Frame 1: 720x480 at 29.97, one discrete interval */                     \
    0x1E, 0x24, 0x07, 0x01, 0x00, WBVAL(720), WBVAL(480),                      \
    DBVAL(UVC_MIN_BITRATE), DBVAL(UVC_MAX_BITRATE),                            \
    DBVAL(UVC_MAX_FRAME_SIZE), DBVAL(UVC_INTERVAL_NTSC), 0x01,                 \
    DBVAL(UVC_INTERVAL_NTSC),                                                  \
                                                                               \
    /* Frame 2: 720x576 at 25, for a PAL source */                             \
    0x1E, 0x24, 0x07, 0x02, 0x00, WBVAL(720), WBVAL(576),                      \
    DBVAL(UVC_MIN_BITRATE), DBVAL(UVC_MAX_BITRATE),                            \
    DBVAL(UVC_MAX_FRAME_SIZE), DBVAL(UVC_INTERVAL_PAL), 0x01,                  \
    DBVAL(UVC_INTERVAL_PAL),                                                   \
                                                                               \
    /* Bulk IN. bInterval is meaningless for bulk and must be 0. */            \
    0x07, 0x05, UVC_IN_EP, 0x02, WBVAL(UVC_MAX_MPS), 0x00

/* True while a host has the stream open. The class drives this through its
 * usbd_video_open/close callbacks, which src/usbuvc.c implements. */
int usbuvc_streaming(void);
