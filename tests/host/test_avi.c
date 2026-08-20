/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_avi.c - host-side unit test for the AVI 1.0 MJPEG muxer (src/avi.c).
 *
 * The muxer is exercised against an in-memory "file" (growable malloc buffer
 * behind the avi_io_t vtable), then the resulting bytes are checked by a
 * small INDEPENDENT parser written from the RIFF/AVI spec — it shares no
 * constants with avi.c beyond the fixed header layout that is part of the
 * crash-safety contract (patch offsets 4/48/140/216, header size 224).
 *
 * A golden file with real decodable JPEG payloads is also written to disk
 * (test_out.avi) so tools/checkavi.py and, when installed, ffprobe can
 * cross-validate the container against third-party parsers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "avi.h"

/* ------------------------------------------------------------------ */
/* check helpers                                                       */

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } } while (0)

#define CHECKU(got, want) do { \
    uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); \
    if (g_ != w_) { \
        fprintf(stderr, "FAIL %s:%d: %s == %u, want %s == %u\n", \
                __FILE__, __LINE__, #got, g_, #want, w_); \
        exit(1); \
    } } while (0)

/* ------------------------------------------------------------------ */
/* in-memory file behind avi_io_t, with fault injection + call counts  */

typedef struct memfile {
    uint8_t* buf;
    uint32_t pos, len, cap;
    int calls;          /* every io entry (write+seek+sync) */
    int write_no;       /* running write call number, 1-based */
    int fail_write_at;  /* fail this write call number; 0 = never */
} memfile_t;

static int mf_write(void* ctx, const void* p, uint32_t n)
{
    memfile_t* m = (memfile_t*)ctx;
    m->calls++;
    m->write_no++;
    if (m->fail_write_at && m->write_no == m->fail_write_at)
        return -5;
    if (m->pos + n > m->cap) {
        uint32_t ncap = m->cap ? m->cap : 4096;
        while (m->pos + n > ncap) ncap *= 2;
        m->buf = (uint8_t*)realloc(m->buf, ncap);
        CHECK(m->buf != NULL);
        m->cap = ncap;
    }
    memcpy(m->buf + m->pos, p, n);
    m->pos += n;
    if (m->pos > m->len) m->len = m->pos;
    return 0;
}

static int mf_seek(void* ctx, uint32_t off)
{
    memfile_t* m = (memfile_t*)ctx;
    m->calls++;
    m->pos = off;
    return 0;
}

static int mf_sync(void* ctx)
{
    memfile_t* m = (memfile_t*)ctx;
    m->calls++;
    return 0;
}

static void mf_init(memfile_t* m, avi_io_t* io)
{
    memset(m, 0, sizeof(*m));
    io->ctx = m;
    io->write = mf_write;
    io->seek = mf_seek;
    io->sync = mf_sync;
}

/* ------------------------------------------------------------------ */
/* independent little parser                                           */

