/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Application directories, resolved without hard-depending on Electron.
 *
 * Reaching for `app.getPath()` at module scope makes a file impossible to
 * exercise outside a running Electron process, which in practice means the
 * download-and-flash path could only ever be tested by hand. Everything
 * that needs a directory goes through here instead, and falls back to a
 * sensible location when Electron is absent.
 */
import { homedir, tmpdir } from 'node:os'
import { join } from 'node:path'

type ElectronApp = { getPath(name: string): string }

let electronApp: ElectronApp | null = null
try {
  const mod = (await import('electron')) as unknown as { app?: ElectronApp }
  /* In a plain Node process the import resolves to a path string, not the
   * module, so the guard has to check the shape rather than truthiness. */
  electronApp = typeof mod?.app?.getPath === 'function' ? mod.app : null
} catch {
  electronApp = null
}

function fallback(name: string): string {
  if (name === 'userData') return join(tmpdir(), 'fpvault-desktop')
  if (name === 'videos') return join(homedir(), 'Movies')
  return tmpdir()
}

export function appPath(name: 'userData' | 'videos'): string {
  try {
    return electronApp?.getPath(name) ?? fallback(name)
  } catch {
    return fallback(name)
  }
}

/** Where downloaded firmware images are cached, keyed by release tag. */
export function firmwareDir(): string {
  return join(appPath('userData'), 'firmware')
}

/** Default library root, overridable by the user. */
export function defaultLibraryRoot(): string {
  return join(appPath('videos'), 'FPVault')
}
