/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The container contract, ported from src/avi.c. Line references are kept so
 * a firmware change is greppable from here.
 *
 *   0   'RIFF'  4  riffsz [PATCH]        8  'AVI '
 *   12  'LIST'  16 hdrlsz=192            20 'hdrl'
 *   24  'avih'  28 cb=56                 32..87  main header
 *                                        (dwFlags at 44, dwTotalFrames at 48)
 *   88  'LIST'  92 strlsz=116            96 'strl'
 *   100 'strh'  104 cb=56                108..163 stream header
 *                                        (dwLength at 140)
 *   164 'strf'  168 cb=40                172..211 BITMAPINFOHEADER
 *   212 'LIST'  216 movisz [PATCH]        220 'movi'
 *   224 first '00dc' chunk
 */

/** src/avi.c:36-41 */
export const AVI_HDR_SIZE = 224
export const OFF_RIFFSZ = 4
export const OFF_FLAGS = 44
export const OFF_TOTALFRAMES = 48
export const OFF_STRH_LEN = 140
export const OFF_MOVISZ = 216

/** src/avi.c:44-46 */
export const AVIF_HASINDEX = 0x00000010
export const AVIF_ISINTERLEAVED = 0x00000100
export const AVIIF_KEYFRAME = 0x00000010

/** src/recorder.c:65 — f_expand size; a crash-cut clip is exactly this long. */
export const REC_PREALLOC = 209_715_200
/** src/recorder.c:69 — a clip with exactly this many frames ended by rollover. */
export const REC_SEG_FRAMES = 9000
/** src/recorder.c:342 — header refresh cadence, so staleness is bounded by it. */
export const REFRESH_EVERY = 30

/** The only chunk id the recorder ever writes into movi. */
export const FOURCC_00DC = '00dc'
/** Fourccs that may legitimately follow the last frame. */
export const MOVI_FOLLOWERS = new Set(['idx1', 'LIST', 'JUNK'])