static uint32_t rd32(const uint8_t* b, uint32_t off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static uint16_t rd16(const uint8_t* b, uint32_t off)
{
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}

static int fcc_is(const uint8_t* b, uint32_t off, const char* f)
{
    return memcmp(b + off, f, 4) == 0;
}

typedef struct chunkinfo {
    uint32_t abs_off; /* absolute offset of the '00dc' fourcc */
    uint32_t size;    /* stored (unpadded) length */
} chunkinfo_t;

typedef struct parsed {
    uint32_t riffsz, usperframe, flags, totalframes, width, height;
    uint32_t scale, rate, strh_len;
    uint32_t movisz;      /* stored movi LIST size */
    uint32_t movi_end;    /* absolute end of movi LIST */
    uint32_t nchunks;
    chunkinfo_t chunk[64];
} parsed_t;

/* Verifies the fixed header layout + walks every movi chunk. `blen` is the
 * file length; the chunk walk must land exactly on movi_end. */
static void parse_avi(const uint8_t* b, uint32_t blen, parsed_t* p)
{
    uint32_t off, end;

    memset(p, 0, sizeof(*p));
    CHECK(blen >= 224);

    CHECK(fcc_is(b, 0, "RIFF"));
    p->riffsz = rd32(b, 4);
    CHECK(fcc_is(b, 8, "AVI "));

    CHECK(fcc_is(b, 12, "LIST"));
    CHECKU(rd32(b, 16), 192);            /* hdrl LIST size, fixed layout */
    CHECK(fcc_is(b, 20, "hdrl"));

    CHECK(fcc_is(b, 24, "avih"));
    CHECKU(rd32(b, 28), 56);
    p->usperframe = rd32(b, 32);
    /* 36 dwMaxBytesPerSec: hint only, no fixed value required */
    CHECKU(rd32(b, 40), 0);              /* dwPaddingGranularity */
    p->flags = rd32(b, 44);
    p->totalframes = rd32(b, 48);
    CHECKU(rd32(b, 52), 0);              /* dwInitialFrames */
    CHECKU(rd32(b, 56), 1);              /* dwStreams */
    CHECKU(rd32(b, 60), 256 * 1024);     /* dwSuggestedBufferSize */
    p->width = rd32(b, 64);
    p->height = rd32(b, 68);

    CHECK(fcc_is(b, 88, "LIST"));
    CHECKU(rd32(b, 92), 116);            /* strl LIST size, fixed layout */
    CHECK(fcc_is(b, 96, "strl"));

    CHECK(fcc_is(b, 100, "strh"));
    CHECKU(rd32(b, 104), 56);
    CHECK(fcc_is(b, 108, "vids"));
    CHECK(fcc_is(b, 112, "MJPG"));
    CHECKU(rd32(b, 116), 0);             /* dwFlags */
    CHECKU(rd32(b, 124), 0);             /* dwInitialFrames */
    p->scale = rd32(b, 128);
    p->rate = rd32(b, 132);
    CHECKU(rd32(b, 136), 0);             /* dwStart */
    p->strh_len = rd32(b, 140);
    CHECKU(rd32(b, 144), 256 * 1024);    /* dwSuggestedBufferSize */
    CHECKU(rd32(b, 148), 0xFFFFFFFFu);   /* dwQuality = -1 */
    CHECKU(rd32(b, 152), 0);             /* dwSampleSize */
    CHECKU(rd16(b, 156), 0);             /* rcFrame.left */
    CHECKU(rd16(b, 158), 0);             /* rcFrame.top */
    CHECKU(rd16(b, 160), p->width);      /* rcFrame.right */
    CHECKU(rd16(b, 162), p->height);     /* rcFrame.bottom */

    CHECK(fcc_is(b, 164, "strf"));
    CHECKU(rd32(b, 168), 40);
    CHECKU(rd32(b, 172), 40);            /* biSize */
    CHECKU(rd32(b, 176), p->width);
    CHECKU(rd32(b, 180), p->height);
    CHECKU(rd16(b, 184), 1);             /* biPlanes */
    CHECKU(rd16(b, 186), 24);            /* biBitCount */
    CHECK(fcc_is(b, 188, "MJPG"));
    CHECKU(rd32(b, 192), p->width * p->height * 3); /* biSizeImage */

    CHECK(fcc_is(b, 212, "LIST"));
    p->movisz = rd32(b, 216);
    CHECK(fcc_is(b, 220, "movi"));

    /* Walk the movi chunks; 220 is where the 'movi' fourcc lives, so the
     * LIST data ends at 220 + movisz. */
    p->movi_end = 220 + p->movisz;
    CHECK(p->movi_end <= blen);
    off = 224;
    end = p->movi_end;
    while (off < end) {
        uint32_t len, pad, i;
        CHECKU(off & 3, 0);              /* every chunk DWORD-aligned */
        CHECK(off + 8 <= end);
        CHECK(fcc_is(b, off, "00dc"));
        len = rd32(b, off + 4);
        pad = (4 - (len & 3)) & 3;
        CHECK(off + 8 + len + pad <= end);
        for (i = 0; i < pad; i++)
            CHECKU(b[off + 8 + len + i], 0); /* pad bytes must be zero */
        CHECK(p->nchunks < 64);
        p->chunk[p->nchunks].abs_off = off;
        p->chunk[p->nchunks].size = len;
        p->nchunks++;
        off += 8 + len + pad;
    }
    CHECKU(off, end); /* movisz consistent with the chunk walk */
}

/* Verifies idx1 right after movi against the independently walked chunks. */
static void parse_idx1(const uint8_t* b, uint32_t blen, const parsed_t* p)
{
    uint32_t off = p->movi_end, i;
    CHECK(off + 8 <= blen);
    CHECK(fcc_is(b, off, "idx1"));
    CHECKU(rd32(b, off + 4), p->nchunks * 16);
    CHECK(off + 8 + p->nchunks * 16 <= blen);
    for (i = 0; i < p->nchunks; i++) {
        uint32_t e = off + 8 + 16 * i;
        CHECK(fcc_is(b, e, "00dc"));
        CHECKU(rd32(b, e + 4), 0x10);    /* AVIF_KEYFRAME */
        /* dwOffset relative to the 'movi' fourcc (at abs 220): first chunk
         * lands at 4 — the ffmpeg-compatible convention. */
        CHECKU(rd32(b, e + 8), p->chunk[i].abs_off - 220);
        CHECKU(rd32(b, e + 12), p->chunk[i].size);
    }
    /* nothing after idx1, and riffsz covers everything */
    CHECKU(blen, off + 8 + p->nchunks * 16);
    CHECKU(p->riffsz, blen - 8);
}

/* ------------------------------------------------------------------ */
/* fake JPEG payloads: SOI .. patterned filler .. EOI                  */

static void make_fake_jpeg(uint8_t* dst, uint32_t len, uint8_t seed)
{
    uint32_t i;
    CHECK(len >= 4);
    dst[0] = 0xFF; dst[1] = 0xD8;
    for (i = 2; i + 2 < len; i++)
        dst[i] = (uint8_t)(seed + i);
    dst[len - 2] = 0xFF; dst[len - 1] = 0xD9;
}

/* real decodable 96x64 baseline JPEG for the golden file */
static const uint8_t tiny_jpeg[922] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x1b, 0x12, 0x14, 0x17, 0x14, 0x11, 0x1b,
    0x17, 0x16, 0x17, 0x1e, 0x1c, 0x1b, 0x20, 0x28, 0x42, 0x2b, 0x28, 0x25, 0x25, 0x28, 0x51, 0x3a,
    0x3d, 0x30, 0x42, 0x60, 0x55, 0x65, 0x64, 0x5f, 0x55, 0x5d, 0x5b, 0x6a, 0x78, 0x99, 0x81, 0x6a,
    0x71, 0x90, 0x73, 0x5b, 0x5d, 0x85, 0xb5, 0x86, 0x90, 0x9e, 0xa3, 0xab, 0xad, 0xab, 0x67, 0x80,
    0xbc, 0xc9, 0xba, 0xa6, 0xc7, 0x99, 0xa8, 0xab, 0xa4, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x1c, 0x1e,
    0x1e, 0x28, 0x23, 0x28, 0x4e, 0x2b, 0x2b, 0x4e, 0xa4, 0x6e, 0x5d, 0x6e, 0xa4, 0xa4, 0xa4, 0xa4,
    0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4,
    0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4,
    0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xa4, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x40, 0x00, 0x60, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
    0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
    0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
    0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xc1,
    0x0b, 0x4e, 0x0b, 0x4f, 0x0b, 0x4e, 0x0b, 0x5d, 0xce, 0x46, 0x71, 0x91, 0x18, 0x5a, 0x70, 0x5a,
    0x90, 0x2d, 0x38, 0x2d, 0x4b, 0x91, 0xb4, 0x64, 0x46, 0x16, 0x9c, 0x16, 0xa4, 0x0b, 0x4e, 0x0b,
    0x52, 0xe4, 0x6f, 0x19, 0x11, 0x85, 0xa7, 0x05, 0xa9, 0x02, 0xd3, 0x82, 0xd4, 0xb9, 0x1b, 0x46,
    0x44, 0x61, 0x69, 0xc1, 0x69, 0xe1, 0x69, 0xc1, 0x6a, 0x5c, 0x8d, 0xa3, 0x21, 0x81, 0x69, 0xc1,
    0x69, 0xe1, 0x69, 0xc1, 0x6a, 0x5c, 0x8d, 0xa3, 0x22, 0x80, 0x5a, 0x70, 0x5a, 0x90, 0x2d, 0x28,
    0x5a, 0xd1, 0xc8, 0xf9, 0x88, 0xc8, 0x60, 0x5a, 0x70, 0x5a, 0x78, 0x5a, 0x70, 0x5a, 0x97, 0x23,
    0x78, 0xc8, 0x8c, 0x2d, 0x38, 0x2d, 0x48, 0x16, 0x9c, 0x16, 0xa5, 0xc8, 0xda, 0x32, 0x23, 0x0b,
    0x4e, 0x0b, 0x52, 0x05, 0xa7, 0x05, 0xa9, 0x72, 0x36, 0x8c, 0x88, 0xc2, 0xd3, 0x82, 0xd4, 0x81,
    0x69, 0xc1, 0x6a, 0x5c, 0x8d, 0xa3, 0x22, 0x30, 0xb4, 0xe0, 0xb5, 0x20, 0x5a, 0x50, 0xb5, 0x2e,
    0x46, 0xd1, 0x91, 0x40, 0x2d, 0x38, 0x2d, 0x48, 0x16, 0x9c, 0x16, 0xb4, 0x72, 0x3e, 0x66, 0x32,
    0x23, 0x0b, 0x4e, 0x0b, 0x52, 0x05, 0xa5, 0x0b, 0x52, 0xe4, 0x6d, 0x19, 0x0c, 0x0b, 0x4e, 0x0b,
    0x4f, 0x0b, 0x4e, 0x0b, 0x52, 0xe4, 0x6d, 0x19, 0x11, 0x85, 0xa7, 0x05, 0xa9, 0x02, 0xd3, 0x82,
    0xd4, 0xb9, 0x1b, 0x46, 0x44, 0x61, 0x69, 0xc1, 0x6a, 0x40, 0xb4, 0xe0, 0xb5, 0x2e, 0x46, 0xd1,
    0x91, 0x18, 0x5a, 0x70, 0x5a, 0x90, 0x2d, 0x38, 0x2d, 0x4b, 0x91, 0xb4, 0x64, 0x67, 0x85, 0xa7,
    0x05, 0xa9, 0x02, 0xd3, 0x82, 0xd6, 0x8e, 0x47, 0xcc, 0xc6, 0x44, 0x61, 0x69, 0xc1, 0x6a, 0x40,
    0xb4, 0xe0, 0xb5, 0x2e, 0x46, 0xd1, 0x91, 0x18, 0x5a, 0x70, 0x5a, 0x90, 0x2d, 0x28, 0x5a, 0x97,
    0x23, 0x68, 0xc8, 0x60, 0x5a, 0x70, 0x5a, 0x78, 0x5a, 0x70, 0x5a, 0x97, 0x23, 0x68, 0xc8, 0x60,
    0x5a, 0x70, 0x5a, 0x78, 0x5a, 0x70, 0x5a, 0x97, 0x23, 0x78, 0xc8, 0x8c, 0x2d, 0x38, 0x2d, 0x48,
    0x16, 0x9c, 0x16, 0xa5, 0xc8, 0xda, 0x32, 0x3f, 0xff, 0xd9,
};

