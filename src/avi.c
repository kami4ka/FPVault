/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * avi.c - minimal AVI 1.0 MJPEG muxer (single 'vids'/'MJPG' stream, no
 * audio). See avi.h for the crash-safety model this implements.
 *
 * Container structure follows the classic AVI 1.0 spec (RIFF 'AVI ' with
 * hdrl/avih/strl/strh/strf, movi, idx1); uli/allwinner-bare-metal's
 * h264avi.c (MIT) and s60sc/ESP32-CAM_MJPEG2SD were studied as references
 * for field conventions, but this was written fresh against the spec.
 *
 * The whole pre-movi header has a FIXED layout, 224 bytes:
 *
 *   0   'RIFF'   4  riffsz [PATCH]        8  'AVI '
 *   12  'LIST'   16 hdrlsz=192            20 'hdrl'
 *   24  'avih'   28 cb=56                 32..87  main header
 *                                         (dwTotalFrames at 48 [PATCH])
 *   88  'LIST'   92 strlsz=116            96 'strl'
 *   100 'strh'   104 cb=56                108..163 stream header
 *                                         (dwLength at 140 [PATCH])
 *   164 'strf'   168 cb=40                172..211 BITMAPINFOHEADER
 *   212 'LIST'   216 movisz [PATCH]       220 'movi'
 *   224 first '00dc' chunk (DWORD-aligned by construction)
 *
 * Only the four [PATCH] dwords ever change after avi_start, so refresh and
 * finalize are exactly 4 small seek+write pairs plus one seek back — cheap
 * enough on FatFs to run every second while recording.
 *
 * Freestanding: no libc I/O, no allocation; string.h (memcpy/memset) and
 * stdint.h only. The 64-bit divides below pull in __aeabi_uldivmod from
 * libgcc on ARM926 — they run once per file, in avi_start.
 */
#include <string.h>

#include "avi.h"

#define AVI_HDR_SIZE     224u
#define OFF_RIFFSZ         4u
#define OFF_TOTALFRAMES   48u
#define OFF_STRH_LEN     140u
#define OFF_MOVISZ       216u

#define SUGGESTED_BUF    (256u * 1024u)
#define AVIF_HASINDEX    0x00000010u
#define AVIF_ISINTERLEAVED 0x00000100u
#define AVIIF_KEYFRAME   0x00000010u

/* All multi-byte fields little-endian, spelled out byte by byte so the code
 * is endian- and alignment-agnostic. */
static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t pad4(uint32_t len)
{
    return (4u - (len & 3u)) & 3u;
}

/* Sequential write at the running end; advances a->off. Sets the sticky
 * error and never calls io again once it is set. */
static int wr(avi_t* a, const void* buf, uint32_t len)
{
    int r;
    if (a->error)
        return a->error;
    r = a->io.write(a->io.ctx, buf, len);
    if (r < 0) {
        a->error = r;
        return r;
    }
    a->off += len;
    return 0;
}

/* Random-access header patch; does NOT touch a->off (we seek back once at
 * the end of a patch cycle instead of after every field). */
static int patch32(avi_t* a, uint32_t off, uint32_t val)
{
    uint8_t b[4];
    int r;
    put32(b, val);
    r = a->io.seek(a->io.ctx, off);
    if (r >= 0)
        r = a->io.write(a->io.ctx, b, 4);
    if (r < 0)
        a->error = r;
    return a->error;
}

/* The four running fields + seek back to the running end. */
static int patch_header(avi_t* a, uint32_t riffsz)
{
    int r;
    if (patch32(a, OFF_RIFFSZ, riffsz))
        return a->error;
    if (patch32(a, OFF_TOTALFRAMES, a->frames))
        return a->error;
    if (patch32(a, OFF_STRH_LEN, a->frames))
        return a->error;
    if (patch32(a, OFF_MOVISZ, a->movi_bytes + 4u))
        return a->error;
    r = a->io.seek(a->io.ctx, a->off);
    if (r < 0)
        a->error = r;
    return a->error;
}

/* Stage one idx1 entry in the caller's buffer. Past idx_cap we silently
 * stop: a short index still lets players seek through what it covers, and
 * finalize writes only the staged count. Must run BEFORE frames/movi_bytes
 * are advanced for the chunk. */
