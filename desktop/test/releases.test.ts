/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The recovery trailer decides whether the app steers a release away from
 * the Install button, so getting it wrong is either a silent half-update or
 * a warning on every release. Both were live mistakes: the first version of
 * this feature inferred it from the assets, and every release ships a U-Boot
 * image whether or not it changed one.
 */
import { describe, expect, it } from 'vitest'

/* Mirrors src/main/ipc.ts. */
const RECOVERY_TRAILER = /^[ \t]*FPVault-Requires-Recovery:[ \t]*(yes|true)[ \t]*$/im
const strip = (notes: string) =>
  notes.replace(/^[ \t]*FPVault-[A-Za-z-]+:[^\n]*\n?/gim, '').trimEnd()

describe('release notes trailer', () => {
  it('detects the trailer', () => {
    expect(RECOVERY_TRAILER.test('Fixes a thing.\n\nFPVault-Requires-Recovery: yes\n')).toBe(true)
  })

  it('accepts the spellings a human might write', () => {
    for (const line of [
      'FPVault-Requires-Recovery: yes',
      'fpvault-requires-recovery: YES',
      'FPVault-Requires-Recovery:true',
      '  FPVault-Requires-Recovery:   yes  '
    ])
      expect(RECOVERY_TRAILER.test(`notes\n${line}\n`)).toBe(true)
  })

  it('does not fire on an ordinary release', () => {
    expect(RECOVERY_TRAILER.test('High-Speed USB, 4-bit SD, v2 board\n')).toBe(false)
  })

  it('does not fire on prose that merely mentions recovery', () => {
    const notes = 'Use recovery if the board will not boot. FPVault-Requires-Recovery is a trailer.'
    expect(RECOVERY_TRAILER.test(notes)).toBe(false)
  })

  it('strips every trailer from what the reader sees', () => {
    const notes = 'Body text.\n\nFPVault-Requires-Recovery: yes\nFPVault-Something-Else: 1\n'
    expect(strip(notes)).toBe('Body text.')
  })

  it('leaves notes without a trailer alone', () => {
    expect(strip('Just a release.\n')).toBe('Just a release.')
  })
})
