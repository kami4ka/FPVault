/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What was actually bundled with this build, and under what licence.
 *
 * Two jobs in one place. The first is honesty about missing buttons: the
 * MP4 export and FEL recovery both disappear when their executable was not
 * fetched, and a hidden button with no explanation is worse than no button.
 * Settings names the tool, says where it was looked for and what is lost.
 *
 * The second is the GPL obligation. The bundled ffmpeg is a GPL build and
 * sunxi-fel is GPL-2.0-or-later, so redistributing them carries an offer of
 * corresponding source. The facts below match CREDITS.md; the prose that
 * describes each one lives in the string tables, so both languages stay in
 * step.
 */
import { execFile } from 'node:child_process'
import { readFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import type { BundledToolInfo, LicenceInfo } from '@shared/types'
import { resolveBin, type Tool } from './resolveBin.js'

const FACTS: Record<Tool, Omit<BundledToolInfo, 'id' | 'path' | 'version'>> = {
  ffmpeg: {
    license: 'GPL-2.0-or-later',
    homepage: 'https://ffmpeg.org/',
    sourceUrl: 'https://ffmpeg.org/releases/'
  },
  'sunxi-fel': {
    license: 'GPL-2.0-or-later',
    homepage: 'https://github.com/linux-sunxi/sunxi-tools',
    sourceUrl: 'https://github.com/linux-sunxi/sunxi-tools'
  }
}

/** Run a binary for its version string, giving up rather than hanging. */
function probe(bin: string, args: string[]): Promise<string> {
  return new Promise((resolve_) => {
    execFile(bin, args, { timeout: 4000, windowsHide: true }, (err, stdout, stderr) => {
      if (err && !stdout && !stderr) return resolve_('')
      resolve_(`${stdout}${stderr}`)
    })
  })
}

async function versionOf(tool: Tool, bin: string): Promise<string | null> {
  if (tool === 'ffmpeg') {
    const out = await probe(bin, ['-version'])
    return /^ffmpeg version (\S+)/m.exec(out)?.[1] ?? null
  }

  /* sunxi-fel's own `version` command talks to a board, so it cannot
   * answer without one plugged in. scripts/build-sunxi-fel.sh writes the
   * commit it built beside the binary instead — the honest answer, and no
   * constant here to drift out of step with the script. */
  try {
    const stamp = await readFile(join(dirname(bin), 'sunxi-fel.version'), 'utf8')
    return stamp.trim() || null
  } catch {
    return null
  }
}

const cache = new Map<Tool, BundledToolInfo>()

export async function bundledTool(tool: Tool): Promise<BundledToolInfo> {
  const hit = cache.get(tool)
  if (hit) return hit

  const path = await resolveBin(tool)
  const info: BundledToolInfo = {
    id: tool,
    path,
    version: path ? await versionOf(tool, path) : null,
    ...FACTS[tool]
  }
  cache.set(tool, info)
  return info
}

export async function bundledTools(): Promise<BundledToolInfo[]> {
  return Promise.all((['ffmpeg', 'sunxi-fel'] as Tool[]).map(bundledTool))
}

/**
 * Everything the licences panel lists: the app itself, the two bundled
 * executables with whatever version is really installed, and the runtime
 * the app is built on. Anything not bundled in this build is left out
 * rather than listed as an obligation we do not have.
 */
export async function licences(appVersion: string): Promise<LicenceInfo[]> {
  const tools = await bundledTools()
  const find = (id: Tool) => tools.find((t) => t.id === id)
  const ffmpeg = find('ffmpeg')
  const fel = find('sunxi-fel')

  const out: LicenceInfo[] = [
    {
      id: 'fpvault',
      name: 'FPVault Desktop',
      version: appVersion,
      license: 'GPL-3.0-or-later',
      homepage: 'https://github.com/kami4ka/FPVault',
      sourceUrl: 'https://github.com/kami4ka/FPVault'
    }
  ]

  if (ffmpeg?.path) {
    out.push({
      id: 'ffmpeg',
      name: 'FFmpeg',
      version: ffmpeg.version,
      license: ffmpeg.license,
      homepage: ffmpeg.homepage,
      sourceUrl: ffmpeg.sourceUrl
    })
    out.push({
      id: 'x264',
      name: 'x264',
      version: null,
      license: 'GPL-2.0-or-later',
      homepage: 'https://www.videolan.org/developers/x264.html',
      sourceUrl: 'https://code.videolan.org/videolan/x264'
    })
  }

  if (fel?.path) {
    out.push({
      id: 'sunxi-tools',
      name: 'sunxi-tools',
      version: fel.version,
      license: fel.license,
      homepage: fel.homepage,
      sourceUrl: fel.sourceUrl
    })
  }

  out.push({
    id: 'runtime',
    name: 'Electron, Node.js, React, Vite, Tailwind CSS',
    version: process.versions.electron,
    license: 'MIT',
    homepage: 'https://electronjs.org/',
    sourceUrl: null
  })

  return out
}