/* ------------------------------------------------------------------ */
/* test 1: full file with 3 frames + 1 drop, verified chunk by chunk   */

static void test_basic(void)
{
    static const uint32_t lens[3] = { 13, 1001, 64 };
    static uint8_t payload[3][1001];
    memfile_t mf;
    avi_io_t io;
    avi_t a;
    uint8_t idx[16 * 16];
    parsed_t p;
    uint32_t i;

    for (i = 0; i < 3; i++)
        make_fake_jpeg(payload[i], lens[i], (uint8_t)(0x11 * (i + 1)));

    mf_init(&mf, &io);
    CHECKU(avi_start(&a, &io, 640, 480, 25, 1, idx, 16), 0);
    CHECKU(a.off, 224); /* fixed header size is part of the contract */
    for (i = 0; i < 3; i++)
        CHECKU(avi_add_frame(&a, payload[i], lens[i]), 0);
    CHECKU(avi_add_empty_frame(&a), 0);
    CHECKU(avi_finalize(&a), 0);
    CHECK(mf.calls > 0);

    parse_avi(mf.buf, mf.len, &p);
    CHECKU(p.usperframe, 40000);
    CHECKU(p.totalframes, 4);
    CHECKU(p.strh_len, 4);
    CHECKU(p.width, 640);
    CHECKU(p.height, 480);
    CHECKU(p.scale, 1);
    CHECKU(p.rate, 25);
    CHECK((p.flags & 0x10) == 0x10); /* AVIF_HASINDEX: idx buffer given */
    CHECKU(p.nchunks, 4);
    for (i = 0; i < 3; i++) {
        CHECKU(p.chunk[i].size, lens[i]);
        CHECK(memcmp(mf.buf + p.chunk[i].abs_off + 8, payload[i], lens[i]) == 0);
    }
    CHECKU(p.chunk[3].size, 0);

    /* hand-computed layout: 13→24, 1001→1012, 64→72, 0→8 chunk bytes */
    CHECKU(p.chunk[0].abs_off, 224);
    CHECKU(p.chunk[1].abs_off, 248);
    CHECKU(p.chunk[2].abs_off, 1260);
    CHECKU(p.chunk[3].abs_off, 1332);
    CHECKU(p.movisz, 4 + 24 + 1012 + 72 + 8);

    parse_idx1(mf.buf, mf.len, &p);
    CHECKU(mf.len, 1340 + 8 + 4 * 16);

    CHECKU(a.frames, 4);
    CHECKU(a.max_frame, 1001);
    free(mf.buf);
}

