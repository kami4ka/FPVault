/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Running work, shown on the screen that owns it.
 *
 * A firmware write has no business taking up space above the clip library,
 * so each job kind belongs to one route and appears only there. And a job
 * that has finished says so for a few seconds and then gets out of the way
 * — except a failure, which stays until it is dismissed, because the error
 * text is the whole point of it.
 */
import type { JobState } from '@shared/types'
import type { Route } from './DeviceRail.js'
import type { Strings } from '../i18n/index.js'

const BAR: Record<JobState['phase'], string> = {
  queued: 'var(--color-muted)',
  running: 'var(--color-brand)',
  done: 'var(--color-ok)',
  failed: 'var(--color-record)',
  cancelled: 'var(--color-muted)'
}

/** Which screen each kind of work belongs to. */
const HOME: Record<JobState['kind'], Route> = {
  import: 'import',
  repair: 'library',
  join: 'library',
  export: 'library',
  firmware: 'firmware',
  recover: 'firmware'
}

export function jobsForRoute(jobs: JobState[], route: Route): JobState[] {
  return jobs.filter((j) => HOME[j.kind] === route)
}

export function JobBar({
  jobs,
  route,
  s,
  onDismiss
}: {
  jobs: JobState[]
  route: Route
  s: Strings
  onDismiss: (id: string) => void
}) {
  const shown = jobsForRoute(jobs, route).slice(-4)
  if (!shown.length) return null

  return (
    <section className="mb-4 rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-3">
      <ul className="space-y-2.5">
        {shown.map((job) => (
          <li key={job.id}>
            <div className="flex items-baseline justify-between gap-3 text-xs">
              <span className="font-semibold">{job.label}</span>
              <span className="text-[var(--color-muted)]">{s.jobPhase[job.phase]}</span>
              {job.phase === 'running' ? (
                <button
                  type="button"
                  onClick={() => void window.fpvault.jobs.cancel(job.id)}
                  className="text-[var(--color-muted)] hover:text-[var(--color-record)]"
                >
                  {s.common.cancel}
                </button>
              ) : (
                <button
                  type="button"
                  onClick={() => onDismiss(job.id)}
                  aria-label={s.common.dismiss}
                  className="text-[var(--color-muted)] hover:text-[var(--color-ink)]"
                >
                  ×
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
