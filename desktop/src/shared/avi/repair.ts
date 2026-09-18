/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Rebuilding a clip the power cut short — and joining several into one.
 *
 * This is our own writer rather than an ffmpeg remux, for three reasons:
 *
 *  1. A remux drops the zero-length dropout chunks. avi_add_empty_frame()
 *     exists specifically so wall-clock time stays honest across signal loss
 *     (src/avi.c:274), and ffmpeg will not surface a 0-byte packet as a
 *     frame. Remuxing would silently delete the timing the firmware went out
 *     of its way to record.
 *  2. What we produce is exactly what avi_finalize() would have written, so a
 *     repaired clip is structurally indistinguishable from one the board
 *     closed itself — and that is testable.
 *  3. The recovery path should not depend on a bundled ffmpeg being healthy.
 *
 * The source file is only ever opened read-only. Repair writes to the
 * library copy; the card is never modified.
 */
import { open, rename } from 'node:fs/promises'
import type { FileHandle } from 'node:fs/promises'
import {
  AVIF_HASINDEX,
  AVIF_ISINTERLEAVED,
  AVIIF_KEYFRAME,
  AVI_HDR_SIZE,
  OFF_MOVISZ,
  OFF_RIFFSZ,
  OFF_STRH_LEN,
  OFF_TOTALFRAMES,
  OFF_FLAGS
} from './constants.js'
import { parseAvi, type ParseResult } from './parse.js'

const COPY_BLOCK = 4 * 1024 * 1024

/**
 * A clip cut by a power loss usually ends with the frame that was being
 * written at the time: the chunk header made it to the card but the JPEG
 * behind it did not finish, so it has no EOI marker. Players show it as a
 * torn band and checkavi calls it an error.
 *
 * The firmware's crash-safety guarantee is precisely "you lose the one frame
 * in flight" (docs/ARCHITECTURE.md), so dropping it is what makes a repaired
 * clip match that promise instead of carrying a broken frame forever.
 */
async function lastFrameIsTorn(
  src: FileHandle,
  chunk: { off: number; size: number }
): Promise<boolean> {
  if (chunk.size < 4) return false
  const tail = Buffer.alloc(2)
  await src.read(tail, 0, 2, chunk.off + 8 + chunk.size - 2)
  return !(tail[0] === 0xff && tail[1] === 0xd9) /* JPEG EOI */
}

export interface RepairProgress {
  bytesCopied: number
  bytesTotal: number
  frames: number
}

export interface RepairResult {
  /** Bytes in the finished file. */
  outputBytes: number
  /** Bytes we had to read from the source — the saving over a naive copy. */
  bytesRead: number
  /** Bytes a naive copy would have moved. */
  naiveBytes: number
  frames: number
  drops: number
  /** True when the torn in-flight frame was dropped (the power-cut frame). */
  droppedTornFrame: boolean
}

/** idx1 entries are movi-fourcc-relative, first chunk at 4 (src/avi.c:130). */
function buildIdx1(chunks: { off: number; size: number }[], moviFccOff: number): Buffer {
  const buf = Buffer.alloc(8 + chunks.length * 16)
  buf.write('idx1', 0, 'ascii')
  buf.writeUInt32LE(chunks.length * 16, 4)
  let p = 8
  for (const c of chunks) {
    buf.write('00dc', p, 'ascii')
    buf.writeUInt32LE(AVIIF_KEYFRAME, p + 4) /* every MJPEG frame is a keyframe */
    buf.writeUInt32LE(c.off - moviFccOff, p + 8)
    buf.writeUInt32LE(c.size, p + 12)
    p += 16
  }
  return buf
}

/** The four DWORDs avi_finalize patches, plus the flags it sets last. */
async function patchHeader(
  out: FileHandle,
  finalLen: number,
  frames: number,
  moviBytes: number
): Promise<void> {
  const put = async (off: number, val: number) => {
    const b = Buffer.alloc(4)
    b.writeUInt32LE(val >>> 0, 0)
    await out.write(b, 0, 4, off)
  }
  await put(OFF_RIFFSZ, finalLen - 8)
  await put(OFF_TOTALFRAMES, frames)
  await put(OFF_STRH_LEN, frames)
  await put(OFF_MOVISZ, moviBytes)
  /* Only now may the header promise an index — the same discipline as
   * src/avi.c:322, and the reason crash-cut clips stay playable. */
  await put(OFF_FLAGS, AVIF_ISINTERLEAVED | AVIF_HASINDEX)
}

/** Copy [from, to) from one handle to another. */
async function copyRange(
  src: FileHandle,
  dst: FileHandle,
  from: number,
  to: number,
  onProgress?: (n: number) => void
): Promise<number> {
  const buf = Buffer.allocUnsafe(COPY_BLOCK)
  let pos = from
  let written = 0
  while (pos < to) {
    const want = Math.min(COPY_BLOCK, to - pos)
    const { bytesRead } = await src.read(buf, 0, want, pos)
    if (bytesRead <= 0) break
    await dst.write(buf, 0, bytesRead)
    pos += bytesRead
    written += bytesRead
    onProgress?.(written)
  }
  return written
}

/**
 * Write a sound copy of `srcPath` to `destPath`: real data only, a rebuilt
 * idx1, corrected counts, and the HASINDEX flag set.
 *
 * Reads only what the clip actually contains. For a clip that died ten
 * seconds in, that is ~8 MB rather than the full 200 MB the file occupies.
 */
