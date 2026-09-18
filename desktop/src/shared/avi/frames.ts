/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pulling individual frames out of a clip.
 *
 * Every '00dc' payload the firmware writes is a complete JPEG — SOI, tables,
 * the VE's entropy scan and EOI, assembled per frame in src/avi.c. So there
 * is nothing to decode at the container level: seek to the offset the index
 * gives, read that many bytes, and you have an image the browser can display
 * directly.
 *
 * That matters because Chromium has no AVI demuxer, so handing a clip to a
 * <video> element does not work. Reading frames ourselves sidesteps the
 * problem entirely, gives frame-accurate scrubbing for free, and plays
 * crash-cut clips that other players refuse to open.
 *
 * Imported clips always carry an idx1, so the seek table is one read rather
 * than a walk of the whole file.
 */
import { open } from 'node:fs/promises'
import type { FileHandle } from 'node:fs/promises'
import { AVI_HDR_SIZE, FOURCC_00DC } from './constants.js'

export interface FrameTable {
  /** Byte offset of each frame's JPEG payload, and its length. */
  frames: { offset: number; size: number }[]
  width: number
  height: number
  rate: number
  scale: number
}

/** Read the idx1 of a finalized clip into a seek table. */
async function tableFromIndex(fh: FileHandle, size: number): Promise<FrameTable | null> {
  const head = Buffer.alloc(AVI_HDR_SIZE)
  await fh.read(head, 0, AVI_HDR_SIZE, 0)
  if (head.toString('ascii', 0, 4) !== 'RIFF') return null

  const moviFccOff = AVI_HDR_SIZE - 4
  const moviSize = head.readUInt32LE(216)
  /* idx1 sits immediately after the movi LIST. */
  const idxOff = moviFccOff + moviSize
  if (idxOff + 8 > size) return null

  const idxHead = Buffer.alloc(8)
  await fh.read(idxHead, 0, 8, idxOff)
  if (idxHead.toString('ascii', 0, 4) !== 'idx1') return null

  const idxBytes = idxHead.readUInt32LE(4)
  const count = Math.floor(Math.min(idxBytes, size - idxOff - 8) / 16)
  if (count <= 0) return null

  const idx = Buffer.alloc(count * 16)
  await fh.read(idx, 0, idx.length, idxOff + 8)

  const frames: { offset: number; size: number }[] = []
  for (let i = 0; i < count; i++) {
    const e = i * 16
    if (idx.toString('ascii', e, e + 4) !== FOURCC_00DC) return null
    /* Offsets are movi-fourcc-relative; +8 skips the chunk header. */
    frames.push({
      offset: moviFccOff + idx.readUInt32LE(e + 8) + 8,
      size: idx.readUInt32LE(e + 12)
    })
  }

  return {
    frames,
    width: head.readUInt32LE(176),
    height: head.readUInt32LE(180),
    scale: head.readUInt32LE(128),
    rate: head.readUInt32LE(132)
  }
}

/** Walk the chunks, for a clip with no usable index. */
async function tableByWalk(fh: FileHandle, size: number): Promise<FrameTable | null> {
  const head = Buffer.alloc(AVI_HDR_SIZE)
  await fh.read(head, 0, AVI_HDR_SIZE, 0)
  if (head.toString('ascii', 0, 4) !== 'RIFF') return null

  const frames: { offset: number; size: number }[] = []
  const buf = Buffer.allocUnsafe(4 * 1024 * 1024)
  let pos = AVI_HDR_SIZE
  let pending = Buffer.alloc(0)

  for (;;) {
    const { bytesRead: n } = await fh.read(buf, 0, buf.length, pos)
    const block = pending.length ? Buffer.concat([pending, buf.subarray(0, n)]) : Buffer.from(buf.subarray(0, n))
    const blockStart = pos - pending.length
    let p = 0
    let stop = false

    while (p + 8 <= block.length) {
      if (block.toString('latin1', p, p + 4) !== FOURCC_00DC) {
        stop = true
        break
      }
      const sz = block.readUInt32LE(p + 4)
      const whole = 8 + sz + ((4 - (sz & 3)) & 3)
      if (p + whole > block.length) break
      frames.push({ offset: blockStart + p + 8, size: sz })
      p += whole
    }

    pending = Buffer.from(block.subarray(p))
    pos = blockStart + p + pending.length
    if (stop || n === 0) break
  }

  return {
    frames,
    width: head.readUInt32LE(176),
    height: head.readUInt32LE(180),
    scale: head.readUInt32LE(128),
    rate: head.readUInt32LE(132)
  }
}

export async function frameTable(path: string): Promise<FrameTable | null> {
  const fh = await open(path, 'r')
  try {
    const { size } = await fh.stat()
    return (await tableFromIndex(fh, size)) ?? (await tableByWalk(fh, size))
  } finally {
    await fh.close()
  }
}

/**
 * The JPEG bytes of one frame. A zero-length result means a dropout chunk:
 * the firmware records those so wall-clock time stays honest, and the player
 * should hold the previous image rather than show nothing.
 */
export async function readFrame(
  path: string,
  table: FrameTable,
  index: number
): Promise<Buffer> {
  const f = table.frames[index]
  if (!f || f.size === 0) return Buffer.alloc(0)
  const fh = await open(path, 'r')
  try {
    const buf = Buffer.allocUnsafe(f.size)
    const { bytesRead } = await fh.read(buf, 0, f.size, f.offset)
    return buf.subarray(0, bytesRead)
  } finally {
    await fh.close()
  }
}

/**
 * A representative frame for a thumbnail. Frame 0 often catches the camera's
 * gain still settling, so a little way in makes a better picture.
 */
export function posterIndex(total: number): number {
  return total > 60 ? 45 : Math.max(0, Math.floor(total / 3))
}
