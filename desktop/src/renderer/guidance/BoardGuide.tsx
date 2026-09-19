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
import { SEQUENCES, type SequenceId } from './steps.js'
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

  const step = steps[Math.min(index, steps.length - 1)]

  /* Advance: immediately if this step's predicate is already true of the
   * live state, otherwise when its own time is up. */
  useEffect(() => {
    if (!step) return

    const satisfied = step.waitFor?.(deviceRef.current) ?? false
    if (satisfied) {
      const next = index + 1
      if (next >= steps.length) {
        onDone?.()
        return
      }
      setIndex(next)
      return
    }

    const timer = setTimeout(() => {
      setIndex((i) => {
        const nxt = i + 1
        if (nxt >= steps.length) return step.restart ? 0 : i
        return nxt
      })
    }, step.ms)
    return () => clearTimeout(timer)
  }, [index, step, steps.length, device, onDone])

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
          <BoardArt highlight={step.highlight} hand={step.press ? 'press' : null} />
        </div>

        <div className="min-w-0 flex-1">
          <p className="text-sm font-semibold">{step.caption(s)}</p>
          {step.sub && (
            <p className="mt-1.5 max-w-prose text-xs leading-relaxed text-[var(--color-muted)]">
              {step.sub(s)}
            </p>
          )}

          <ol className="mt-4 flex gap-1.5" aria-label={s.guide.progress}>
            {steps.map((st, i) => (
              <li
                key={st.id}
                className="h-1 flex-1 rounded-full"
                style={{
                  background:
                    i === index
                      ? 'var(--color-brand)'
                      : i < index
                        ? 'var(--color-line)'
                        : 'var(--color-line)',
                  opacity: i === index ? 1 : i < index ? 0.9 : 0.4
                }}
              />
            ))}
          </ol>
        </div>
      </div>
    </div>
  )
}
