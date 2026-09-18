/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Card to library in a single sequential pass.
 *
 * The obvious implementation — parse, then copy — reads the file twice, and
 * on a crash-cut clip the parse alone costs one small read per frame. Over
 * the board's USB link that is latency-bound: a 130 MB clip with 6085 frames
 * needs 6085 round trips, which measured at 17 s before a single byte was
 * copied.
 *
 * This fuses the two. One forward scan reads 4 MiB at a time, finds the
 * '00dc' headers inside each block, writes the bytes straight through to the
 * destination, and stops at the first fourcc that is not a frame — which on
 * a crash-cut clip is the old cluster data after the last write. The index
 * is accumulated as we go and appended at the end.
 *
 * So a 200 MB file costs one pass over the ~130 MB that is real, instead of
 * a 200 MB copy or a two-pass read. Nothing is ever written to the card: the
 * source handle is opened read-only.
 */
import { open, rename } from 'node:fs/promises'
import { createHash } from 'node:crypto'
import {
  AVIF_HASINDEX,
  AVIF_ISINTERLEAVED,
  AVIIF_KEYFRAME,
  AVI_HDR_SIZE,
  FOURCC_00DC,
  MOVI_FOLLOWERS,
  OFF_FLAGS,
  OFF_MOVISZ,
  OFF_RIFFSZ,
  OFF_STRH_LEN,
  OFF_TOTALFRAMES,
  REC_PREALLOC
} from './constants.js'
import type { ClipHealth } from '../types.js'

const BLOCK = 4 * 1024 * 1024
/* One bitstream slot, src/board.h BSRING_SLOT_SIZE: a frame cannot exceed it. */
const MAX_FRAME_BYTES = 0x40000

export interface IngestProgress {
  bytesRead: number
  /** What a naive copy would have moved, for the "instead of" line. */
  bytesTotal: number
  frames: number
}

export interface IngestResult {
  outputBytes: number
  /** Bytes actually pulled off the card. */
  bytesRead: number
  /** Size of the file on the card. */
  sourceBytes: number
  frames: number
  drops: number
  droppedTornFrame: boolean
  health: ClipHealth
  /** sha256 of the bytes written, so a later verify needs no re-read. */
  sha256: string
  width: number
  height: number
  rate: number
  scale: number
  durationSec: number
}

/** Header fields we need while streaming, read from the first block. */
function readHeaderFacts(head: Buffer) {
  return {
    flags: head.readUInt32LE(OFF_FLAGS),
    totalFrames: head.readUInt32LE(OFF_TOTALFRAMES),
    width: head.readUInt32LE(176),
    height: head.readUInt32LE(180),
    scale: head.readUInt32LE(128),
    rate: head.readUInt32LE(132)
  }
}

/**
 * Copy `srcPath` into `destPath`, keeping only real frames and giving the
 * result a correct index. Returns what it found on the way.
 */