/* ------------------------------------------------------------------ */
/* test 2: power cut after refresh — snapshot must be self-consistent  */

static void test_refresh_powercut(void)
{
    static uint8_t p1[201], p2[57];
    memfile_t mf;
    avi_io_t io;
    avi_t a;
    uint8_t idx[16 * 16];
    uint8_t* snap;
    uint32_t snap_len;
    parsed_t p;

    make_fake_jpeg(p1, sizeof(p1), 0x21);
    make_fake_jpeg(p2, sizeof(p2), 0x42);

    mf_init(&mf, &io);
    CHECKU(avi_start(&a, &io, 320, 240, 30000, 1001, idx, 16), 0);
    CHECKU(avi_add_frame(&a, p1, sizeof(p1)), 0);
    CHECKU(avi_add_frame(&a, p2, sizeof(p2)), 0);
    CHECKU(avi_refresh_header(&a), 0);

    /* the power cut: keep the bytes as they are on disk right now */
    snap_len = mf.len;
    snap = (uint8_t*)malloc(snap_len);
    CHECK(snap != NULL);
    memcpy(snap, mf.buf, snap_len);

    parse_avi(snap, snap_len, &p);
    CHECKU(p.totalframes, 2);
    CHECKU(p.strh_len, 2);
    CHECKU(p.nchunks, 2);
    CHECKU(p.chunk[0].size, 201);
    CHECKU(p.chunk[1].size, 57);
    /* truncated exactly after the chunks: no idx1, riffsz spans the file */
    CHECKU(p.movi_end, snap_len);
    CHECKU(p.riffsz, snap_len - 8);
    free(snap);

    /* refresh must have seeked back: keep writing, then close for real */
    CHECKU(avi_add_frame(&a, p1, sizeof(p1)), 0);
    CHECKU(avi_finalize(&a), 0);
    parse_avi(mf.buf, mf.len, &p);
    CHECKU(p.totalframes, 3);
    CHECKU(p.nchunks, 3);
    CHECKU(p.chunk[2].size, 201);
    parse_idx1(mf.buf, mf.len, &p);
    free(mf.buf);
}

