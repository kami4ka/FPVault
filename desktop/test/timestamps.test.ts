/* SPDX-License-Identifier: GPL-3.0-or-later */
import { describe, expect, it } from 'vitest'
import { REC_SEG_FRAMES } from '../src/shared/avi/constants.js'
import {
  UNKNOWN_GAP_SEC,
  guessSessionStart,
  inferSessionTimes
} from '../src/main/library/timestamps.js'
import type { LibraryClip } from '../src/main/library/store.js'

function clip(id: string, frames: number, dcfIndex: number): LibraryClip {
  return {
    id,
    sessionId: 's',
    dcfDir: 100,
    dcfIndex,
    cardName: `FCDV${String(dcfIndex).padStart(4, '0')}.AVI`,
    file: `${id}.avi`,
    sourceBytes: 0,
    bytes: 0,
    sha256: '',
    health: 'clean',
    frames,
    drops: 0,
    droppedTornFrame: false,
    width: 720,
    height: 480,
    rate: 30000,
    scale: 1001,
    durationSec: frames / (30000 / 1001),
    startUtc: null,
    startSource: 'unknown',
    importedAt: ''
  }
}

const start = new Date('2026-09-18T14:00:00.000Z')

describe('timestamp inference', () => {
  it('treats a full 9000-frame segment as a zero gap', () => {
    /* A rollover closes and reopens in the same tick, so the next clip
     * begins exactly one segment later — 300.3 s at 29.97 fps. */
    const clips = [clip('a', REC_SEG_FRAMES, 1), clip('b', REC_SEG_FRAMES, 2)]
    const [first, second] = inferSessionTimes(clips, start)

    expect(first?.startUtc).toBe('2026-09-18T14:00:00.000Z')
    expect(second?.gapBeforeSec).toBe(0)
    expect(second?.gapUncertain).toBe(false)
    expect(second?.startUtc).toBe('2026-09-18T14:05:00.300Z')
  })

  it('marks the gap after a short clip as uncertain', () => {
    /* Anything short of 9000 frames ended via clip_stop(), which needs 5 s
     * of lost signal plus 1 s to re-lock. Six seconds is a floor, not a
     * measurement, and the UI has to say so. */
    const clips = [clip('a', 3000, 1), clip('b', 3000, 2)]
    const [, second] = inferSessionTimes(clips, start)

    expect(second?.gapBeforeSec).toBe(UNKNOWN_GAP_SEC)
    expect(second?.gapUncertain).toBe(true)
  })

  it('places a crash-cut last clip correctly', () => {
    /* Its frame count is recovered by the walk, so its duration is known
     * and it can never mislead a successor: it is always last. */
    const clips = [clip('a', REC_SEG_FRAMES, 1), clip('b', 467, 2)]
    const times = inferSessionTimes(clips, start)
    expect(times).toHaveLength(2)
    expect(times[1]?.startUtc).toBe('2026-09-18T14:05:00.300Z')
  })

  it('runs the clock forward across a whole session', () => {
    const clips = [
      clip('a', REC_SEG_FRAMES, 1),
      clip('b', REC_SEG_FRAMES, 2),
      clip('c', REC_SEG_FRAMES, 3)
    ]
    const times = inferSessionTimes(clips, start)
    const last = new Date(times[2]!.startUtc).getTime() - start.getTime()
    expect(last / 1000).toBeCloseTo(600.6, 1) /* two full segments */
  })

  it('guesses a start that ends the session about now', () => {
    const clips = [clip('a', REC_SEG_FRAMES, 1), clip('b', REC_SEG_FRAMES, 2)]
    const now = new Date('2026-09-18T15:00:00.000Z')
    const guessed = guessSessionStart(clips, now)
    /* Two full segments back from now. */
    expect((now.getTime() - guessed.getTime()) / 1000).toBeCloseTo(600.6, 1)
  })
})