export async function ingestClip(
  srcPath: string,
  destPath: string,
  onProgress?: (p: IngestProgress) => void
): Promise<IngestResult> {
  const src = await open(srcPath, 'r')
  const tmp = `${destPath}.part`
  /* 'w+' not 'w': the torn-frame check and the verify pass both read the
   * destination back before it is renamed into place. */
  const dst = await open(tmp, 'w+')

  try {
    const { size: sourceBytes } = await src.stat()

    /* The 224-byte header first: geometry, timebase and buffer hints stay
     * exactly as the firmware wrote them. */
    const head = Buffer.alloc(AVI_HDR_SIZE)
    const { bytesRead: headRead } = await src.read(head, 0, AVI_HDR_SIZE, 0)
    if (headRead < AVI_HDR_SIZE) throw new Error(`${srcPath}: too short to be a clip`)
    if (head.toString('ascii', 0, 4) !== 'RIFF' || head.toString('ascii', 8, 12) !== 'AVI ')
      throw new Error(`${srcPath}: not a RIFF/AVI file`)

    const facts = readHeaderFacts(head)
    const moviFccOff = AVI_HDR_SIZE - 4

    await dst.write(head, 0, AVI_HDR_SIZE)

    const chunks: { off: number; size: number }[] = []
    const buf = Buffer.allocUnsafe(BLOCK)

    /* srcPos: next byte to read from the card.
     * outPos: bytes written to the destination so far, which is also the
     *         offset the next chunk will occupy in the output file.
     * pending: bytes read but not yet confirmed as a whole chunk — a chunk
     *         header or payload can straddle a block boundary. */
    let srcPos = AVI_HDR_SIZE
    let outPos = AVI_HDR_SIZE
    let bytesRead = AVI_HDR_SIZE
    let pending = Buffer.alloc(0)
    let truncated = false

    for (;;) {
      const { bytesRead: n } = await src.read(buf, 0, BLOCK, srcPos)
      srcPos += n
      bytesRead += n

      const block = pending.length
        ? Buffer.concat([pending, buf.subarray(0, n)])
        : Buffer.from(buf.subarray(0, n))

      let p = 0
      let stop = false
      while (p + 8 <= block.length) {
        const fcc = block.toString('latin1', p, p + 4)
        if (fcc !== FOURCC_00DC) {
          /* 'idx1', 'LIST' and 'JUNK' legitimately follow the last frame of
           * a clip the firmware closed itself. Anything else is old cluster
           * data, which is where a crash-cut clip really ends. */
          if (!MOVI_FOLLOWERS.has(fcc)) truncated = true
          stop = true
          break
        }
        const size = block.readUInt32LE(p + 4)
        const whole = 8 + size + ((4 - (size & 3)) & 3)

        if (size > MAX_FRAME_BYTES) {
          /* A frame can never exceed one bitstream slot (src/board.h), so a
           * larger size field is garbage that happened to follow '00dc'. */
          truncated = true
          stop = true
          break
        }
        if (p + whole > block.length) break /* need the next block first */

        chunks.push({ off: outPos + p, size })
        p += whole
      }

      if (p > 0) {
        await dst.write(block, 0, p)
        outPos += p
      }
      pending = Buffer.from(block.subarray(p))

      onProgress?.({ bytesRead, bytesTotal: sourceBytes, frames: chunks.length })

      if (stop) break
      if (n === 0) {
        /* End of file with bytes left over: the last chunk never completed. */
        if (pending.length) truncated = true
        break
      }
    }

    /* Re-base offsets: they were accumulated against the output file, which
     * is what idx1 must describe. */
    let end = outPos
    let droppedTornFrame = false

    /* A clip cut mid-write ends with a JPEG that never got its EOI. The
     * firmware's guarantee is that you lose exactly that frame. */
    const last = chunks[chunks.length - 1]
    if (truncated && last && last.size >= 2) {
      const tail = Buffer.alloc(2)
      await dst.read(tail, 0, 2, last.off + 8 + last.size - 2)
      if (!(tail[0] === 0xff && tail[1] === 0xd9)) {
        chunks.pop()
        end = last.off
        droppedTornFrame = true
        await dst.truncate(end)
      }
    }

    const idx = Buffer.alloc(8 + chunks.length * 16)
    idx.write('idx1', 0, 'ascii')
    idx.writeUInt32LE(chunks.length * 16, 4)
    let q = 8
    for (const c of chunks) {
      idx.write('00dc', q, 'ascii')
      idx.writeUInt32LE(AVIIF_KEYFRAME, q + 4)
      idx.writeUInt32LE(c.off - moviFccOff, q + 8)
      idx.writeUInt32LE(c.size, q + 12)
      q += 16
    }
    await dst.write(idx, 0, idx.length, end)

    const finalLen = end + idx.length
    const put = async (off: number, v: number) => {
      const b = Buffer.alloc(4)
      b.writeUInt32LE(v >>> 0, 0)
      await dst.write(b, 0, 4, off)
    }
    await put(OFF_RIFFSZ, finalLen - 8)
    await put(OFF_TOTALFRAMES, chunks.length)
    await put(OFF_STRH_LEN, chunks.length)
    await put(OFF_MOVISZ, end - moviFccOff)
    await put(OFF_FLAGS, AVIF_ISINTERLEAVED | AVIF_HASINDEX)

    await dst.sync()

    /* Digest the file as it finally stands. Reading it back also proves it
     * is readable, which is the cheapest possible verify. */
    const verify = createHash('sha256')
    const vbuf = Buffer.allocUnsafe(BLOCK)
    for (let pos = 0; pos < finalLen; ) {
      const { bytesRead: vn } = await dst.read(vbuf, 0, Math.min(BLOCK, finalLen - pos), pos)
      if (vn <= 0) break
      verify.update(vbuf.subarray(0, vn))
      pos += vn
    }

    await dst.close()
    await src.close()
    await rename(tmp, destPath)

    const fps = facts.scale ? facts.rate / facts.scale : 0
    /* A finalized clip is always f_truncate'd, and a full 9000-frame segment
     * is larger than the 200 MB preallocation, so a file of exactly that size
     * can only be one that was never closed. */
    const health: ClipHealth = truncated || sourceBytes === REC_PREALLOC ? 'crashCut' : 'clean'

    return {
      outputBytes: finalLen,
      bytesRead,
      sourceBytes,
      frames: chunks.length,
      drops: chunks.filter((c) => c.size === 0).length,
      droppedTornFrame,
      health,
      sha256: verify.digest('hex'),
      width: facts.width,
      height: facts.height,
      rate: facts.rate,
      scale: facts.scale,
      durationSec: fps ? chunks.length / fps : 0
    }
  } catch (err) {
    await dst.close().catch(() => {})
    await src.close().catch(() => {})
    throw err
  }
}