export async function repairTo(
  srcPath: string,
  destPath: string,
  onProgress?: (p: RepairProgress) => void,
  parsed?: ParseResult
): Promise<RepairResult> {
  const res = parsed ?? (await parseAvi(srcPath))
  if (res.moviFccOff <= 0 || res.chunks.length === 0)
    throw new Error(`nothing recoverable in ${srcPath}`)

  const tmp = `${destPath}.part`
  const src = await open(srcPath, 'r')
  const dst = await open(tmp, 'w')
  try {
    let chunks = res.chunks
    let end = res.walkEnd
    let droppedTornFrame = false

    /* Only a clip that was actually cut can have a frame in flight. */
    const last = chunks[chunks.length - 1]
    if (res.truncated && last && last.size > 0 && (await lastFrameIsTorn(src, last))) {
      chunks = chunks.slice(0, -1)
      end = last.off
      droppedTornFrame = true
    }

    /* Header and every whole frame, verbatim. Geometry, timebase and
     * buffer hints stay exactly as the firmware wrote them. */
    const copied = await copyRange(src, dst, 0, end, (n) =>
      onProgress?.({ bytesCopied: n, bytesTotal: end, frames: chunks.length })
    )

    const idx = buildIdx1(chunks, res.moviFccOff)
    await dst.write(idx, 0, idx.length)

    const finalLen = copied + idx.length
    const moviBytes = end - res.moviFccOff
    await patchHeader(dst, finalLen, chunks.length, moviBytes)
    await dst.sync()
    await dst.close()
    await src.close()
    await rename(tmp, destPath)

    return {
      outputBytes: finalLen,
      bytesRead: copied + res.bytesRead,
      naiveBytes: res.sizeBytes,
      frames: chunks.length,
      drops: res.emptyFrames,
      droppedTornFrame
    }
  } catch (err) {
    await dst.close().catch(() => {})
    await src.close().catch(() => {})
    throw err
  }
}

/**
 * Join a session's segments into one clip, losslessly.
 *
 * Same writer with a loop around it: one header, every segment's movi body in
 * order, one rebuilt index. No decode, no re-mux, disk speed only.
 *
 * idx1 offsets are 32-bit, so the result must stay under 4 GB — roughly 95
 * minutes. Past that the caller should split, or use an MKV export instead.
 */
export const AVI_MAX_BYTES = 0xffff_ffff

export async function concatTo(
  srcPaths: string[],
  destPath: string,
  onProgress?: (p: RepairProgress) => void
): Promise<RepairResult> {
  if (srcPaths.length === 0) throw new Error('nothing to join')

  const parsed: ParseResult[] = []
  for (const p of srcPaths) parsed.push(await parseAvi(p))

  const first = parsed[0]
  if (!first) throw new Error('nothing to join')
  for (const p of parsed.slice(1)) {
    if (p.width !== first.width || p.height !== first.height)
      throw new Error(`${p.path}: ${p.width}x${p.height} does not match ${first.width}x${first.height}`)
    if (p.rate !== first.rate || p.scale !== first.scale)
      throw new Error(`${p.path}: timebase ${p.rate}/${p.scale} does not match ${first.rate}/${first.scale}`)
  }

  const bodyBytes = parsed.reduce((n, p) => n + (p.walkEnd - (p.moviFccOff + 4)), 0)
  const totalFrames = parsed.reduce((n, p) => n + p.chunks.length, 0)
  const projected = AVI_HDR_SIZE + bodyBytes + 8 + totalFrames * 16
  if (projected > AVI_MAX_BYTES)
    throw new Error(
      `joined file would be ${(projected / 1024 ** 3).toFixed(1)} GB; AVI indexes are 32-bit ` +
        `and cap at 4 GB. Split the session or export to MKV instead.`
    )

  const tmp = `${destPath}.part`
  const dst = await open(tmp, 'w')
  try {
    /* Header from the first segment, so the timebase and geometry are the
     * ones the board actually recorded with. */
    const head = await open(first.path, 'r')
    await copyRange(head, dst, 0, AVI_HDR_SIZE)
    await head.close()

    const chunks: { off: number; size: number }[] = []
    let outPos = AVI_HDR_SIZE
    let copiedTotal = 0

    for (const p of parsed) {
      const src = await open(p.path, 'r')
      const bodyStart = p.moviFccOff + 4
      /* Re-base each segment's chunk offsets into the output file. */
      for (const c of p.chunks) chunks.push({ off: outPos + (c.off - bodyStart), size: c.size })
      const n = await copyRange(src, dst, bodyStart, p.walkEnd, (written) =>
        onProgress?.({
          bytesCopied: copiedTotal + written,
          bytesTotal: bodyBytes,
          frames: chunks.length
        })
      )
      await src.close()
      outPos += n
      copiedTotal += n
    }

    const moviFccOff = AVI_HDR_SIZE - 4
    const idx = buildIdx1(chunks, moviFccOff)
    await dst.write(idx, 0, idx.length)

    const finalLen = outPos + idx.length
    await patchHeader(dst, finalLen, chunks.length, outPos - moviFccOff)
    await dst.sync()
    await dst.close()
    await rename(tmp, destPath)

    return {
      outputBytes: finalLen,
      bytesRead: copiedTotal,
      naiveBytes: parsed.reduce((n, p) => n + p.sizeBytes, 0),
      frames: chunks.length,
      drops: parsed.reduce((n, p) => n + p.emptyFrames, 0),
      droppedTornFrame: false
    }
  } catch (err) {
    await dst.close().catch(() => {})
    throw err
  }
}
