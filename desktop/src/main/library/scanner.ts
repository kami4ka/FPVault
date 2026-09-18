/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What is on the card, grouped the way the firmware wrote it.
 *
 * This deliberately does not open a single clip. Listing a card over the
 * board's USB link is fast, but parsing 80 clips is not — so the grid is
 * built from names and sizes alone, and the expensive walk happens per clip
 * only when the user asks for it or during import.
 *
 * That is possible because a clip's size already carries the one fact that
 * matters most: a file of exactly the preallocation size was never closed.
 */
import { readdir, stat } from 'node:fs/promises'
import { join } from 'node:path'
import { parseClipName, parseSessionDir } from '@shared/dcf'
import { REC_PREALLOC } from '@shared/avi/constants'

export interface CardClip {
  /** Absolute path on the mounted card. */
  path: string
  name: string
  dcfIndex: number
  bytes: number
  /** True when the file is exactly the f_expand size, i.e. never finalized. */
  looksCrashCut: boolean
}

export interface CardSession {
  /** The NNN of NNNFCDVR, which increases with every power-on. */
  dcfDir: number
  dirName: string
  clips: CardClip[]
  bytes: number
}

export interface CardContents {
  volumePath: string
  sessions: CardSession[]
  totalClips: number
  totalBytes: number
  /** How much of that is the wasted tail of unfinalised clips. */
  reclaimableBytes: number
}

export async function scanCard(volumePath: string): Promise<CardContents> {
  const dcim = join(volumePath, 'DCIM')
  let entries: string[] = []
  try {
    entries = await readdir(dcim)
  } catch {
    return {
      volumePath,
      sessions: [],
      totalClips: 0,
      totalBytes: 0,
      reclaimableBytes: 0
    }
  }

  const sessions: CardSession[] = []
  let totalClips = 0
  let totalBytes = 0
  let reclaimableBytes = 0

  for (const entry of entries.sort()) {
    const dcfDir = parseSessionDir(entry)
    if (dcfDir === null) continue

    const dirPath = join(dcim, entry)
    let files: string[]
    try {
      files = await readdir(dirPath)
    } catch {
      continue
    }

    const clips: CardClip[] = []
    let bytes = 0

    for (const name of files.sort()) {
      const dcfIndex = parseClipName(name)
      if (dcfIndex === null) continue
      const path = join(dirPath, name)
      let size: number
      try {
        size = (await stat(path)).size
      } catch {
        continue
      }
      const looksCrashCut = size === REC_PREALLOC
      clips.push({ path, name, dcfIndex, bytes: size, looksCrashCut })
      bytes += size
      if (looksCrashCut) {
        /* Most of a 200 MB unfinalised clip is preallocation nobody wants.
         * The real figure needs a walk; this is the honest upper bound. */
        reclaimableBytes += size
      }
    }

    if (!clips.length) continue
    clips.sort((a, b) => a.dcfIndex - b.dcfIndex)
    sessions.push({ dcfDir, dirName: entry, clips, bytes })
    totalClips += clips.length
    totalBytes += bytes
  }

  sessions.sort((a, b) => a.dcfDir - b.dcfDir)
  return { volumePath, sessions, totalClips, totalBytes, reclaimableBytes }
}
