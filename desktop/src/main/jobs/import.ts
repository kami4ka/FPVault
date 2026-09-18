/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Importing a session from the card into the library.
 *
 * The card is opened read-only throughout — repair happens on the copy, and
 * nothing here ever writes to the removable volume.
 */
import { mkdir } from 'node:fs/promises'
import { join } from 'node:path'
import { randomUUID } from 'node:crypto'
import { ingestClip } from '@shared/avi/ingest'
import { clipFileName } from '@shared/dcf'
import type { CardSession } from '../library/scanner.js'
import type { JobContext } from './queue.js'
import type { LibraryClip, Store } from '../library/store.js'

const MB = 1024 * 1024

export interface ImportSummary {
  imported: number
  skipped: number
  bytesRead: number
  bytesWritten: number
  bytesOnCard: number
  reclaimed: number
}

function sessionIdFor(dcfDir: number): string {
  return `s${String(dcfDir).padStart(3, '0')}`
}

export async function importSession(
  session: CardSession,
  store: Store,
  ctx: JobContext
): Promise<ImportSummary> {
  const sessionId = sessionIdFor(session.dcfDir)
  const destDir = join(store.root, 'sessions', sessionId)
  await mkdir(destDir, { recursive: true })

  const summary: ImportSummary = {
    imported: 0,
    skipped: 0,
    bytesRead: 0,
    bytesWritten: 0,
    bytesOnCard: 0,
    reclaimed: 0
  }

  for (const [i, clip] of session.clips.entries()) {
    ctx.throwIfCancelled()

    if (store.has(session.dcfDir, clip.dcfIndex, clip.bytes)) {
      summary.skipped++
      continue
    }

    const dest = join(destDir, clipFileName(clip.dcfIndex))
    const base = i / session.clips.length
    const span = 1 / session.clips.length

    const res = await ingestClip(clip.path, dest, (p) => {
      ctx.throwIfCancelled()
      /* For an unfinalised clip the total is the 200 MB preallocation, so
       * the bar would crawl to 6% and jump. Show bytes read instead, and say
       * what the file claims to be — that contrast is the story. */
      const within = p.bytesTotal ? Math.min(1, p.bytesRead / p.bytesTotal) : 0
      ctx.report(
        base + within * span,
        `${clip.name}: read ${(p.bytesRead / MB).toFixed(0)} MB of a ` +
          `${(p.bytesTotal / MB).toFixed(0)} MB file, ${p.frames} frames`
      )
    })

    const record: LibraryClip = {
      id: randomUUID(),
      sessionId,
      dcfDir: session.dcfDir,
      dcfIndex: clip.dcfIndex,
      cardName: clip.name,
      file: join('sessions', sessionId, clipFileName(clip.dcfIndex)),
      sourceBytes: res.sourceBytes,
      bytes: res.outputBytes,
      sha256: res.sha256,
      health: res.health,
      frames: res.frames,
      drops: res.drops,
      droppedTornFrame: res.droppedTornFrame,
      width: res.width,
      height: res.height,
      rate: res.rate,
      scale: res.scale,
      durationSec: res.durationSec,
      startUtc: null,
      startSource: 'unknown',
      importedAt: new Date().toISOString()
    }
    await store.addClip(record)

    summary.imported++
    summary.bytesRead += res.bytesRead
    summary.bytesWritten += res.outputBytes
    summary.bytesOnCard += res.sourceBytes
    summary.reclaimed += Math.max(0, res.sourceBytes - res.outputBytes)
  }

  ctx.report(1, `${summary.imported} clips imported, ${summary.skipped} already in the library`)
  return summary
}
