/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Where the bundled executables live.
 *
 * In development they sit under resources/bin/<platform>-<arch>/, fetched by
 * scripts/fetch-binaries.mjs. In a packaged app electron-builder copies that
 * directory to Contents/Resources/bin via extraResources — deliberately not
 * asarUnpack, because an executable cannot run from inside an archive and
 * extraResources keeps it out of one entirely.
 */
import { access, constants } from 'node:fs/promises'
import { join, resolve } from 'node:path'

/* Electron is absent when this module is exercised from a plain Node script
 * or a test, so it is loaded defensively and the dev paths are used. */
type ElectronApp = { isPackaged: boolean; getAppPath(): string }
let electronApp: ElectronApp | null = null
try {
  const mod = (await import('electron')) as unknown as { app?: ElectronApp }
  electronApp = mod.app ?? null
} catch {
  electronApp = null
}

export type Tool = 'ffmpeg' | 'sunxi-fel'

const suffix = process.platform === 'win32' ? '.exe' : ''

function candidates(tool: Tool): string[] {
  const name = tool + suffix
  if (electronApp?.isPackaged) return [join(process.resourcesPath, 'bin', name)]

  /* Dev: electron-vite runs from out/main, so walk back to the package root. */
  const platform = `${process.platform}-${process.arch}`
  const roots = [process.cwd()]
  if (electronApp) roots.unshift(electronApp.getAppPath())
  return roots.map((r) => resolve(r, 'resources', 'bin', platform, name))
}

const cache = new Map<Tool, string | null>()

/** Absolute path to a bundled tool, or null when it was not shipped. */
export async function resolveBin(tool: Tool): Promise<string | null> {
  if (cache.has(tool)) return cache.get(tool) ?? null

  let found: string | null = null
  for (const path of candidates(tool)) {
    try {
      await access(path, constants.X_OK)
      found = path
      break
    } catch {
      /* try the next candidate */
    }
  }

  if (!found) {
    console.warn(
      `[tools] ${tool} is not bundled for ${process.platform}-${process.arch}. ` +
        `Run: npm run fetch-binaries`
    )
  }
  cache.set(tool, found)
  return found
}
