/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Synthesises AVI files that match what src/avi.c writes, so the parser can
 * be tested without a board and without committing a 21 MB clip.
 *
 * The crash-cut fixture is created sparse: truncated to the full 200 MB
 * preallocation, but only the header, a few frames and a patch of garbage
 * are actually written. That costs about a megabyte of real disk and runs in
 * milliseconds, while still being exactly REC_PREALLOC bytes on stat() —
 * which is the fingerprint the classifier keys on.
 */
import { open, mkdir } from 'node:fs/promises'
import { dirname } from 'node:path'
import {
  AVIF_HASINDEX,
  AVIF_ISINTERLEAVED,
  AVIIF_KEYFRAME,
  AVI_HDR_SIZE,
  REC_PREALLOC
} from '../../src/shared/avi/constants.js'

/** A minimal but genuinely decodable 1x1 baseline JPEG. */
export const TINY_JPEG = Buffer.from(
  '/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a' +
    'HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA' +
    'AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q==',
  'base64'
)

export interface BuildOptions {
  frames?: number
  /** Indices that should be written as zero-length dropout chunks. */
  dropAt?: number[]
  width?: number
  height?: number
  rate?: number
  scale?: number
  /** Emit idx1 and set HASINDEX, i.e. a cleanly finalized clip. */
  finalize?: boolean
  /** Leave the header's frame counts this many frames behind (power cut). */
  staleBy?: number
  /** Pad the file to the full f_expand size and append garbage. */
  crashCut?: boolean
  /** Write a chunk whose payload runs past EOF. */
  cutInPayload?: boolean
  /** Corrupt the pad bytes after a frame. */
  nonZeroPad?: boolean
  /** Claim HASINDEX without writing an idx1. */
  lieAboutIndex?: boolean
  /** Truncate the last frame's JPEG so it has no EOI, as a power cut does. */
  tornLast?: boolean
}

function header(o: Required<Pick<BuildOptions, 'width' | 'height' | 'rate' | 'scale'>>): Buffer {
  const b = Buffer.alloc(AVI_HDR_SIZE)
  const w = (off: number, s: string) => b.write(s, off, 'ascii')
  const d = (off: number, v: number) => b.writeUInt32LE(v >>> 0, off)

  w(0, 'RIFF'); d(4, AVI_HDR_SIZE - 8); w(8, 'AVI ')
  w(12, 'LIST'); d(16, 192); w(20, 'hdrl')
  w(24, 'avih'); d(28, 56)
  d(32, Math.round((1e6 * o.scale) / o.rate)) /* dwMicroSecPerFrame */
  d(36, Math.floor((o.width * o.height * 3 * o.rate) / o.scale / 4))
  d(44, AVIF_ISINTERLEAVED) /* flags: never HASINDEX while recording */
  d(48, 0) /* dwTotalFrames, patched later */
  d(56, 1) /* dwStreams */
  d(60, 262144) /* dwSuggestedBufferSize */
  d(64, o.width); d(68, o.height)

  w(88, 'LIST'); d(92, 116); w(96, 'strl')
  w(100, 'strh'); d(104, 56)
  w(108, 'vids'); w(112, 'MJPG')
  d(128, o.scale); d(132, o.rate)
  d(140, 0) /* dwLength, patched later */
  d(144, 262144)
  d(148, 0xffffffff) /* dwQuality */
  b.writeUInt16LE(0, 156); b.writeUInt16LE(0, 158)
  b.writeUInt16LE(o.width, 160); b.writeUInt16LE(o.height, 162)

  w(164, 'strf'); d(168, 40)
  d(172, 40); d(176, o.width); d(180, o.height)
  b.writeUInt16LE(1, 184); b.writeUInt16LE(24, 186)
  w(188, 'MJPG'); d(192, o.width * o.height * 3)

  w(212, 'LIST'); d(216, 4); w(220, 'movi')
  return b
}

