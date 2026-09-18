/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Giving clips a real date.
 *
 * The board has no RTC. FatFs is built with FF_FS_NORTC and FF_NORTC_YEAR
 * 2020, so every clip and directory on every card carries the identical
 * stamp 2020-01-01 00:00:00 — which hosts west of UTC render as
 * 2019-12-31. Those values must never be shown as if they meant something.
 *
 * Order, though, is exact and needs no clock:
 *
 *  - A session directory is one power-on, and dcf_boot_scan takes max + 1,
 *    so NNN increases with time.
 *  - Clip indices are scanned across every directory and never reused, so
 *    FCDV#### totally orders the card.
 *
 * So one real time — the moment a session started — plus each clip's true
 * duration places every clip in that session.
 *
 * The gap between clips is where honesty matters. recorder_on_frame rolls
 * over the instant frames_written reaches REC_SEG_FRAMES, so a clip with
 * exactly 9000 frames was followed immediately by the next one: the gap is
 * genuinely zero. A shorter clip ended through clip_stop(), which in
 * recording state needs 5 s of lost signal, and the next clip needs 1 s of
 * stable signal to start — at least six seconds, and really unknowable. We
 * mark those rather than invent a number.
 */
import { REC_SEG_FRAMES } from '@shared/avi/constants'
import type { LibraryClip } from './store.js'

/** Lower bound on a gap that was not a segment rollover, in seconds. */
export const UNKNOWN_GAP_SEC = 6

export interface TimedClip {
  id: string
  startUtc: string
  /** Seconds of dead time assumed before this clip. */
  gapBeforeSec: number
  /** True when that gap is a floor, not a known value. */
  gapUncertain: boolean
}

/**
 * Place every clip in a session on the clock, given when the session began.
 * Clips must be in DCF index order; the caller gets that from the store.
 */
export function inferSessionTimes(clips: LibraryClip[], sessionStart: Date): TimedClip[] {
  const out: TimedClip[] = []
  let cursor = sessionStart.getTime()

  for (const [i, clip] of clips.entries()) {
    let gapBeforeSec = 0
    let gapUncertain = false

    if (i > 0) {
      const prev = clips[i - 1]
      /* An exact segment rollover closes and reopens in the same tick. */
      const rolledOver = prev?.frames === REC_SEG_FRAMES
      if (!rolledOver) {
        gapBeforeSec = UNKNOWN_GAP_SEC
        gapUncertain = true
      }
      cursor += gapBeforeSec * 1000
    }

    out.push({
      id: clip.id,
      startUtc: new Date(cursor).toISOString(),
      gapBeforeSec,
      gapUncertain
    })

    /* Duration from the frames actually present, including the zero-length
     * dropout chunks — that is exactly why the firmware writes them. */
    const fps = clip.scale ? clip.rate / clip.scale : 0
    cursor += fps ? (clip.frames / fps) * 1000 : 0
  }

  return out
}

/**
 * A sensible default for the session start box: if the user plugged in soon
 * after landing, the session ended at about now, so it began one total
 * duration ago.
 */
export function guessSessionStart(clips: LibraryClip[], now = new Date()): Date {
  let seconds = 0
  for (const [i, clip] of clips.entries()) {
    const fps = clip.scale ? clip.rate / clip.scale : 0
    if (fps) seconds += clip.frames / fps
    if (i > 0 && clips[i - 1]?.frames !== REC_SEG_FRAMES) seconds += UNKNOWN_GAP_SEC
  }
  return new Date(now.getTime() - seconds * 1000)
}

/** A label for the session, from its real start time when there is one. */
export function sessionLabel(dcfDir: number, startUtc: string | null): string {
  if (!startUtc) return `Session ${dcfDir}`
  const d = new Date(startUtc)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`
}
