/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Running work, wherever you are in the app. Import and join both take
 * minutes on a full card, so the progress has to follow the user between
 * screens rather than live on the one that started it.
 */
import type { JobState } from '@shared/types'
import type { Strings } from '../i18n/index.js'

const BAR: Record<JobState['phase'], string> = {
  queued: 'var(--color-muted)',
  running: 'var(--color-brand)',
  done: 'var(--color-ok)',
  failed: 'var(--color-record)',
  cancelled: 'var(--color-muted)'
}

export function JobBar({ jobs, s }: { jobs: JobState[]; s: Strings }) {
  /* Finished jobs stay until something new starts, so the last result is
   * still readable; anything older than the current batch is dropped. */
  const shown = jobs.slice(-4)
  if (!shown.length) return null

  return (
    <section className="mb-4 rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-3">
      <ul className="space-y-2.5">
        {shown.map((job) => (
          <li key={job.id}>
            <div className="flex items-baseline justify-between gap-3 text-xs">
              <span className="font-semibold">{job.label}</span>
              <span className="text-[var(--color-muted)]">{s.jobPhase[job.phase]}</span>
              {job.phase === 'running' && (
                <button
                  type="button"
                  onClick={() => void window.fpvault.jobs.cancel(job.id)}
                  className="text-[var(--color-muted)] hover:text-[var(--color-record)]"
                >
                  {s.common.cancel}
                </button>
              )}
            </div>
            <div className="mt-1 h-1.5 w-full overflow-hidden rounded-full bg-[var(--color-line)]">
              <div
                className="h-full rounded-full transition-[width] duration-200"
                style={{
                  width: `${Math.round((job.progress ?? 0) * 100)}%`,
                  background: BAR[job.phase]
                }}
              />
            </div>
            <p className="mt-1 truncate font-[family-name:var(--font-mono)] text-[11px] text-[var(--color-muted)]">
              {job.error ?? job.detail}
            </p>
          </li>
        ))}
      </ul>
    </section>
  )
}
