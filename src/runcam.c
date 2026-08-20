/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * runcam.c - device side of the RunCam Device Protocol v1.0 (see runcam.h
 * for the frame catalogue and verified reference vectors).
 *
 * Design constraints:
 *
 *  - Pure byte machine: no I/O, no timers, no allocation. The only output
 *    paths are the return value of rcam_feed() and the injected tx callback.
 *    Bare-metal safe: stdint/string only.
 *
 *  - FC -> device frames carry no length byte; length is implied by the
 *    command id. An unknown id therefore means we are mis-framed (noise or
 *    a lost byte), never a longer frame we could skip over.
 *
 *  - Resync strategy: on a CRC failure or unknown command we cannot trust
 *    the 0xCC we framed on - it may have been payload of an earlier mangled
 *    frame while the REAL header is sitting later in the buffer. So we drop
 *    exactly one byte and re-scan the remainder as if it just arrived.
 *    Frames are at most 4 bytes, so the loop is trivially bounded.
 *
 *  - CAMERA_CONTROL (0x01) gets NO reply, ever: Betaflight expects zero
 *    response bytes for command 0x01 and any reply desyncs its parser.
 *    Only GET_DEVICE_INFO is answered; 5KEY frames are parsed and counted
 *    but ignored because we do not advertise the 5KEY feature bits.
 */
#include <stdint.h>
#include <string.h>

#include "runcam.h"

#define RCAM_HDR 0xCC

/* CRC8 DVB-S2: poly 0xD5, init 0, no reflection, over EVERY frame byte
 * including the 0xCC header. */
uint8_t rcam_crc8_dvb_s2(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5)
                           : (uint8_t)(crc << 1);
    return crc;
}

/* Total frame length implied by the command id; 0 = unknown command. */
static uint8_t rcam_frame_len(uint8_t cmd)
{
    switch (cmd) {
    case 0x00: return 3; /* GET_DEVICE_INFO */
    case 0x01: return 4; /* CAMERA_CONTROL  */
    case 0x02: return 4; /* 5KEY press      */
    case 0x03: return 3; /* 5KEY release    */
    case 0x04: return 4; /* 5KEY connection */
    default:   return 0;
    }
}

/* Drop the first buffered byte, then re-scan the remainder as if it were
 * newly received: bytes before the next 0xCC are line noise and vanish
 * silently, exactly as they would in rcam_feed() while idle. */
static void rcam_resync(rcam_t* r)
{
    uint8_t i = 1;
    while (i < r->n && r->buf[i] != RCAM_HDR)
        i++;
    uint8_t k = 0;
    while (i < r->n)
        r->buf[k++] = r->buf[i++];
    r->n = k;
    r->resyncs++;
}

/* Answer GET_DEVICE_INFO: CC, protocol, featLo, featHi, CRC. Advertising
 * 0x00C5 (bits 6/7 = explicit start/stop) is what makes ArduPilot send
 * 0x03/0x04 instead of blindly toggling with 0x01. */
static void rcam_send_device_info(rcam_t* r)
{
    uint8_t rep[5];
    rep[0] = RCAM_HDR;
    rep[1] = RCAM_PROTO_VERSION;
    rep[2] = (uint8_t)(RCAM_FEATURES & 0xFF);
    rep[3] = (uint8_t)(RCAM_FEATURES >> 8);
    uint8_t crc = 0;
    for (int i = 0; i < 4; i++)
        crc = rcam_crc8_dvb_s2(crc, rep[i]);
    rep[4] = crc;
    if (r->tx)
        r->tx(r->tx_ctx, rep, sizeof(rep));
}

/* A complete, CRC-valid frame sits in buf[]. Dispatch it. */
static rcam_event_e rcam_dispatch(rcam_t* r)
{
    rcam_event_e ev = RCAM_EVT_NONE;

    switch (r->buf[1]) {
    case 0x00:
        rcam_send_device_info(r);
        break;
    case 0x01:
        /* NO reply here - see file header. Actions other than the three
         * record controls (wifi button 0x00, change mode 0x02, unknown)
         * are parsed and counted but not actionable for a DVR. */
        switch (r->buf[2]) {
        case 0x01: ev = RCAM_EVT_TOGGLE; break;
        case 0x03: ev = RCAM_EVT_START; break;
        case 0x04: ev = RCAM_EVT_STOP; break;
        default: break;
        }
        break;
    default:
        /* 5KEY (0x02/0x03/0x04): we don't advertise the feature, so the
         * FC shouldn't send these; if it does, swallow without reply. */
        break;
    }

    r->rx_frames++;
    r->n = 0;
    return ev;
}

/* Run the framer over whatever is buffered. Loops because a resync may
 * expose another complete frame (an embedded 0xCC header). */
static rcam_event_e rcam_scan(rcam_t* r)
{
    while (r->n >= 2) {
        uint8_t len = rcam_frame_len(r->buf[1]);

        if (len == 0) {
            /* Unknown command id = mis-framed, not a CRC failure. */
            rcam_resync(r);
            continue;
        }
        if (r->n < len)
            return RCAM_EVT_NONE; /* frame still incomplete */

        uint8_t crc = 0;
        for (uint8_t i = 0; i < (uint8_t)(len - 1); i++)
            crc = rcam_crc8_dvb_s2(crc, r->buf[i]);
        if (crc != r->buf[len - 1]) {
            r->crc_errors++;
            rcam_resync(r); /* the real header may be inside the frame */
            continue;
        }

        return rcam_dispatch(r);
    }
    return RCAM_EVT_NONE;
}

void rcam_init(rcam_t* r, void (*tx)(void* ctx, const uint8_t* d, uint8_t len),
               void* tx_ctx)
{
    memset(r, 0, sizeof(*r));
    r->tx = tx;
    r->tx_ctx = tx_ctx;
}

rcam_event_e rcam_feed(rcam_t* r, uint8_t byte)
{
    if (r->n == 0) {
        if (byte != RCAM_HDR)
            return RCAM_EVT_NONE; /* line noise while idle: drop silently */
        r->buf[0] = byte;
        r->n = 1;
        return RCAM_EVT_NONE;
    }

    /* n is bounded by the longest frame (4): rcam_scan() consumes or
     * resyncs as soon as the implied length is reached, so n < 4 here. */
    r->buf[r->n++] = byte;
    return rcam_scan(r);
}

void rcam_idle(rcam_t* r)
{
    /* UART quiet with a partial frame buffered: the rest is never coming
     * (FC frames are back-to-back bytes), so abandon it. */
    if (r->n > 0) {
        r->n = 0;
        r->resyncs++;
    }
}
