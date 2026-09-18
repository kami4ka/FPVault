/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Task tiles under the status card. A tile that cannot do anything useful in
 * the current device state is disabled rather than hidden, so the layout does
 * not jump around as the board is plugged and unplugged.
 */
import type { DeviceState } from '@shared/types'
import type { Strings } from '../i18n/index.js'

type TaskId = 'import' | 'repair' | 'glue' | 'timestamps' | 'update'

const ACCENT: Record<TaskId, string> = {
  import: 'var(--color-brand)',
  repair: 'var(--color-warn)',
  glue: 'var(--color-busy)',
  timestamps: 'var(--color-info)',
  update: 'var(--color-record)'
}

function enabledFor(task: TaskId, state: DeviceState): boolean {
  const hasCard =
    (state.kind === 'reader' || state.kind === 'legacy') && state.volume !== null
      ? true
      : state.kind === 'cardOnly'

  switch (task) {
    case 'import':
      return hasCard
    case 'repair':
    case 'glue':
    case 'timestamps':
      // These act on the library, which exists independently of the board.
      return true
    case 'update':
      return state.kind === 'reader' || state.kind === 'legacy' || state.kind === 'fel'
  }
}

export function TaskTiles({
  state,
  s,
  onOpen
}: {
  state: DeviceState
  s: Strings
  onOpen: (task: TaskId) => void
}) {
  const tasks: TaskId[] = ['import', 'repair', 'glue', 'timestamps', 'update']

  return (
    <div className="mt-6 grid grid-cols-[repeat(auto-fill,minmax(210px,1fr))] gap-3">
      {tasks.map((id) => {
        const enabled = enabledFor(id, state)
        const copy = s.tasks[id]
        return (
          <button
            key={id}
            type="button"
            disabled={!enabled}
            onClick={() => onOpen(id)}
            className="no-drag rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-4 text-left transition-colors enabled:hover:border-[var(--color-brand)] disabled:cursor-not-allowed disabled:opacity-45"
          >
            <span
              className="block h-1 w-7 rounded-full"
              style={{ background: ACCENT[id] }}
              aria-hidden
            />
            <span className="mt-3 block text-sm font-semibold">{copy.title}</span>
            <span className="mt-1 block text-xs leading-relaxed text-[var(--color-muted)]">
              {copy.body}
            </span>
          </button>
        )
      })}
    </div>
  )
}
