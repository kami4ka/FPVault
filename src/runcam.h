/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * runcam.h - device side of the RunCam Device Protocol v1.0, the only
 * record-control protocol Betaflight, INAV and ArduPilot all speak.
 *
 * Wire: UART 115200 8N1. FC -> device frames have NO length byte - the
 * length is implied by the command id:
 *
 *   0xCC 0x00                 CRC   GET_DEVICE_INFO (3 bytes)
 *   0xCC 0x01 <action>        CRC   CAMERA_CONTROL  (4 bytes)
 *   0xCC 0x02 <key>           CRC   5KEY press      (4 bytes, parsed+ignored)
 *   0xCC 0x03                 CRC   5KEY release    (3 bytes, parsed+ignored)
 *   0xCC 0x04 <open/close>    CRC   5KEY connection (4 bytes, parsed+ignored)
 *
 * CRC8 DVB-S2 (poly 0xD5, init 0, no reflection) over EVERY byte including
 * the 0xCC header. Verified reference frames:
 *   toggle (power btn) = CC 01 01 E7
 *   start recording    = CC 01 03 98
 *   stop recording     = CC 01 04 CC
 *
 * GET_DEVICE_INFO reply (5 bytes): CC, protocol=0x01, featLo, featHi, CRC.
 * We advertise 0x00C5 = SIMULATE_POWER_BUTTON | CHANGE_MODE |
 * START_RECORDING | STOP_RECORDING. Bits 6/7 are what make ArduPilot send
 * the explicit 0x03/0x04; Betaflight and INAV only ever send action 0x01
 * (power button) as a record TOGGLE.
 *
 * CAMERA_CONTROL gets NO reply - Betaflight expects zero response bytes for
 * command 0x01 and any reply desyncs its parser.
 *
 * The parser is a pure byte machine (no I/O, no time source) so it unit
 * tests on the host: feed RX bytes, get events, replies come back through
 * the injected tx callback. Call rcam_idle() when the UART has been quiet
 * for >= ~20 ms to abandon a half-received frame (resync after line noise).
 */
#pragma once

#include <stdint.h>

#define RCAM_PROTO_VERSION 0x01
#define RCAM_FEATURES      0x00C5u

typedef enum {
    RCAM_EVT_NONE = 0, /* nothing actionable this byte */
    RCAM_EVT_TOGGLE,   /* action 0x01: toggle recording (BF/INAV path) */
    RCAM_EVT_START,    /* action 0x03: start recording (ArduPilot) */
    RCAM_EVT_STOP,     /* action 0x04: stop recording (ArduPilot) */
} rcam_event_e;

typedef struct rcam {
    void (*tx)(void* ctx, const uint8_t* data, uint8_t len);
    void* tx_ctx;
    uint8_t buf[4];
    uint8_t n;
    /* stats for the console */
    uint32_t rx_frames;
    uint32_t crc_errors;
    uint32_t resyncs;
} rcam_t;

void rcam_init(rcam_t* r, void (*tx)(void* ctx, const uint8_t* d, uint8_t len),
               void* tx_ctx);

/* Feed one received byte; returns an event when a valid frame completes.
 * Replies (device info) are sent through tx from inside this call. */
rcam_event_e rcam_feed(rcam_t* r, uint8_t byte);

/* UART idle gap: abandon any partial frame. */
void rcam_idle(rcam_t* r);

/* Exposed for unit tests and for building replies elsewhere. */
uint8_t rcam_crc8_dvb_s2(uint8_t crc, uint8_t byte);
