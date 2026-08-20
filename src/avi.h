/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * avi.h - minimal AVI 1.0 muxer for MJPEG, built for an airborne recorder
 * that can lose power at any moment.
 *
 * Crash-safety model:
 *   - the ~310-byte header is written FIRST, with plausible placeholder
 *     counts, so a truncated file is repairable (ffmpeg reads headerless-idx1
 *     MJPEG AVIs fine);
 *   - avi_refresh_header() re-patches the four running fields (RIFF size,
 *     movi size, total frames, stream length) and seeks back; call it about
 *     once a second so a power cut costs at most that interval;
 *   - the idx1 index lives in caller-provided RAM (16 B per chunk) and is
 *     appended only at close. Its loss is tolerable; its absence only breaks
 *     seeking in some players.
 *
 * Every chunk is DWORD-aligned (pad bytes after odd/unaligned JPEG lengths):
 * non-aligned chunks are the #1 cause of "unplayable but not truncated" AVIs.
 *
 * The writer is I/O-agnostic (vtable) so the same code runs against FatFs on
 * the target and plain files in the host unit tests.
 */
#pragma once

#include <stdint.h>

typedef struct avi_io {
    void* ctx;
    /* All return 0 on success, negative on failure. write advances the
     * position; seek is absolute from file start. */
    int (*write)(void* ctx, const void* buf, uint32_t len);
    int (*seek)(void* ctx, uint32_t off);
    int (*sync)(void* ctx);
} avi_io_t;

typedef struct avi {
    avi_io_t io;
    uint16_t width, height;
    uint32_t rate, scale; /* fps = rate/scale, e.g. 30000/1001, 25/1 */
    uint32_t frames;      /* '00dc' chunks written, zero-size ones included */
    uint32_t movi_bytes;  /* payload bytes inside the movi LIST (after 'movi') */
    uint32_t off;         /* current absolute write offset */
    uint8_t* idx;         /* caller-provided idx1 staging */
    uint32_t idx_cap;     /* capacity in entries (16 bytes each) */
    uint32_t max_frame;   /* largest JPEG seen, stat only */
    int      error;       /* first I/O error, sticky */
} avi_t;

/* Writes the full header + 'movi' LIST header; leaves the offset at the
 * first chunk position. idx_buf may be NULL (no index written at close). */
int avi_start(avi_t* a, const avi_io_t* io, uint16_t w, uint16_t h,
              uint32_t rate, uint32_t scale, uint8_t* idx_buf, uint32_t idx_cap);

/* One MJPEG frame. jpeg/len is the raw JPEG (SOI..EOI). Writes the 8-byte
 * '00dc' chunk header, the payload, and 0..3 zero pad bytes. */
int avi_add_frame(avi_t* a, const void* jpeg, uint32_t len);

/* Zero-size '00dc' chunk: the standard way to keep wall-clock time true
 * across dropped frames. */
int avi_add_empty_frame(avi_t* a);

/* For the zero-copy path on target: the caller owns a buffer where the 8
 * chunk-header bytes directly precede the JPEG payload, and writes both with
 * ONE io.write of avi_chunk_total(len) bytes. Fill the header with
 * avi_fill_chunk_header(), write the buffer yourself via avi_add_raw(). */
void avi_fill_chunk_header(uint8_t hdr[8], uint32_t jpeg_len);
uint32_t avi_chunk_total(uint32_t jpeg_len); /* 8 + len + pad */
int avi_add_raw(avi_t* a, const void* chunk, uint32_t jpeg_len);

/* Re-patch the running header fields in place, then return to the end.
 * Call ~1/s. Does NOT sync; call io.sync yourself if wanted. */
int avi_refresh_header(avi_t* a);

/* Append idx1 (if idx_buf was given), patch the header, sync. The avi_t is
 * dead afterwards; the file handle remains the caller's to close. */
int avi_finalize(avi_t* a);