export async function buildAvi(path: string, opts: BuildOptions = {}): Promise<void> {
  const o = {
    frames: opts.frames ?? 10,
    dropAt: opts.dropAt ?? [],
    width: opts.width ?? 720,
    height: opts.height ?? 480,
    rate: opts.rate ?? 30000,
    scale: opts.scale ?? 1001,
    finalize: opts.finalize ?? false,
    staleBy: opts.staleBy ?? 0,
    crashCut: opts.crashCut ?? false,
    cutInPayload: opts.cutInPayload ?? false,
    nonZeroPad: opts.nonZeroPad ?? false,
    lieAboutIndex: opts.lieAboutIndex ?? false,
    tornLast: opts.tornLast ?? false
  }

  await mkdir(dirname(path), { recursive: true })
  const fh = await open(path, 'w')
  try {
    const head = header(o)
    await fh.write(head, 0, head.length, 0)

    const chunks: { off: number; size: number }[] = []
    let pos = AVI_HDR_SIZE

    for (let i = 0; i < o.frames; i++) {
      const isLast = i === o.frames - 1
      const payload = o.dropAt.includes(i)
        ? Buffer.alloc(0)
        : o.tornLast && isLast
          ? TINY_JPEG.subarray(0, TINY_JPEG.length - 6) /* EOI cut off */
          : TINY_JPEG
      const pad = (4 - (payload.length & 3)) & 3
      const chunk = Buffer.alloc(8 + payload.length + pad)
      chunk.write('00dc', 0, 'ascii')
      chunk.writeUInt32LE(payload.length, 4)
      payload.copy(chunk, 8)
      if (o.nonZeroPad && pad && i === 1) chunk.fill(0xaa, 8 + payload.length)

      const last = i === o.frames - 1
      if (o.cutInPayload && last) {
        /* Claim a payload far larger than what follows, then stop. */
        const cut = Buffer.alloc(8 + 4)
        cut.write('00dc', 0, 'ascii')
        cut.writeUInt32LE(999_999, 4)
        await fh.write(cut, 0, cut.length, pos)
        pos += cut.length
        break
      }

      await fh.write(chunk, 0, chunk.length, pos)
      chunks.push({ off: pos, size: payload.length })
      pos += chunk.length
    }

    const moviFccOff = AVI_HDR_SIZE - 4
    const moviBytes = pos - moviFccOff
    let finalLen = pos

    if (o.finalize) {
      const idx = Buffer.alloc(8 + chunks.length * 16)
      idx.write('idx1', 0, 'ascii')
      idx.writeUInt32LE(chunks.length * 16, 4)
      let p = 8
      for (const c of chunks) {
        idx.write('00dc', p, 'ascii')
        idx.writeUInt32LE(AVIIF_KEYFRAME, p + 4)
        idx.writeUInt32LE(c.off - moviFccOff, p + 8)
        idx.writeUInt32LE(c.size, p + 12)
        p += 16
      }
      await fh.write(idx, 0, idx.length, pos)
      finalLen = pos + idx.length
    }

    if (o.crashCut) {
      /* Garbage right where the next chunk header would be: this is what
       * prior cluster contents look like to the walker. */
      const junk = Buffer.alloc(65536)
      for (let i = 0; i < junk.length; i++) junk[i] = (i * 37 + 11) & 0xff
      await fh.write(junk, 0, junk.length, pos)
      await fh.truncate(REC_PREALLOC) /* sparse: costs almost no real disk */
      finalLen = REC_PREALLOC
    }

    /* Patch the four DWORDs, mimicking the per-second refresh. A stale
     * header simply reports fewer frames than the movi really holds. */
    const reported = Math.max(0, chunks.length - o.staleBy)
    const patch = Buffer.alloc(4)
    const put = async (off: number, v: number) => {
      patch.writeUInt32LE(v >>> 0, 0)
      await fh.write(patch, 0, 4, off)
    }
    await put(4, (o.crashCut ? pos : finalLen) - 8)
    await put(48, reported)
    await put(140, reported)
    await put(216, moviBytes)
    if (o.finalize) await put(44, AVIF_ISINTERLEAVED | AVIF_HASINDEX)
    else if (o.lieAboutIndex) await put(44, AVIF_ISINTERLEAVED | AVIF_HASINDEX)
  } finally {
    await fh.close()
  }
}
