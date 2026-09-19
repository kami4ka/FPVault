/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * User preferences.
 *
 * The shape lives in @shared/types so the renderer sees the same thing; what
 * is here is the defaults, the quality table those names map to, and the
 * reading and writing.
 *
 * Written through a temp file and a rename, like the library index: a crash
 * mid-write should cost the change, not the file.
 */
import { mkdir, readFile, rename, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import type { ExportQuality, Prefs } from '@shared/types'
import { appPath } from './paths.js'

/**
 * What each quality name means to x264. The middle one is what every
 * export used before this was a choice, so an existing library keeps
 * producing the files it already has.
 */
export const QUALITY: Record<ExportQuality, { crf: number; preset: string }> = {
  high: { crf: 18, preset: 'slow' },
  balanced: { crf: 20, preset: 'veryfast' },
  small: { crf: 24, preset: 'veryfast' }
}

const DEFAULTS: Prefs = {
  libraryRoot: null,
  deinterlace: true,
  quality: 'balanced',
  gapSeconds: 6
}

function file(): string {
  return join(appPath('userData'), 'settings.json')
}

let cache: Prefs | null = null

export async function loadPrefs(): Promise<Prefs> {
  if (cache) return cache
  try {
    const raw = JSON.parse(await readFile(file(), 'utf8')) as Partial<Prefs>
    cache = normalise({ ...DEFAULTS, ...raw })
  } catch {
    cache = { ...DEFAULTS }
  }
  return cache
}

/**
 * The current preferences without awaiting. Safe because loadPrefs runs
 * before any IPC handler is registered, so by the time anything can ask,
 * the file has been read.
 */
export function prefs(): Prefs {
  return cache ?? DEFAULTS
}

export async function savePrefs(patch: Partial<Prefs>): Promise<Prefs> {
  const next = normalise({ ...(await loadPrefs()), ...patch })
  cache = next

  const path = file()
  const tmp = `${path}.tmp`
  await mkdir(dirname(path), { recursive: true })
  await writeFile(tmp, JSON.stringify(next, null, 1), 'utf8')
  await rename(tmp, path)
  return next
}

/** Keep values in range whether they came from the UI or a hand-edited file. */
export function normalise(p: Prefs): Prefs {
  const gap = Number(p.gapSeconds)
  return {
    ...p,
    deinterlace: Boolean(p.deinterlace),
    quality: p.quality in QUALITY ? p.quality : DEFAULTS.quality,
    /* A negative gap would run the session clock backwards. */
    gapSeconds: Number.isFinite(gap) ? Math.max(0, Math.min(3600, Math.round(gap))) : DEFAULTS.gapSeconds
  }
}