/* ------------------------------------------------------------------ */
/* test 3: zero-copy path is byte-identical to avi_add_frame           */

static void test_zero_copy(void)
{
    static const uint32_t lens[2] = { 37, 922 };
    const uint8_t* pay[2];
    static uint8_t p0[37];
    memfile_t mfa, mfb;
    avi_io_t ioa, iob;
    avi_t a, b;
    uint8_t idxa[16 * 16], idxb[16 * 16];
    uint32_t i;

    make_fake_jpeg(p0, sizeof(p0), 0x77);
    pay[0] = p0;
    pay[1] = tiny_jpeg;

    mf_init(&mfa, &ioa);
    mf_init(&mfb, &iob);
    CHECKU(avi_start(&a, &ioa, 96, 64, 25, 1, idxa, 16), 0);
    CHECKU(avi_start(&b, &iob, 96, 64, 25, 1, idxb, 16), 0);

    for (i = 0; i < 2; i++) {
        uint8_t raw[8 + 922 + 3];
        CHECKU(avi_chunk_total(lens[i]), 8 + ((lens[i] + 3) & ~3u));
        CHECKU(avi_add_frame(&a, pay[i], lens[i]), 0);
        /* zero-copy contract: pad bytes in the caller's buffer are zero */
        memset(raw, 0, sizeof(raw));
        avi_fill_chunk_header(raw, lens[i]);
        memcpy(raw + 8, pay[i], lens[i]);
        CHECKU(avi_add_raw(&b, raw, lens[i]), 0);
    }
    CHECKU(avi_finalize(&a), 0);
    CHECKU(avi_finalize(&b), 0);

    CHECKU(mfb.len, mfa.len);
    CHECK(memcmp(mfa.buf, mfb.buf, mfa.len) == 0);
    free(mfa.buf);
    free(mfb.buf);
}

/* ------------------------------------------------------------------ */
/* test 4: fps → dwMicroSecPerFrame rounding                           */

