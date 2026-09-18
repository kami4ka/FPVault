/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The one card that answers "what is plugged in and what can I do about it".
 * Every device state maps to a colour, a headline, a sentence and exactly one
 * primary action, so the user never has to work out which of five situations
 * they are in.
 */
import type { CardVolume, DeviceState } from '@shared/types'
import type { Strings } from '../i18n/index.js'

const ACCENT: Record<DeviceState['kind'], string> = {
  absent: 'var(--color-muted)',
  reader: 'var(--color-ok)',
  legacy: 'var(--color-warn)',
  fel: 'var(--color-busy)',
  cardOnly: 'var(--color-info)'
}

function gib(bytes: number): string {
  return `${(bytes / 1024 ** 3).toFixed(1)} GB`
}

function VolumeFacts({ volume, s }: { volume: CardVolume; s: Strings }) {
  const used =
    volume.totalBytes && volume.freeBytes ? 1 - volume.freeBytes / volume.totalBytes : null

  return (
    <div className="mt-5 border-t border-[var(--color-line)] pt-4">
      <div className="flex flex-wrap items-baseline gap-x-6 gap-y-1 text-sm">
        <span className="font-semibold">{s.card.sessions(volume.sessions.length)}</span>
        {volume.totalBytes !== null && volume.freeBytes !== null && (
          <span className="text-[var(--color-muted)]">
            {gib(volume.freeBytes)} {s.card.free} {s.card.of} {gib(volume.totalBytes)}
          </span>
        )}
      </div>

      {used !== null && (
        <div className="mt-2 h-1.5 w-full overflow-hidden rounded-full bg-[var(--color-line)]">
          <div
            className="h-full rounded-full bg-[var(--color-brand)]"
            style={{ width: `${Math.min(100, Math.max(2, used * 100))}%` }}
          />
        </div>
      )}

      <div className="mt-3 font-[family-name:var(--font-mono)] text-xs text-[var(--color-muted)]">
        {s.card.mount} {volume.path}
      </div>
    </div>
  )
}

export function StatusCard({
  state,
  s,
  onAction
}: {
  state: DeviceState
  s: Strings
  onAction: () => void
}) {
  const copy = s.state[state.kind]
  const accent = ACCENT[state.kind]
  const volume = 'volume' in state ? state.volume : null
  const live = state.kind !== 'absent'

  const firmware =
    state.kind === 'reader' || state.kind === 'legacy'
      ? (state.firmware.version ?? s.card.firmwareUnknown)
      : null

  return (
    <section
      className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-6"
      aria-live="polite"
    >
      <div className="flex items-start gap-3">
        <span
          className={`mt-1.5 h-2.5 w-2.5 shrink-0 rounded-full ${live ? 'dot-live' : ''}`}
          style={{ background: accent }}
          aria-hidden
        />
        <div className="min-w-0 flex-1">
          <h1 className="text-lg font-semibold">{copy.title}</h1>
          <p className="mt-1.5 max-w-prose text-sm leading-relaxed text-[var(--color-muted)]">
            {copy.body}
          </p>

          {firmware && (
            <p className="mt-3 text-sm">
              <span className="text-[var(--color-muted)]">{s.card.firmware}: </span>
              <span className="font-[family-name:var(--font-mono)]">{firmware}</span>
            </p>
          )}

          {volume ? (
            <VolumeFacts volume={volume} s={s} />
          ) : (
            (state.kind === 'reader' || state.kind === 'legacy') && (
              <p className="mt-4 text-sm text-[var(--color-warn)]">{s.state.noCardYet}</p>
            )
          )}

          <button
            type="button"
            onClick={onAction}
            className="no-drag mt-5 rounded-[var(--radius-card)] px-4 py-2 text-sm font-semibold text-white transition-opacity hover:opacity-90"
            style={{ background: accent }}
          >
            {copy.action}
          </button>
        </div>
      </div>
    </section>
  )
}
