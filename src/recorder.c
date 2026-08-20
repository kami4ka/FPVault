/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * recorder.c - see recorder.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "recorder.h"
#include "avi.h"
#include "dcf.h"
#include "jpegtab.h"
#include "capture.h"
#include "pipeline.h"
#include "f1c100s_timer.h"
#include "ff.h"

extern void sdtest_unmount(void);

/* ---- FatFs glue for the muxer ------------------------------------------- */
static FIL clip;

static int io_write(void* ctx, const void* buf, uint32_t len) {
    UINT bw;
    (void)ctx;
    if(f_write(&clip, buf, len, &bw) != FR_OK || bw != len) return -1;
    return 0;
}
static int io_seek(void* ctx, uint32_t off) {
    (void)ctx;
    return (f_lseek(&clip, off) == FR_OK) ? 0 : -1;
}
static int io_sync(void* ctx) {
    (void)ctx;
    return (f_sync(&clip) == FR_OK) ? 0 : -1;
}
static const avi_io_t clip_io = {0, io_write, io_seek, io_sync};

/* ---- FatFs glue for DCF ------------------------------------------------- */
static int fs_list_dir(void* ctx, const char* path,
                       void (*cb)(void*, const char*, int), void* arg) {
    DIR d;
    FILINFO fi;
    FRESULT fr;
    (void)ctx;
    fr = f_opendir(&d, path);
    if(fr == FR_NO_PATH || fr == FR_NO_FILE) return 0;
    if(fr != FR_OK) return -1;
    while(f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        wdg_feed();
        cb(arg, fi.fname, (fi.fattrib & AM_DIR) ? 1 : 0);
    }
    f_closedir(&d);
    return 0;
}
static int fs_make_dir(void* ctx, const char* path) {
    FRESULT fr = f_mkdir(path);
    (void)ctx;
    return (fr == FR_OK || fr == FR_EXIST) ? 0 : -1;
}
static const dcf_ops_t fs_ops = {0, fs_list_dir, fs_make_dir};

/* 200 MB covers a 5-minute segment with margin at D1 quality-75 rates. */
#define REC_PREALLOC (200u * 1024u * 1024u)
/* Segment length in frames: ~5 min NTSC. Bounds worst-case loss and keeps
 * single files small enough to handle; a new segment opens seamlessly (the
 * bitstream ring absorbs the close/open gap). */
#define REC_SEG_FRAMES 9000u

/* ---- state --------------------------------------------------------------- */
typedef enum {
    REC_NO_CARD = 0,  /* no mounted card - retry mount periodically */
    REC_WAIT_SIGNAL,  /* armed: start once the video signal is stable */
    REC_RECORDING,
    REC_MANUAL_STOP,  /* user said stop - no auto restart until told */
    REC_ERROR,        /* fell over; retries the card after a pause */
} rec_state_e;

static avi_t avi;
static dcf_t dcf;
static rec_state_e state = REC_NO_CARD;
static uint8_t auto_record = 1;
static uint8_t clip_open = 0, dcf_ready = 0;
static int staged_quality = -1;
static uint32_t frames_written = 0, drops = 0, wr_us_max = 0;
static uint32_t last_refresh_frame = 0;
static uint32_t seg_count = 0;
static char clip_path[DCF_PATH_MAX];
static uint32_t t_state, t_mount_try, t_last_frame, t_signal_edge;

extern int sdtest_is_mounted(void);
extern void sdtest_mount(void);

static uint32_t now(void) {
    return tim_get_cnt(TIM0); /* down-counter */
}
static uint32_t since_us(uint32_t t) {
    return (uint32_t)(t - tim_get_cnt(TIM0)) / 24u;
}
static uint32_t frame_period_ticks(void) {
    /* NTSC 30000/1001 fps -> 800800 ticks; PAL 25 -> 960000 */
    return (capture_standard() == VID_PAL) ? 960000u : 800800u;
}

int recorder_active(void) {
    return state == REC_RECORDING;
}

