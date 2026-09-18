/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Running the bundled ffmpeg.
 *
 * Progress comes from `-progress pipe:1 -nostats`, which emits plain
 * key=value lines on stdout. That is far steadier than scraping the
 * human-readable status line off stderr, which reflows and repaints.
 *
 * ffmpeg is only ever used for the optional MP4 export. Import, repair,
 * join and playback are pure TypeScript, so a missing ffmpeg degrades one
 * feature rather than breaking the app.
 */
import { spawn } from 'node:child_process'
import { resolveBin } from './resolveBin.js'

export interface FfmpegProgress {
  /** Frames encoded so far, when ffmpeg reports them. */
  frame: number
  /** Output position in seconds. */
  outTimeSec: number
  /** Encoding rate relative to real time, e.g. 46.6 means 46.6x. */
  speed: number
}

export class FfmpegMissing extends Error {
  constructor() {
    super('ffmpeg was not bundled with this build')
    this.name = 'FfmpegMissing'
  }
}

export async function ffmpegAvailable(): Promise<boolean> {
  return (await resolveBin('ffmpeg')) !== null
}

/**
 * Run ffmpeg to completion. Rejects with the tail of stderr on a non-zero
 * exit, because ffmpeg's last few lines are almost always the real reason.
 */
export function runFfmpeg(
  args: string[],
  onProgress?: (p: FfmpegProgress) => void,
  signal?: AbortSignal
): Promise<void> {
  return new Promise((resolve_, reject) => {
    void resolveBin('ffmpeg').then((bin) => {
      if (!bin) return reject(new FfmpegMissing())

      const child = spawn(bin, ['-hide_banner', '-nostdin', '-progress', 'pipe:1', '-nostats', ...args], {
        stdio: ['ignore', 'pipe', 'pipe']
      })

      const stderr: string[] = []
      let partial = ''
      const current: FfmpegProgress = { frame: 0, outTimeSec: 0, speed: 0 }

      child.stdout.setEncoding('utf8')
      child.stdout.on('data', (chunk: string) => {
        partial += chunk
        const lines = partial.split('\n')
        partial = lines.pop() ?? ''
        for (const line of lines) {
          const [key, value] = line.split('=')
          if (!key || value === undefined) continue
          if (key === 'frame') current.frame = Number(value) || 0
          else if (key === 'out_time_us') current.outTimeSec = (Number(value) || 0) / 1e6
          else if (key === 'speed') current.speed = parseFloat(value) || 0
          else if (key === 'progress') onProgress?.({ ...current })
        }
      })

      child.stderr.setEncoding('utf8')
      child.stderr.on('data', (chunk: string) => {
        stderr.push(chunk)
        if (stderr.length > 40) stderr.shift()
      })

      signal?.addEventListener('abort', () => child.kill('SIGTERM'), { once: true })

      child.on('error', reject)
      child.on('close', (code) => {
        if (code === 0) return resolve_()
        if (signal?.aborted) return reject(new Error('cancelled'))
        const tail = stderr.join('').trim().split('\n').slice(-6).join('\n')
        reject(new Error(tail || `ffmpeg exited with code ${code}`))
      })
    })
  })
}