static void test_fps_math(void)
{
    memfile_t mf;
    avi_io_t io;
    avi_t a;

    mf_init(&mf, &io);
    CHECKU(avi_start(&a, &io, 640, 480, 30000, 1001, NULL, 0), 0);
    CHECKU(rd32(mf.buf, 32), 33366);  /* 1e6*1001/30000, truncated */
    CHECK((rd32(mf.buf, 44) & 0x10) == 0); /* no idx buffer → no HASINDEX */
    free(mf.buf);

    mf_init(&mf, &io);
    CHECKU(avi_start(&a, &io, 640, 480, 25, 1, NULL, 0), 0);
    CHECKU(rd32(mf.buf, 32), 40000);
    free(mf.buf);
}

/* ------------------------------------------------------------------ */
/* test 5: one io failure → error is sticky, io never touched again    */

static void test_sticky_error(void)
{
    static uint8_t p1[45];
    memfile_t mf;
    avi_io_t io;
    avi_t a;
    uint8_t idx[16 * 16];
    uint8_t raw[8 + 48];
    int err, calls;

    make_fake_jpeg(p1, sizeof(p1), 0x33);
    mf_init(&mf, &io);
    CHECKU(avi_start(&a, &io, 320, 240, 25, 1, idx, 16), 0);
    CHECKU(avi_add_frame(&a, p1, sizeof(p1)), 0);

    mf.fail_write_at = mf.write_no + 2; /* die inside the next add_frame */
    err = avi_add_frame(&a, p1, sizeof(p1));
    CHECK(err < 0);
    CHECKU(a.error, (uint32_t)err);

    calls = mf.calls;
    CHECKU(avi_add_frame(&a, p1, sizeof(p1)), (uint32_t)err);
    CHECKU(avi_add_empty_frame(&a), (uint32_t)err);
    memset(raw, 0, sizeof(raw));
    avi_fill_chunk_header(raw, 45);
    memcpy(raw + 8, p1, 45);
    CHECKU(avi_add_raw(&a, raw, 45), (uint32_t)err);
    CHECKU(avi_refresh_header(&a), (uint32_t)err);
    CHECKU(avi_finalize(&a), (uint32_t)err);
    CHECKU(mf.calls, (uint32_t)calls); /* not a single further io call */
    free(mf.buf);
}

/* ------------------------------------------------------------------ */
/* golden file on disk + optional ffprobe cross-check                  */

static void test_golden_file(void)
{
    memfile_t mf;
    avi_io_t io;
    avi_t a;
    static uint8_t idx[64 * 16];
    parsed_t p;
    uint32_t i;
    FILE* f;

    mf_init(&mf, &io);
    CHECKU(avi_start(&a, &io, 96, 64, 25, 1, idx, 64), 0);
    for (i = 0; i < 25; i++) {
        CHECKU(avi_add_frame(&a, tiny_jpeg, sizeof(tiny_jpeg)), 0);
        if (i == 5 || i == 17)
            CHECKU(avi_add_empty_frame(&a), 0); /* two drops */
        if (i == 12)
            CHECKU(avi_refresh_header(&a), 0);  /* mid-stream patch */
    }
    CHECKU(avi_finalize(&a), 0);

    parse_avi(mf.buf, mf.len, &p);
    CHECKU(p.totalframes, 27);
    CHECKU(p.nchunks, 27);
    parse_idx1(mf.buf, mf.len, &p);

    f = fopen("test_out.avi", "wb");
    CHECK(f != NULL);
    CHECK(fwrite(mf.buf, 1, mf.len, f) == mf.len);
    CHECK(fclose(f) == 0);
    free(mf.buf);

    if (system("which ffprobe >/dev/null 2>&1") == 0) {
        int rc = system("ffprobe -v error test_out.avi");
        CHECK(rc != -1 && WIFEXITED(rc));
        CHECKU(WEXITSTATUS(rc), 0);
        printf("  ffprobe: ok\n");
    } else {
        printf("  ffprobe: not found, skipped\n");
    }
}

int main(void)
{
    test_basic();
    printf("test_basic ok\n");
    test_refresh_powercut();
    printf("test_refresh_powercut ok\n");
    test_zero_copy();
    printf("test_zero_copy ok\n");
    test_fps_math();
    printf("test_fps_math ok\n");
    test_sticky_error();
    printf("test_sticky_error ok\n");
    test_golden_file();
    printf("test_golden_file ok\n");
    printf("test_avi: all tests pass\n");
    return 0;
}
