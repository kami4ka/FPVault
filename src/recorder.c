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
#include "f1c100s_timer.h"
#include "ff.h"

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

/* ---- state --------------------------------------------------------------- */
static avi_t avi;
static dcf_t dcf;
static uint8_t recording = 0, dcf_ready = 0;
static int staged_quality = -1;
static uint32_t frames_written = 0, drops = 0, wr_us_max = 0;
static uint32_t last_refresh_frame = 0;
static char clip_path[DCF_PATH_MAX];

extern int sdtest_is_mounted(void); /* sdtest owns the mount */

int recorder_active(void) {
    return recording;
}

void recorder_toggle(void) {
    if(recording) {
        avi_finalize(&avi);
        f_close(&clip);
        recording = 0;
        printf("[rec] STOP %s: %lu frames, %lu drops\r\n", clip_path,
               (unsigned long)frames_written, (unsigned long)drops);
        return;
    }
    if(!sdtest_is_mounted()) {
        printf("[rec] mount the card first (:M)\r\n");
        return;
    }
    if(!dcf_ready) {
        int r = dcf_boot_scan(&dcf, &fs_ops);
        if(r != DCF_OK) {
            printf("[rec] dcf scan failed (%d)\r\n", r);
            return;
        }
        dcf_ready = 1;
    }
    if(dcf_next_clip(&dcf, clip_path) != DCF_OK) {
        printf("[rec] no clip slot (card full?)\r\n");
        return;
    }
    if(f_open(&clip, clip_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("[rec] open %s failed\r\n", clip_path);
        return;
    }
    {
        /* NTSC 30000/1001, PAL 25/1. The rate should track what capture
         * actually delivers; nominal standard rate is the v0 answer. */
        uint32_t rate = (capture_standard() == VID_PAL) ? 25 : 30000;
        uint32_t scale = (capture_standard() == VID_PAL) ? 1 : 1001;
        if(avi_start(&avi, &clip_io, CAP_FW, capture_height(), rate, scale,
                     (uint8_t*)IDX_BASE, IDX_SIZE / 16u) != 0) {
            printf("[rec] avi_start failed\r\n");
            f_close(&clip);
            return;
        }
    }
    staged_quality = -1;
    frames_written = 0;
    drops = 0;
    wr_us_max = 0;
    last_refresh_frame = 0;
    recording = 1;
    printf("[rec] RECORDING -> %s\r\n", clip_path);
}

void recorder_on_frame(uint32_t slot_base, uint32_t bitstream_len, int quality) {
    uint8_t* slot = (uint8_t*)slot_base;
    uint32_t jpeg_len, total, t0, us;
    uint32_t pad, i;

    if(!recording) return;

    /* NB: hdr buffer is JPEGTAB_HDR_LEN and capture.h provides the height.
     * Stage the full JPEG header block (SOI..SOS) into the slot. Tables
     * change with quality, geometry with the standard - recompute on
     * change, copy always (605 bytes, ~us; every slot in the rotating
     * ring needs its own copy). */
    {
        static uint8_t hdr[JPEGTAB_HDR_LEN];
        static uint16_t staged_h = 0;
        uint16_t h = capture_height();
        uint32_t i;
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

    /* EOI + zero pad after the hardware bitstream, chunk header up front:
     * one contiguous '00dc' chunk, one f_write. */
    jpeg_len = JPEGTAB_HDR_LEN + bitstream_len + 2u;
    slot[BSRING_DATA_OFF + bitstream_len] = 0xFF;
    slot[BSRING_DATA_OFF + bitstream_len + 1] = 0xD9;
    pad = (4u - (jpeg_len & 3u)) & 3u;
    for(i = 0; i < pad; i++)
        slot[BSRING_DATA_OFF + bitstream_len + 2u + i] = 0;
    avi_fill_chunk_header(slot + BSRING_CHUNK_OFF, jpeg_len);
    total = avi_chunk_total(jpeg_len);

    t0 = tim_get_cnt(TIM0);
    if(avi_add_raw(&avi, slot + BSRING_CHUNK_OFF, jpeg_len) != 0) {
        drops++;
        return;
    }
    us = (uint32_t)(t0 - tim_get_cnt(TIM0)) / 24u;
    if(us > wr_us_max) wr_us_max = us;
    (void)total;
    frames_written++;

    /* Crash safety: refresh the header + sync about once a second. */
    if(frames_written - last_refresh_frame >= 30u) {
        last_refresh_frame = frames_written;
        avi_refresh_header(&avi);
        io_sync(0);
    }
}

void recorder_stats(void) {
    if(!recording) return;
    printf("[rec] %s: %lu frames, %lu drops, wr max %lu us\r\n", clip_path,
           (unsigned long)frames_written, (unsigned long)drops,
           (unsigned long)wr_us_max);
}
