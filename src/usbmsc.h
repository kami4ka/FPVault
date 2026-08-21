/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * usbmsc.h - USB Mass Storage: the SD card appears as a USB disk on any
 * host plugged into the board's USB port (the same port that powers it).
 *
 * Mode policy: the moment a host CONFIGURES the device, the recorder
 * yields - the open clip is finalized, FatFs unmounts, and the host owns
 * the card until the next power cycle (on this board USB *is* the power
 * source, so cable-out means reboot anyway). With no host attached the
 * DVR records normally; enumeration never happens in the air.
 */
#pragma once

#include <stdint.h>

void usbmsc_init(void);        /* bring up the USB device controller */
int usbmsc_host_present(void); /* a host has configured us */
void usbmsc_set_ready(void);   /* recorder released the card - serve data */
void usbmsc_stats(void);
