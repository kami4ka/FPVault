/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The library: sessions as groups, clips inside them.
 *
 * The card's own timestamps are never shown as dates. Every clip on every
 * card carries the same constant, so a date column built from them would be
 * pure noise. A session gets real times only once the user says when it
 * started, and clips whose preceding gap is a floor rather than a
 * measurement are marked as such.
 */
import { useEffect, useState } from 'react'
import { ClipPlayer } from '../components/ClipPlayer.js'
import { ClipThumb } from '../components/ClipThumb.js'
import type { LibraryClipView, LibraryView } from '@shared/types'
import { bytes, clockTime, duration, toLocalInput } from '../components/format.js'
import type { Strings } from '../i18n/index.js'

const HEALTH_COLOR: Record<LibraryClipView['health'], string> = {
  clean: 'var(--color-ok)',
  crashCut: 'var(--color-warn)',
  damaged: 'var(--color-record)'
}

function ClipRow({
  clip,
  s,
  onPlay
}: {
  clip: LibraryClipView
  s: Strings
  onPlay: () => void
}) {
  const saved = clip.sourceBytes - clip.bytes
  return (
    <li className="flex items-center gap-3 border-b border-[var(--color-line)] px-4 py-2 last:border-b-0">
      <span
        className="h-2 w-2 shrink-0 rounded-full"
        style={{ background: HEALTH_COLOR[clip.health] }}
        title={s.health[clip.health]}
      />
      <button
        type="button"
        onClick={onPlay}
        className="flex shrink-0 items-center gap-2.5 text-left hover:text-[var(--color-brand)]"
        title={s.libraryScreen.playHint}
      >
        <ClipThumb clipId={clip.id} frames={clip.frames} className="h-9 w-14" />
        <span className="w-28 font-[family-name:var(--font-mono)] text-xs">{clip.cardName}</span>
      </button>
      <span className="w-16 shrink-0 text-xs text-[var(--color-muted)]">
        {duration(clip.durationSec)}
      </span>
      <span className="w-20 shrink-0 text-xs text-[var(--color-muted)]">
        {clip.frames} f
      </span>
      <span className="w-20 shrink-0 text-xs text-[var(--color-muted)]">
        {bytes(clip.bytes)}
      </span>
      <span className="flex-1 truncate text-xs text-[var(--color-muted)]">
        {clip.health === 'crashCut' && saved > 0 && s.libraryScreen.recovered(bytes(saved))}
        {clip.droppedTornFrame && ` · ${s.libraryScreen.tornDropped}`}
      </span>
      <span className="w-20 shrink-0 text-right font-[family-name:var(--font-mono)] text-xs">
        {clockTime(clip.startUtc)}
        {clip.gapUncertain && (
          <span className="text-[var(--color-warn)]" title={s.libraryScreen.gapUncertain}>
            {' ~'}
          </span>
        )}
      </span>
      <button
        type="button"
        onClick={() => void window.fpvault.library.reveal(clip.file)}
        className="shrink-0 rounded px-2 py-0.5 text-xs text-[var(--color-muted)] hover:text-[var(--color-ink)]"
      >
        {s.libraryScreen.reveal}
      </button>
    </li>
  )
}

function SessionGroup({
  session,
  s,
  onSetStart,
  onPlay,
  canExport
}: {
  session: LibraryView['sessions'][number]
  s: Strings
  onSetStart: (id: string, iso: string | null) => void
  onPlay: (clip: LibraryClipView) => void
  canExport: boolean
}) {
  const [open, setOpen] = useState(true)
  const [editing, setEditing] = useState(false)
  const cut = session.clips.filter((c) => c.health === 'crashCut').length

  return (
    <section className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)]">
      <header className="flex items-center gap-3 px-4 py-3">
        <button
          type="button"
          onClick={() => setOpen((v) => !v)}
          className="text-sm font-semibold"
          aria-expanded={open}
        >
          {open ? '▾' : '▸'} {session.label}
        </button>
        <span className="text-xs text-[var(--color-muted)]">
          {session.clips.length} {session.clips.length === 1 ? 'clip' : 'clips'} ·{' '}
          {duration(session.durationSec)} · {bytes(session.bytes)}
          {cut > 0 && ` · ${s.libraryScreen.repaired(cut)}`}
        </span>
        <span className="flex-1" />
        <button
          type="button"
          onClick={() => void window.fpvault.jobs.joinSession(session.id)}
          className="rounded px-2 py-1 text-xs text-[var(--color-busy)] hover:underline"
          title={s.libraryScreen.joinHint}
        >
          {s.libraryScreen.join}
        </button>
        {canExport && (
          <button
            type="button"
            onClick={() => void window.fpvault.jobs.exportSession(session.id)}
            className="rounded px-2 py-1 text-xs text-[var(--color-info)] hover:underline"
            title={s.libraryScreen.exportHint}
          >
            {s.libraryScreen.export}
          </button>
        )}
        {editing ? (
          <input
            type="datetime-local"
            autoFocus
            defaultValue={toLocalInput(
              session.startUtc ? new Date(session.startUtc) : new Date()
            )}
            onBlur={(e) => {
              setEditing(false)
              if (e.target.value) onSetStart(session.id, new Date(e.target.value).toISOString())
            }}
            className="rounded border border-[var(--color-line)] bg-transparent px-2 py-1 text-xs"
          />
        ) : (
          <button
            type="button"
            onClick={() => setEditing(true)}
            className="rounded px-2 py-1 text-xs text-[var(--color-brand)] hover:underline"
          >
            {session.startUtc ? s.libraryScreen.changeStart : s.libraryScreen.setStart}
          </button>
        )}
      </header>

      {open && (
        <ul className="border-t border-[var(--color-line)]">
          {session.clips.map((c) => (
            <ClipRow key={c.id} clip={c} s={s} onPlay={() => onPlay(c)} />
          ))}
        </ul>
      )}
    </section>
  )
}

export function Library({ library, s }: { library: LibraryView | null; s: Strings }) {
  const [playing, setPlaying] = useState<LibraryClipView | null>(null)
  const [canExport, setCanExport] = useState(false)

  useEffect(() => {
    void window.fpvault.app.canExport().then(setCanExport)
  }, [])

  if (!library || !library.sessions.length)
    return (
      <div className="rounded-[var(--radius-card)] border border-dashed border-[var(--color-line)] p-10 text-center text-sm text-[var(--color-muted)]">
        {s.libraryScreen.empty}
      </div>
    )

  const setStart = (id: string, iso: string | null) => {
    void window.fpvault.library.setSessionStart(id, iso)
  }

  return (
    <div className="space-y-3">
      <p className="font-[family-name:var(--font-mono)] text-xs text-[var(--color-muted)]">
        {library.root}
      </p>
      {library.sessions.map((session) => (
        <SessionGroup
          key={session.id}
          session={session}
          s={s}
          onSetStart={setStart}
          onPlay={setPlaying}
          canExport={canExport}
        />
      ))}
      <p className="pt-2 text-xs text-[var(--color-muted)]">{s.libraryScreen.noRtcNote}</p>

      {playing && (
        <ClipPlayer
          clipId={playing.id}
          name={playing.cardName}
          s={s}
          onClose={() => setPlaying(null)}
        />
      )}
    </div>
  )
}
