/* SPDX-License-Identifier: GPL-3.0-or-later */
import { describe, expect, it } from 'vitest'
import { normalise, QUALITY } from '../src/main/settings.js'
import type { Prefs } from '../src/shared/types.js'

const base: Prefs = {
  libraryRoot: null,
  deinterlace: true,
  quality: 'balanced',
  gapSeconds: 6
}

describe('preferences', () => {
  it('keeps the balanced quality equal to what exports used before it was a choice', () => {
    /* An existing library must keep producing the files it already has. */
    expect(QUALITY.balanced).toEqual({ crf: 20, preset: 'veryfast' })
  })

  it('orders the quality names by size', () => {
    expect(QUALITY.high.crf).toBeLessThan(QUALITY.balanced.crf)
    expect(QUALITY.balanced.crf).toBeLessThan(QUALITY.small.crf)
  })

  it('refuses a negative gap, which would run the session clock backwards', () => {
    expect(normalise({ ...base, gapSeconds: -30 }).gapSeconds).toBe(0)
  })

  it('rounds and caps the gap', () => {
    expect(normalise({ ...base, gapSeconds: 7.6 }).gapSeconds).toBe(8)
    expect(normalise({ ...base, gapSeconds: 99999 }).gapSeconds).toBe(3600)
  })

  it('falls back when a hand-edited file holds nonsense', () => {
    const bad = { ...base, gapSeconds: NaN, quality: 'enormous' } as unknown as Prefs
    const fixed = normalise(bad)
    expect(fixed.gapSeconds).toBe(6)
    expect(fixed.quality).toBe('balanced')
  })
})