static void idx_stage(avi_t* a, uint32_t jpeg_len)
{
    uint8_t* e;
    if (!a->idx || a->frames >= a->idx_cap)
        return;
    e = a->idx + 16u * a->frames;
    memcpy(e, "00dc", 4);
    put32(e + 4, AVIIF_KEYFRAME); /* every MJPEG frame is a keyframe */
    /* dwOffset of the '00dc' fourcc relative to the 'movi' fourcc: the
     * first chunk sits at 4 (the ffmpeg-compatible convention). */
    put32(e + 8, 4u + a->movi_bytes);
    put32(e + 12, jpeg_len);
}

int avi_start(avi_t* a, const avi_io_t* io, uint16_t w, uint16_t h,
              uint32_t rate, uint32_t scale, uint8_t* idx_buf, uint32_t idx_cap)
{
    uint8_t hdr[AVI_HDR_SIZE];
    uint32_t usec, maxbps, flags;
    int r;

    if (!a || !io || !io->write || !io->seek || !rate || !scale)
        return -1;

    memset(a, 0, sizeof(*a));
    a->io = *io;
    a->width = w;
    a->height = h;
    a->rate = rate;
    a->scale = scale;
    a->idx = idx_buf;
    a->idx_cap = idx_buf ? idx_cap : 0;

    usec = (uint32_t)(1000000ull * scale / rate);
    /* Read-ahead hint only; a quarter of the uncompressed byte rate is a
     * generous ceiling for MJPEG. */
    maxbps = (uint32_t)((uint64_t)w * h * 3u * rate / scale / 4u);
    flags = AVIF_ISINTERLEAVED |
            (idx_buf ? AVIF_HASINDEX : 0u); /* promise idx1 only if we can */

    /* Placeholder counts describe a valid EMPTY movie, so a file cut before
     * the first refresh still opens. */
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    put32(hdr + 4, AVI_HDR_SIZE - 8u);       /* riffsz [PATCH] */
    memcpy(hdr + 8, "AVI ", 4);

    memcpy(hdr + 12, "LIST", 4);
    put32(hdr + 16, 192);                    /* 'hdrl'..end of strl */
    memcpy(hdr + 20, "hdrl", 4);

    memcpy(hdr + 24, "avih", 4);
    put32(hdr + 28, 56);
    put32(hdr + 32, usec);                   /* dwMicroSecPerFrame */
    put32(hdr + 36, maxbps);                 /* dwMaxBytesPerSec */
    /* 40: dwPaddingGranularity = 0 */
    put32(hdr + 44, flags);                  /* dwFlags */
    /* 48: dwTotalFrames = 0 [PATCH]; 52: dwInitialFrames = 0 */
    put32(hdr + 56, 1);                      /* dwStreams */
    put32(hdr + 60, SUGGESTED_BUF);
    put32(hdr + 64, w);                      /* dwWidth */
    put32(hdr + 68, h);                      /* dwHeight */
    /* 72..87: dwReserved[4] = 0 */

    memcpy(hdr + 88, "LIST", 4);
    put32(hdr + 92, 116);                    /* 'strl' + strh + strf */
    memcpy(hdr + 96, "strl", 4);

    memcpy(hdr + 100, "strh", 4);
    put32(hdr + 104, 56);
    memcpy(hdr + 108, "vids", 4);            /* fccType */
    memcpy(hdr + 112, "MJPG", 4);            /* fccHandler */
    /* 116: dwFlags = 0; 120/122: wPriority/wLanguage = 0;
     * 124: dwInitialFrames = 0 */
    put32(hdr + 128, scale);                 /* dwScale */
    put32(hdr + 132, rate);                  /* dwRate */
    /* 136: dwStart = 0; 140: dwLength = 0 [PATCH] */
    put32(hdr + 144, SUGGESTED_BUF);
    put32(hdr + 148, 0xFFFFFFFFu);           /* dwQuality = -1 (driver dflt) */
    /* 152: dwSampleSize = 0 (video: one sample = one frame) */
    /* 156..163: rcFrame = {0, 0, w, h} */
    put16(hdr + 160, w);
    put16(hdr + 162, h);

    memcpy(hdr + 164, "strf", 4);
    put32(hdr + 168, 40);
    put32(hdr + 172, 40);                    /* biSize */
    put32(hdr + 176, w);                     /* biWidth */
    put32(hdr + 180, h);                     /* biHeight */
    put16(hdr + 184, 1);                     /* biPlanes */
    put16(hdr + 186, 24);                    /* biBitCount */
    memcpy(hdr + 188, "MJPG", 4);            /* biCompression */
    put32(hdr + 192, (uint32_t)w * h * 3u);  /* biSizeImage (nominal) */
    /* 196..211: XPels/YPels/ClrUsed/ClrImportant = 0 */

    memcpy(hdr + 212, "LIST", 4);
    put32(hdr + 216, 4);                     /* movisz [PATCH]: fcc only */
    memcpy(hdr + 220, "movi", 4);

    r = a->io.seek(a->io.ctx, 0);
    if (r < 0) {
        a->error = r;
        return r;
    }
    return wr(a, hdr, AVI_HDR_SIZE);
}

