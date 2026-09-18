/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Choosing what to bring off the card.
 *
 * The number worth showing is the contrast: a clip cut by a power loss
 * occupies 200 MB on the card but usually holds a fraction of that, and the
 * app only ever reads the real part. Saying so is more useful than a
 * progress bar alone.
 */
import { useEffect, useState } from 'react'
import type { CardContentsInfo, DeviceState, JobState } from '@shared/types'
import { bytes } from '../components/format.js'
import type { Strings } from '../i18n/index.js'

function SessionRow({
  session,
  checked,
  onToggle
}: {
  session: CardContentsInfo['sessions'][number]
  checked: boolean
  onToggle: () => void
}) {
  const cut = session.clips.filter((c) => c.looksCrashCut).length
  return (
    <label className="flex cursor-pointer items-center gap-3 border-b border-[var(--color-line)] px-4 py-3 last:border-b-0 hover:bg-[var(--color-line)]/40">
      <input type="checkbox" checked={checked} onChange={onToggle} className="h-4 w-4" />
      <span className="min-w-0 flex-1">
        <span className="block text-sm font-semibold">Session {session.dcfDir}</span>
        <span className="block text-xs text-[var(--color-muted)]">
          {session.clips.length} {session.clips.length === 1 ? 'clip' : 'clips'} ·{' '}
          {bytes(session.bytes)} on the card
          {cut > 0 && (
            <>
              {' · '}
              <span className="text-[var(--color-warn)]">
                {cut} cut by a power loss
              </span>
            </>
          )}
        </span>
      </span>
      <span className="font-[family-name:var(--font-mono)] text-xs text-[var(--color-muted)]">
        {session.dirName}
      </span>
    </label>
  )
}

export function Import({
  device,
  jobs,
  s
}: {
  device: DeviceState
  jobs: JobState[]
  s: Strings
}) {
  const volume = 'volume' in device ? device.volume : null
  const [contents, setContents] = useState<CardContentsInfo | null>(null)
  const [selected, setSelected] = useState<Set<number>>(new Set())
  const [scanning, setScanning] = useState(false)

  useEffect(() => {
    if (!volume) {
      setContents(null)
      return
    }
    let alive = true
    setScanning(true)
    void window.fpvault.card
      .scan(volume.path)
      .then((c) => {
        if (!alive) return
        setContents(c)
        setSelected(new Set(c.sessions.map((x) => x.dcfDir)))
      })
      .finally(() => alive && setScanning(false))
    return () => {
      alive = false
    }
  }, [volume?.path])

  const active = jobs.filter((j) => j.phase === 'running' || j.phase === 'queued')

  if (!volume)
    return (
      <div className="rounded-[var(--radius-card)] border border-dashed border-[var(--color-line)] p-10 text-center text-sm text-[var(--color-muted)]">
        {s.state.noCard}
      </div>
    )

  const reclaim = contents
    ? contents.sessions
        .flatMap((x) => x.clips)
        .filter((c) => selected.has(Math.floor(c.dcfIndex)) || true)
        .filter((c) => c.looksCrashCut).length
    : 0

  return (
    <div className="space-y-4">
      <section className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)]">
        <header className="flex items-baseline justify-between border-b border-[var(--color-line)] px-4 py-3">
          <h2 className="text-sm font-semibold">{s.tasks.import.title}</h2>
          <span className="text-xs text-[var(--color-muted)]">
            {scanning
              ? s.importScreen.scanning
              : contents
                ? `${contents.totalClips} clips · ${bytes(contents.totalBytes)}`
                : ''}
          </span>
        </header>

        <div className="max-h-[46vh] overflow-y-auto">
          {contents?.sessions.map((session) => (
            <SessionRow
              key={session.dcfDir}
              session={session}
              checked={selected.has(session.dcfDir)}
              onToggle={() =>
                setSelected((prev) => {
                  const next = new Set(prev)
                  if (next.has(session.dcfDir)) next.delete(session.dcfDir)
                  else next.add(session.dcfDir)
                  return next
                })
              }
            />
          ))}
          {contents && !contents.sessions.length && (
            <p className="px-4 py-8 text-center text-sm text-[var(--color-muted)]">
              {s.importScreen.empty}
            </p>
          )}
        </div>

        <footer className="flex items-center justify-between border-t border-[var(--color-line)] px-4 py-3">
          <span className="text-xs text-[var(--color-muted)]">
            {reclaim > 0 && s.importScreen.reclaimNote(reclaim)}
          </span>
          <button
            type="button"
            disabled={!selected.size || active.length > 0}
            onClick={() =>
              void window.fpvault.jobs.importSessions(volume.path, [...selected])
            }
            className="rounded-[var(--radius-card)] bg-[var(--color-brand)] px-4 py-2 text-sm font-semibold text-white disabled:opacity-40"
          >
            {s.importScreen.start(selected.size)}
          </button>
        </footer>
      </section>

      {jobs.length > 0 && (
        <section className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-4">
          <h2 className="mb-3 text-sm font-semibold">{s.importScreen.transfers}</h2>
          <ul className="space-y-3">
            {jobs.map((job) => (
              <li key={job.id}>
                <div className="flex items-baseline justify-between text-xs">
                  <span className="font-semibold">{job.label}</span>
                  <span className="text-[var(--color-muted)]">{job.phase}</span>
                </div>
                <div className="mt-1 h-1.5 w-full overflow-hidden rounded-full bg-[var(--color-line)]">
                  <div
                    className="h-full rounded-full transition-[width]"
                    style={{
                      width: `${Math.round((job.progress ?? 0) * 100)}%`,
                      background:
                        job.phase === 'failed'
                          ? 'var(--color-record)'
                          : job.phase === 'done'
                            ? 'var(--color-ok)'
                            : 'var(--color-brand)'
                    }}
                  />
                </div>
                <p className="mt-1 font-[family-name:var(--font-mono)] text-[11px] text-[var(--color-muted)]">
                  {job.error ?? job.detail}
                </p>
              </li>
            ))}
          </ul>
        </section>
      )}
    </div>
  )
}
