/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Installing a firmware release on a connected board.
 */
import { app } from 'electron'
import { join } from 'node:path'
import { dfuDownload, validateImage } from '../update/dfu.js'
import { downloadAsset, listReleases, type Release } from '../update/releases.js'
import type { JobContext } from './queue.js'

const KB = 1024

export interface FlashResult {
  tag: string
  bytes: number
}

function firmwareDir(): string {
  return join(app.getPath('userData'), 'firmware')
}

function findImage(release: Release) {
  const asset = release.assets.find((a) => a.name === 'fpvault.bin')
  if (!asset) throw new Error(`release ${release.tag} has no fpvault.bin`)
  return asset
}

/**
 * Fetch a release's firmware and send it to the board over DFU.
 *
 * Deliberately safe to repeat: flashing the same image twice is harmless,
 * which matters because the app cannot read the installed version (the
 * descriptors carry none) and so must never claim a board is up to date.
 */
export async function flashRelease(tag: string, ctx: JobContext): Promise<FlashResult> {
  const releases = await listReleases()
  const release = releases.find((r) => r.tag === tag)
  if (!release) throw new Error(`no release tagged ${tag}`)

  const asset = findImage(release)

  ctx.report(0, `downloading ${asset.name} from ${release.tag}`)
  const { bytes } = await downloadAsset(release, asset, firmwareDir(), (got, total) => {
    ctx.throwIfCancelled()
    /* Download is a tenth of the bar; the flash itself is the rest. */
    ctx.report(total ? (got / total) * 0.1 : 0, `downloaded ${(got / KB).toFixed(0)} KB`)
  })

  /* Check the image the way the board will, so a bad file fails here with a
   * sentence rather than as an opaque rejection mid-flash. */
  const bad = validateImage(bytes)
  if (bad) throw new Error(bad)

  ctx.report(0.1, `verified ${bytes.length} bytes, sending to the board`)

  await dfuDownload(
    bytes,
    (p) => {
      const frac = p.bytesTotal ? p.bytesSent / p.bytesTotal : 0
      if (p.phase === 'sending')
        ctx.report(0.1 + frac * 0.8, `sending ${(p.bytesSent / KB).toFixed(0)} of ${(p.bytesTotal / KB).toFixed(0)} KB`)
      else if (p.phase === 'burning')
        ctx.report(0.92, 'the board is writing its flash — do not unplug it')
      else ctx.report(0.98, 'written and verified, the board is rebooting')
    },
    () => ctx.throwIfCancelled()
  )

  ctx.report(1, `${release.tag} installed; the board is restarting`)
  return { tag: release.tag, bytes: bytes.length }
}