static void clip_stop(void) {
    if(!clip_open) return;
    avi_finalize(&avi);
    f_truncate(&clip);
    f_close(&clip);
    clip_open = 0;
    printf("[rec] closed %s: %lu frames, %lu drops\r\n", clip_path,
           (unsigned long)frames_written, (unsigned long)drops);
}

static int clip_start(void) {
    if(!dcf_ready) {
        if(dcf_boot_scan(&dcf, &fs_ops) != DCF_OK) return -1;
        dcf_ready = 1;
    }
    if(dcf_next_clip(&dcf, clip_path) != DCF_OK) return -1;
    if(f_open(&clip, clip_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    /* Preallocate + sync ONCE: the directory entry is final before the
     * first frame, so no periodic f_sync is needed while recording
     * (f_sync under the IRQ pipeline hard-crashes - debt list - and this
     * is crash-safer anyway). Header refresh flushes via its seek-away. */
    if(f_expand(&clip, REC_PREALLOC, 1) != FR_OK)
        printf("[rec] prealloc failed - recording without it\r\n");
    if(f_sync(&clip) != FR_OK) {
        f_close(&clip);
        return -1;
    }
    {
        uint32_t rate = (capture_standard() == VID_PAL) ? 25 : 30000;
        uint32_t scale = (capture_standard() == VID_PAL) ? 1 : 1001;
        if(avi_start(&avi, &clip_io, CAP_FW, capture_height(), rate, scale,
                     (uint8_t*)IDX_BASE, IDX_SIZE / 16u) != 0) {
            f_close(&clip);
            return -1;
        }
    }
    staged_quality = -1;
    frames_written = 0;
    drops = 0;
    wr_us_max = 0;
    last_refresh_frame = 0;
    t_last_frame = now();
    clip_open = 1;
    printf("[rec] RECORDING -> %s\r\n", clip_path);
    return 0;
}

static void enter(rec_state_e s) {
    state = s;
    t_state = now();
}

void recorder_toggle(void) {
    if(state == REC_RECORDING) {
        clip_stop();
        enter(REC_MANUAL_STOP);
        printf("[rec] manual stop (auto-record paused; :R to re-arm)\r\n");
    } else {
        /* manual start / re-arm */
        if(!sdtest_is_mounted()) {
            printf("[rec] no card mounted yet\r\n");
            enter(REC_NO_CARD);
            return;
        }
        if(clip_start() == 0)
            enter(REC_RECORDING);
        else
            enter(REC_ERROR);
    }
}

void recorder_toggle_auto(void) {
    auto_record ^= 1;
    printf("[rec] auto-record %s\r\n", auto_record ? "ON" : "off");
}

/* The state machine; call every main-loop pass (cheap). */
void recorder_task(void) {
    switch(state) {
    case REC_NO_CARD:
        if(sdtest_is_mounted()) {
            enter(REC_WAIT_SIGNAL);
            break;
        }
        if(since_us(t_mount_try) > 2000000u) {
            t_mount_try = now();
            sdtest_mount(); /* prints its own result */
            if(sdtest_is_mounted()) enter(REC_WAIT_SIGNAL);
        }
        break;

    case REC_WAIT_SIGNAL:
        if(!auto_record) break;
        if(!pipeline_active() || !capture_signal_ok()) {
            t_signal_edge = now();
            break;
        }
        /* one second of stable signal = start */
        if(since_us(t_signal_edge) > 1000000u) {
            if(clip_start() == 0)
                enter(REC_RECORDING);
            else
                enter(REC_ERROR);
        }
        break;

    case REC_RECORDING:
        if(avi.error) {
            /* Write path died (card yanked, card full): close what we can
             * and go retry the card from scratch. */
            clip_stop();
            sdtest_unmount();
            enter(REC_NO_CARD);
            break;
        }
        if(!capture_signal_ok()) {
            /* Keep wall-clock true across dropouts with empty frames; a
             * sustained loss closes the clip. */
            while((uint32_t)(t_last_frame - tim_get_cnt(TIM0)) >
                  frame_period_ticks()) {
                avi_add_empty_frame(&avi);
                frames_written++;
                t_last_frame -= frame_period_ticks();
            }
            if(since_us(t_signal_edge) > 5000000u) {
                clip_stop();
                enter(REC_WAIT_SIGNAL);
            }
        } else {
            t_signal_edge = now();
        }
        break;

    case REC_MANUAL_STOP:
        break;

    case REC_ERROR:
        if(since_us(t_state) > 5000000u) {
            sdtest_unmount();
            enter(REC_NO_CARD);
        }
        break;
    }
}

/* LED pattern byte per state, one bit per 125 ms, MSB first. */
uint8_t recorder_led_pattern(void) {
    switch(state) {
    case REC_RECORDING: return 0xFF;   /* solid */
    case REC_WAIT_SIGNAL: return 0xF0; /* slow blink */
    case REC_MANUAL_STOP: return 0xC0;
    case REC_NO_CARD: return 0xA0; /* double blink */
    case REC_ERROR: return 0xAA;   /* fast blink */
    }
    return 0;
}

void recorder_on_frame(uint32_t slot_base, uint32_t bitstream_len, int quality) {
    uint8_t* slot = (uint8_t*)slot_base;
    uint32_t jpeg_len, t0, us;
    uint32_t pad, i;

    if(state != REC_RECORDING || !clip_open) return;

    /* Segment rollover: close and open seamlessly; the bitstream ring
     * absorbs the gap. Every segment gets a fresh DCF index. */
    if(frames_written >= REC_SEG_FRAMES) {
        seg_count++;
        clip_stop();
        if(clip_start() != 0) {
            enter(REC_ERROR);
            return;
        }
    }

    /* Stage the full JPEG header block (SOI..SOS). Recompute on quality or
     * geometry change, copy always (608 B; each rotating slot needs it). */
    {
        static uint8_t hdr[JPEGTAB_HDR_LEN];
        static uint16_t staged_h = 0;
        uint16_t h = capture_height();
        if(quality != staged_quality || h != staged_h) {
            uint16_t qY[64], qC[64];
            jpegtab_quant(quality, qY, qC);
            jpegtab_headers(hdr, qY, qC, CAP_FW, h, 1);
            staged_quality = quality;
            staged_h = h;
        }
        for(i = 0; i < JPEGTAB_HDR_LEN; i++)
            slot[BSRING_PREFIX_OFF + i] = hdr[i];
    }

    jpeg_len = JPEGTAB_HDR_LEN + bitstream_len + 2u;
    slot[BSRING_DATA_OFF + bitstream_len] = 0xFF;
    slot[BSRING_DATA_OFF + bitstream_len + 1] = 0xD9;
    pad = (4u - (jpeg_len & 3u)) & 3u;
    for(i = 0; i < pad; i++)
        slot[BSRING_DATA_OFF + bitstream_len + 2u + i] = 0;
    avi_fill_chunk_header(slot + BSRING_CHUNK_OFF, jpeg_len);

    t0 = now();
    if(avi_add_raw(&avi, slot + BSRING_CHUNK_OFF, jpeg_len) != 0) {
        drops++;
        return;
    }
    us = since_us(t0);
    if(us > wr_us_max) wr_us_max = us;
    frames_written++;
    t_last_frame = now();

    if(frames_written - last_refresh_frame >= 30u) {
        last_refresh_frame = frames_written;
        avi_refresh_header(&avi);
    }
}

void recorder_stats(void) {
    static const char* names[] = {"NO_CARD", "WAIT_SIGNAL", "RECORDING",
                                  "MANUAL_STOP", "ERROR"};
    if(state == REC_RECORDING)
        printf("[rec] %s: %lu frames, %lu drops, wr max %lu us, seg %lu\r\n",
               clip_path, (unsigned long)frames_written, (unsigned long)drops,
               (unsigned long)wr_us_max, (unsigned long)seg_count);
    else if(state != REC_MANUAL_STOP)
        printf("[rec] %s%s\r\n", names[state], auto_record ? " (auto)" : "");
}
