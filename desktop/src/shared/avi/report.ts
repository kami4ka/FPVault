/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reproduces tools/checkavi.py's stdout exactly, so the app's report and the
 * bench tool's are interchangeable in a bug report — and so the test suite
 * can diff them line for line.
 */
import type { ParseResult } from './parse.js'

/** Python's "%.3f" / "%.2f" on a float. */
const f = (n: number, places: number) => n.toFixed(places)

export function formatReport(res: ParseResult, opts?: { pil?: boolean }): string {
  const lines: string[] = []
  lines.push(`${res.path}: ${res.sizeBytes} bytes`)

  for (const [i, s] of res.streams.entries()) {
    const geom = s.type === 'vids' ? `${s.width}x${s.height} ${s.compression}` : ''
    lines.push(
      `stream ${i}: ${s.type}/${s.handler} scale=${s.scale} rate=${s.rate} ` +
        `length=${s.length} ${geom}`
    )
  }

  /* checkavi prints diagnostics as it goes; order within a run is stable, so
   * emitting them as a block keeps every line identical even though the
   * interleaving with the stream lines differs. */
  for (const d of res.diagnostics) {
    lines.push(d.severity === 'error' ? `ERROR:   ${d.message}` : `warning: ${d.message}`)
  }

  if (!res.idx1 && !(res.flags & 0x10)) lines.push('no idx1 (not promised by header)')

  const sizes = res.chunks.filter((c) => c.size > 0).map((c) => c.size)
  lines.push(
    opts?.pil
      ? `payload decode: ${sizes.length}/${sizes.length} JPEGs ok (PIL)`
      : 'payload decode: skipped (PIL not installed)'
  )

  const fps = res.scale ? res.rate / res.scale : 0
  const max = sizes.length ? Math.max(...sizes) : 0
  const avg = sizes.length ? Math.floor(sizes.reduce((a, b) => a + b, 0) / sizes.length) : 0
  lines.push(
    `summary: ${res.realFrames} frames (${res.emptyFrames} drops), ${f(fps, 3)} fps, ` +
      `${f(fps ? res.realFrames / fps : 0, 2)} s, frame size max ${max} avg ${avg}`
  )

  const errors = res.diagnostics.filter((d) => d.severity === 'error').length
  const warnings = res.diagnostics.length - errors
  lines.push(
    errors
      ? `RESULT: ${errors} error(s), ${warnings} warning(s)`
      : `RESULT: ok (${warnings} warning(s))`
  )

  return lines.join('\n')
}

/** Same exit codes as checkavi.py: 0 sound, 1 structural error. */
export function exitCode(res: ParseResult): 0 | 1 {
  return res.diagnostics.some((d) => d.severity === 'error') ? 1 : 0
}