void avi_fill_chunk_header(uint8_t hdr[8], uint32_t jpeg_len)
{
    memcpy(hdr, "00dc", 4);
    put32(hdr + 4, jpeg_len); /* stored size is the UNPADDED length */
}

uint32_t avi_chunk_total(uint32_t jpeg_len)
{
    return 8u + jpeg_len + pad4(jpeg_len);
}

/* Common accounting once a chunk's bytes are on disk. */
static void chunk_done(avi_t* a, uint32_t jpeg_len)
{
    a->movi_bytes += avi_chunk_total(jpeg_len);
    a->frames++;
    if (jpeg_len > a->max_frame)
        a->max_frame = jpeg_len;
}

int avi_add_frame(avi_t* a, const void* jpeg, uint32_t len)
{
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };
    uint8_t hdr[8];
    uint32_t pad;

    if (a->error)
        return a->error;
    avi_fill_chunk_header(hdr, len);
    idx_stage(a, len);
    if (wr(a, hdr, 8))
        return a->error;
    if (len && wr(a, jpeg, len))
        return a->error;
    pad = pad4(len);
    if (pad && wr(a, zeros, pad))
        return a->error;
    chunk_done(a, len);
    return 0;
}

int avi_add_empty_frame(avi_t* a)
{
    /* len 0 needs no pad; still gets an idx1 entry with dwSize 0 so index
     * position keeps matching frame number. */
    return avi_add_frame(a, (const void*)0, 0);
}

int avi_add_raw(avi_t* a, const void* chunk, uint32_t jpeg_len)
{
    if (a->error)
        return a->error;
    idx_stage(a, jpeg_len);
    /* One write of header+payload+pad. The 0..3 pad bytes come from the
     * caller's buffer, so the caller must keep them zero (a zero-filled or
     * reused-in-place DMA buffer satisfies this for free). */
    if (wr(a, chunk, avi_chunk_total(jpeg_len)))
        return a->error;
    chunk_done(a, jpeg_len);
    return 0;
}

int avi_refresh_header(avi_t* a)
{
    if (a->error)
        return a->error;
    /* riffsz as if the file were closed right now, without idx1. */
    return patch_header(a, AVI_HDR_SIZE - 8u + a->movi_bytes);
}

int avi_finalize(avi_t* a)
{
    uint32_t riffsz;

    if (a->error)
        return a->error;

    riffsz = AVI_HDR_SIZE - 8u + a->movi_bytes;
    if (a->idx) {
        uint8_t ih[8];
        uint32_t n = (a->frames < a->idx_cap) ? a->frames : a->idx_cap;
        memcpy(ih, "idx1", 4);
        put32(ih + 4, n * 16u);
        if (wr(a, ih, 8))
            return a->error;
        if (n && wr(a, a->idx, n * 16u))
            return a->error;
        riffsz += 8u + n * 16u;
    }
    if (patch_header(a, riffsz))
        return a->error;
    if (a->io.sync) {
        int r = a->io.sync(a->io.ctx);
        if (r < 0) {
            a->error = r;
            return r;
        }
    }
    return 0;
}
