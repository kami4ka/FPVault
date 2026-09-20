/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbuvc.c - the USB video interface.
 *
 * There is no mode to switch into. UVC streaming is host-initiated: the
 * class calls usbd_video_open() when something on the computer opens the
 * camera and usbd_video_close() when it lets go, so the board is a camera
 * exactly while a camera is being watched and a card reader the rest of the
 * time. Both at once, since they are separate interfaces on one device.
 *
 * Nothing here produces frames yet. The source already exists and already
 * runs: the pipeline is started at main.c before the host fork and never
 * stopped, so in card-reader mode the VE is encoding JPEGs that are thrown
 * away at recorder.c's early return. Feeding those to the endpoint is the
 * next piece; this file is the half that knows whether anyone is watching.
 */
#include <stdint.h>
#include <stdio.h>
#include "usbd_core.h"
#include "usbd_video.h"
#include "usbuvc.h"

static volatile uint8_t streaming = 0;

int usbuvc_streaming(void) {
    return streaming;
}

/* Called from the USB interrupt when the host commits a format and starts
 * the stream. Keep it to setting the flag: the frame pump runs in the main
 * loop, where a stall cannot cost a capture. */
void usbd_video_open(uint8_t busid, uint8_t intf) {
    (void)busid;
    (void)intf;
    streaming = 1;
    printf("[uvc] host opened the stream\r\n");
}

void usbd_video_close(uint8_t busid, uint8_t intf) {
    (void)busid;
    (void)intf;
    streaming = 0;
    printf("[uvc] host closed the stream\r\n");
}
