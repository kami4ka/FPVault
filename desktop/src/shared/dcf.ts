/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DCF naming, ported from src/dcf.c.
 *
 * This is also how a FPVault card is recognised. The USB mass-storage
 * strings are CherryUSB's defaults ("MASS" / "Storage Device"), not
 * "FPVault", so nothing that keys on a name would work. The directory
 * layout is the real signature.
 *
 * Two properties of the firmware's numbering make everything downstream
 * possible, and both come from dcf_boot_scan():
 *
 *  - Session directories are monotonically increasing: a new power-on takes
 *    max_dir + 1, so NNN orders sessions in time even with no clock.
 *  - Clip indices are scanned across *every* directory and never reused, so
 *    FCDV#### totally orders the whole card, across sessions too.
 */

/** src/dcf.c:22-24 */
export const DIR_MIN = 100
export const DIR_MAX = 999
export const IDX_MAX = 9999

const DIRTAG = 'FCDVR'
const PREFIX = 'FCDV'
const EXT = 'AVI'

/**
 * "NNNFCDVR", exactly 8 characters, NNN in 100..999. Returns NNN, or null.
 * Rejects the cases src/dcf.c calls out by name: "100MEDIA" (wrong tag),
 * "99FCDVR" (too short), "1000FCDVR" (too long), "099FCDVR" (below the DCF
 * range), and anything lowercase — FatFs is built without long filenames.
 */
export function parseSessionDir(name: string): number | null {
  if (name.length !== 3 + DIRTAG.length) return null
  if (name.slice(3) !== DIRTAG) return null
  const digits = name.slice(0, 3)
  if (!/^\d{3}$/.test(digits)) return null
  const v = Number(digits)
  return v < DIR_MIN || v > DIR_MAX ? null : v
}

/** "FCDV####.AVI", exactly 12 characters, uppercase. Returns ####, or null. */
export function parseClipName(name: string): number | null {
  if (name.length !== PREFIX.length + 4 + 1 + EXT.length) return null
  if (!name.startsWith(PREFIX)) return null
  const digits = name.slice(PREFIX.length, PREFIX.length + 4)
  if (!/^\d{4}$/.test(digits)) return null
  if (name[PREFIX.length + 4] !== '.') return null
  if (name.slice(PREFIX.length + 5) !== EXT) return null
  const v = Number(digits)
  return v < 1 || v > IDX_MAX ? null : v
}

export function sessionDirName(n: number): string {
  return `${String(n).padStart(3, '0')}${DIRTAG}`
}

export function clipFileName(n: number): string {
  return `${PREFIX}${String(n).padStart(4, '0')}.${EXT}`
}
