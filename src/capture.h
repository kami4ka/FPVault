/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * capture.h - CVBS capture via the TVD, 3-buffer ring, record-only.
 *
 * Extracted from the f1c200-video-board passthru firmware, which measured
 * every rule encoded here (its FINDINGS.md is the reference):
 *
 *   - 3 buffers, planes 4 MB apart (the TVD DMA overruns the Y plane);
 *   - arm-on-completion: the TVD's addr latch waits for a field boundary
 *     and ALWAYS restarts at row 0, so the next buffer is armed when the
 *     current one completes, and the consumer gets the buffer completed one
 *     cycle EARLIER - a lost race then costs a repeated frame, never a torn
 *     one;
 *   - completion is detected by a sentinel row (0x5A stamped across the
 *     last Y row, frame is done when the DMA overwrote it): needs no
 *     undocumented bits. The TVD frame-done IRQ (TVD_DMA_IRQ0, never used
 *     by anyone) is the M3 experiment that may replace it;
 *   - standard auto-follow keys on 625_LINES with V_LOCK, deliberately NOT
 *     H_LOCK (wrong-standard decoding makes H_LOCK marginal -> deadlock),
 *     with a leaky agreement counter.
 *
 * The output format is runtime-selectable: semi-planar 4:2:2 (NV16 layout,
 * the passthru-proven mode) or 4:2:0 (NV12, exactly what the VE encoder's
 * proven input path wants, but untested on the TVD side). M3 decides which
 * end of the pipeline gets the experiment.
 */
#pragma once

#include <stdint.h>

#define CAP_FW 720u

typedef enum { VID_NTSC = 0, VID_PAL = 1 } vid_std_e;
typedef enum { CAP_FMT_422 = 0, CAP_FMT_420 = 1 } cap_fmt_e;

void capture_init(void); /* TVD bring-up, NTSC until auto-follow says otherwise */
void capture_stop(void); /* disable the TVD DMA (diagnostics) */

void capture_set_standard(vid_std_e s);
void capture_set_fmt(cap_fmt_e f); /* re-inits the ring */

/* Frame-done check + ring advance. Call often (main loop or a 1 kHz timer
 * IRQ). Returns 1 when a newly completed frame became available. */
int capture_poll(void);

/* Buffer index of the most recently COMPLETED frame (the only one safe to
 * read), or -1 before the first completion. Valid until the next two
 * capture_poll()==1 events; consumers that lag further see a newer frame. */
int capture_prev(void);
const uint8_t* capture_y(int buf);
const uint8_t* capture_c(int buf);

/* Standard auto-follow; pace at ~50 Hz from a timer (it must keep running
 * with NO signal - that is exactly when it has work to do). */
void capture_follow_input(void);

vid_std_e capture_standard(void);
uint16_t capture_height(void); /* 480 or 576 */
cap_fmt_e capture_fmt(void);
uint32_t capture_state(void); /* raw TVD_STATE_0 */
int capture_signal_ok(void);  /* V_LOCK present, NO_SIGNAL absent */
uint32_t capture_frames(void);
uint32_t capture_std_switches(void);
