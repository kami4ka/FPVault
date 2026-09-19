/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Animated instructions for the things that need hands on the board.
 *
 * Steps advance on the live device state where one applies, so the
 * animation is a view of the state machine rather than a recording: hold
 * SW2 and plug in, and the moment the app sees the boot ROM the caption
 * moves on to "release it" by itself.
 *
 * Under prefers-reduced-motion the whole thing becomes a numbered list of
 * the same captions with a static diagram, which is also what someone would
 * want to read back afterwards.
 */
import { useEffect, useRef, useState } from 'react'
import type { DeviceState } from '@shared/types'
import { BoardArt } from './BoardArt.js'
import { LOCATE, SEQUENCES, type SequenceId } from './steps.js'
import type { Strings } from '../i18n/index.js'
import './guide.css'

function useReducedMotion(): boolean {
  const [reduced, setReduced] = useState(
    () => window.matchMedia?.('(prefers-reduced-motion: reduce)').matches ?? false
  )
  useEffect(() => {
    const mq = window.matchMedia('(prefers-reduced-motion: reduce)')
    const onChange = () => setReduced(mq.matches)
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [])
  return reduced
}

export function BoardGuide({
  sequence,
  device,
  s,
  onDone
}: {
  sequence: SequenceId
  device: DeviceState
  s: Strings
  onDone?: () => void
}) {
  const steps = SEQUENCES[sequence]
  const [index, setIndex] = useState(0)
  const reduced = useReducedMotion()
  const deviceRef = useRef(device)
  deviceRef.current = device

  /* Restart when the caller switches sequences. */
  useEffect(() => setIndex(0), [sequence])

  /* The live device state gets the final say on where the animation is.
   * When it has an opinion — the board reached recovery, or booted normally
   * when it should not have — the guide jumps there from wherever its timer
   * had got to, which is what makes this a view of the state machine rather
   * than a recording running alongside one. */
  const forcedId = LOCATE[sequence](device)
  const forcedIndex = forcedId ? steps.findIndex((st) => st.id === forcedId) : -1
  const shown = forcedIndex >= 0 ? forcedIndex : Math.min(index, steps.length - 1)
  const step = steps[shown]

  useEffect(() => {
    if (!step || step.terminal) return

    /* A step with its own predicate advances the moment it comes true. */
    if (step.waitFor?.(deviceRef.current)) {
      const next = shown + 1
      if (next >= steps.length) {
        onDone?.()
        return
      }
      setIndex(next)
      return
    }

    const timer = setTimeout(() => {
      setIndex(() => {
        const next = shown + 1
        /* Destinations are only reachable through the live state, never by
         * running off the end of the list. */
        const last = steps.findIndex((st) => st.terminal)
        const limit = last >= 0 ? last : steps.length
        if (next >= limit) return step.restart ? 0 : shown
        return next
      })
    }, step.ms)
    return () => clearTimeout(timer)
  }, [shown, step, steps, device, onDone])

  if (!step) return null

  if (reduced) {
    return (
      <div className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-4">
        <div className="flex gap-5">
          <div className="w-40 shrink-0">
            <BoardArt highlight={[]} hand={null} />
          </div>
          <ol className="flex-1 list-decimal space-y-2 pl-4 text-sm">
            {steps.map((st) => (
              <li key={st.id}>
                <span className="font-semibold">{st.caption(s)}</span>
                {st.sub && (
                  <span className="mt-0.5 block text-xs text-[var(--color-muted)]">
                    {st.sub(s)}
                  </span>
                )}
              </li>
            ))}
          </ol>
        </div>
      </div>
    )
  }

  return (
    <div className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-4">
      <div className="flex items-center gap-5">
        <div className="w-44 shrink-0">
          <BoardArt
            highlight={step.highlight}
            hand={step.press ? 'press' : null}
            tone={step.tone}
          />
        </div>

        <div className="min-w-0 flex-1">
            <p
            className="text-sm font-semibold"
            style={step.tone === 'success' ? { color: 'var(--color-ok)' } : undefined}
          >
            {step.caption(s)}
          </p>
          {step.sub && (
            <p className="mt-1.5 max-w-prose text-xs leading-relaxed text-[var(--color-muted)]">
              {step.sub(s)}
            </p>
          )}

          {/* Destinations are not steps, so they do not get a pip. */}
          <ol className="mt-4 flex gap-1.5" aria-label={s.guide.progress}>
            {steps
              .filter((st) => !st.terminal)
              .map((st, i) => (
                <li
                  key={st.id}
                  className="h-1 flex-1 rounded-full"
                  style={{
                    background: i === shown ? 'var(--color-brand)' : 'var(--color-line)',
                    opacity: i === shown ? 1 : i < shown ? 0.9 : 0.4
                  }}
                />
              ))}
          </ol>
        </div>
      </div>
    </div>
  )
}
