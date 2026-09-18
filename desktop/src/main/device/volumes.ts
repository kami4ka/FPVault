/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Finding the card's mounted volume, without a native module and without
 * shelling out on any platform.
 *
 * We never identify the card by its label: a user can rename it, and on
 * Windows the label is not visible from a mount point at all. The reliable
 * signature is the directory layout the firmware creates — /DCIM holding one
 * or more DCF session directories named <NNN>FCDVR (src/dcf.c: DIR_MIN 100,
 * DIR_MAX 999, DCF_DIRTAG "FCDVR"). That is cheap to test and is true whether
 * the card is being served by the board or sitting in a native card reader.
 */
import { readdir, readFile, stat, statfs } from 'node:fs/promises'
import { join } from 'node:path'
import type { CardVolume } from '@shared/types'

/** <NNN>FCDVR, uppercase only — FatFs is built without long filenames. */
const SESSION_DIR = /^\d{3}FCDVR$/

/** Mount points worth testing, per platform. */
async function candidateMounts(): Promise<string[]> {
  if (process.platform === 'darwin') {
    // Every removable volume appears here; "Macintosh HD" is a symlink to /
    // and fails the DCIM test anyway, so it costs nothing to include.
    return (await safeReaddir('/Volumes')).map((n) => join('/Volumes', n))
  }

  if (process.platform === 'win32') {
    // Probing 26 drive letters is faster than spawning PowerShell and has no
    // dependency on the shell being available or unrestricted.
    const letters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('')
    const present = await Promise.all(
      letters.map(async (l) => ((await exists(`${l}:\\`)) ? `${l}:\\` : null))
    )
    return present.filter((p): p is string => p !== null)
  }

  // Linux: /proc/mounts is authoritative. Fall back to the usual automount
  // roots if it is unreadable (containers, odd distros).
  const mounts = new Set<string>()
  try {
    const raw = await readFile('/proc/mounts', 'utf8')
    for (const line of raw.split('\n')) {
      const [, mountPoint, fsType] = line.split(' ')
      if (!mountPoint || !fsType) continue
      if (fsType === 'vfat' || fsType === 'msdos' || fsType === 'exfat') {
        mounts.add(decodeMountPoint(mountPoint))
      }
    }
  } catch {
    /* fall through to the automount roots below */
  }
  for (const root of ['/media', '/run/media', '/mnt']) {
    for (const name of await safeReaddir(root)) {
      const p = join(root, name)
      // /run/media/<user>/<label> is one level deeper than /media/<label>.
      const nested = await safeReaddir(p)
      if (nested.length && !(await exists(join(p, 'DCIM')))) {
        for (const n of nested) mounts.add(join(p, n))
      }
      mounts.add(p)
    }
  }
  return [...mounts]
}

/** /proc/mounts octal-escapes spaces and a few other characters. */
function decodeMountPoint(s: string): string {
  return s.replace(/\\([0-7]{3})/g, (_, oct: string) => String.fromCharCode(parseInt(oct, 8)))
}

async function safeReaddir(dir: string): Promise<string[]> {
  try {
    return await readdir(dir)
  } catch {
    return []
  }
}

async function exists(p: string): Promise<boolean> {
  try {
    await stat(p)
    return true
  } catch {
    return false
  }
}

/** The session directories a FPVault card carries, or null if it is not one. */
async function sessionsOf(mount: string): Promise<string[] | null> {
  const entries = await safeReaddir(join(mount, 'DCIM'))
  if (!entries.length) {
    // A freshly scrubbed card has no /DCIM until the first clip is written
    // (src/dcf.c creates it lazily), so absence is not proof it is not ours.
    return null
  }
  const sessions = entries.filter((n) => SESSION_DIR.test(n)).sort()
  return sessions.length ? sessions : null
}

async function describe(mount: string, sessions: string[]): Promise<CardVolume> {
  let totalBytes: number | null = null
  let freeBytes: number | null = null
  try {
    const fsStat = await statfs(mount)
    totalBytes = Number(fsStat.blocks) * Number(fsStat.bsize)
    freeBytes = Number(fsStat.bavail) * Number(fsStat.bsize)
  } catch {
    /* space is cosmetic; a card with an unreadable statfs is still usable */
  }
  const label =
    process.platform === 'win32' ? null : (mount.split(/[/\\]/).filter(Boolean).pop() ?? null)
  return { path: mount, label, totalBytes, freeBytes, sessions }
}

/** Every mounted volume that looks like a FPVault card. */
export async function findCardVolumes(): Promise<CardVolume[]> {
  const mounts = await candidateMounts()
  const found = await Promise.all(
    mounts.map(async (m) => {
      const sessions = await sessionsOf(m)
      return sessions ? await describe(m, sessions) : null
    })
  )
  return found.filter((v): v is CardVolume => v !== null)
}

/**
 * The one card we act on. With a board attached there is normally exactly
 * one; if a user has both a board and a loose card in a reader we prefer the
 * one with more sessions, which is the one they are more likely to have just
 * flown with.
 */
export async function findCardVolume(): Promise<CardVolume | null> {
  const all = await findCardVolumes()
  if (!all.length) return null
  return all.reduce((best, v) => (v.sessions.length > best.sessions.length ? v : best))
}
