/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exporting a session as H.264/MP4, for sharing.
 *
 * The lossless join is the archival copy; this is the one you can actually
 * send someone. MJPEG at D1 runs about 20 KB a frame, so an hour is a couple
 * of gigabytes; the same footage as H.264 is roughly a tenth of that.
 *
 * The input is the concat demuxer over the session's repaired clips. It has
 * to be repaired clips rather than the card's originals: the demuxer needs a
 * real index, and an unfinalised clip would hand ffmpeg 200 MB of old
 * cluster data after the last frame.
 */
import { mkdir, rm, stat, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { runFfmpeg } from '../tools/ffmpeg.js'
import type { JobContext } from './queue.js'
import type { Store } from '../library/store.js'

const MB = 1024 * 1024

export interface ExportResult {
  output: string
  bytes: number
  /** How much smaller than the lossless original, as a ratio. */
  shrink: number
}

export interface ExportOptions {
  deinterlace: boolean
  /** x264 constant rate factor: lower is better and larger. */
  crf: number
  preset: string
}

const DEFAULTS: ExportOptions = { deinterlace: true, crf: 20, preset: 'veryfast' }

export async function exportSessionMp4(
  sessionId: string,
  label: string,
  store: Store,
  ctx: JobContext,
  opts: ExportOptions = DEFAULTS
): Promise<ExportResult> {
  const clips = store.clipsOf(sessionId)
  if (!clips.length) throw new Error('nothing to export')

  const outDir = join(store.root, 'exports')
  await mkdir(outDir, { recursive: true })
  const safe = label.replace(/[^\w.-]+/g, '-').replace(/^-+|-+$/g, '')
  const output = join(outDir, `${safe || sessionId}.mp4`)

  /* The concat demuxer wants a list file. It goes in the library, never on
   * the card, and is removed afterwards. */
  const listPath = join(outDir, `.${sessionId}.concat.txt`)
  const list = clips
    .map((c) => `file '${join(store.root, c.file).replace(/'/g, "'\\''")}'`)
    .join('\n')
  await writeFile(listPath, list + '\n', 'utf8')

  const sourceBytes = clips.reduce((n, c) => n + c.bytes, 0)
  const totalFrames = clips.reduce((n, c) => n + c.frames, 0)
  const first = clips[0]
  const fps = first && first.scale ? first.rate / first.scale : 30000 / 1001

  /* The source is interlaced analog video off the TVD, so deinterlacing is
   * usually right; it stays a choice because a progressive camera would be
   * softened by it. */
  const filters = opts.deinterlace ? ['-vf', 'yadif=mode=0:parity=auto:deint=all'] : []

  const args = [
    '-f', 'concat',
    '-safe', '0',
    '-i', listPath,
    '-map', '0:v',
    '-c:v', 'libx264',
    '-preset', opts.preset,
    '-crf', String(opts.crf),
    '-pix_fmt', 'yuv420p',
    ...filters,
    '-r', String(fps),
    '-movflags', '+faststart',
    ...(clips[0]?.startUtc ? ['-metadata', `creation_time=${clips[0].startUtc}`] : []),
    '-y',
    output
  ]

  const controller = new AbortController()
  try {
    await runFfmpeg(
      args,
      (p) => {
        try {
          ctx.throwIfCancelled()
        } catch (err) {
          controller.abort()
          throw err
        }
        const frac = totalFrames ? Math.min(1, p.frame / totalFrames) : 0
        ctx.report(
          frac,
          `${p.frame} of ${totalFrames} frames · ${p.speed.toFixed(1)}x real time`
        )
      },
      controller.signal
    )
  } finally {
    await rm(listPath, { force: true })
  }

  const { size } = await stat(output)
  const shrink = size ? sourceBytes / size : 0
  ctx.report(1, `${(size / MB).toFixed(0)} MB, ${shrink.toFixed(1)}x smaller than the originals`)

  return { output, bytes: size, shrink }
}
