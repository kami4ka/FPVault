/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Joining a session's segments into one file.
 *
 * The lossless path needs no ffmpeg. Every segment shares the same geometry
 * and timebase, and every frame is an independent JPEG, so joining is: one
 * header, each segment's frames copied byte for byte, one rebuilt index.
 * Disk speed, no decode, and the result is structurally what the firmware
 * would have written had it recorded one long clip.
 */
import { mkdir, stat } from 'node:fs/promises'
import { join } from 'node:path'
import { concatTo } from '@shared/avi/repair'
import type { JobContext } from './queue.js'
import type { Store } from '../library/store.js'

const MB = 1024 * 1024

export interface JoinResult {
  output: string
  bytes: number
  frames: number
  clips: number
}

/**
 * Lossless join of every clip in a session, in DCF order.
 *
 * Clips are already repaired on import, so each one carries a valid index
 * and ends at real data — there is no garbage tail to guard against here.
 */
export async function joinSession(
  sessionId: string,
  label: string,
  store: Store,
  ctx: JobContext
): Promise<JoinResult> {
  const clips = store.clipsOf(sessionId)
  if (!clips.length) throw new Error('nothing to join')

  const outDir = join(store.root, 'exports')
  await mkdir(outDir, { recursive: true })

  /* A filename the user can recognise: the session's real date when they
   * have set one, otherwise its number on the card. */
  const safe = label.replace(/[^\w.-]+/g, '-').replace(/^-+|-+$/g, '')
  const output = join(outDir, `${safe || sessionId}.avi`)

  const paths = clips.map((c) => join(store.root, c.file))

  ctx.report(0, `joining ${clips.length} clips`)
  const res = await concatTo(paths, output, (p) => {
    ctx.throwIfCancelled()
    const frac = p.bytesTotal ? p.bytesCopied / p.bytesTotal : 0
    ctx.report(
      frac,
      `${(p.bytesCopied / MB).toFixed(0)} MB of ${(p.bytesTotal / MB).toFixed(0)} MB, ` +
        `${p.frames} frames`
    )
  })

  const { size } = await stat(output)
  ctx.report(1, `${res.frames} frames in one file, ${(size / MB).toFixed(0)} MB`)

  return { output, bytes: size, frames: res.frames, clips: clips.length }
}
