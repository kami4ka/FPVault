/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Left rail: the device sits at the top as a live entry, sections below it.
 * Structure follows WD Discovery; the colours are FPVault's own.
 */
import type { DeviceState } from '@shared/types'
import type { Lang, Strings } from '../i18n/index.js'

export type Route = 'device' | 'library' | 'firmware' | 'settings'

const DOT: Record<DeviceState['kind'], string> = {
  absent: 'var(--color-muted)',
  reader: 'var(--color-ok)',
  legacy: 'var(--color-warn)',
  fel: 'var(--color-busy)',
  cardOnly: 'var(--color-info)'
}

/* The wordmark from docs/img/logo.png, reduced to type: "FP" in brand cyan,
 * "Vault" in ink. Drawn rather than bitmapped so it stays crisp and themes. */
function Wordmark() {
  return (
    <div className="px-4 pt-3 pb-5 text-[15px] font-bold tracking-tight">
      <span className="text-[var(--color-brand)]">FP</span>
      <span>Vault</span>
    </div>
  )
}

function Item({
  label,
  active,
  onClick,
  dot
}: {
  label: string
  active: boolean
  onClick: () => void
  dot?: string
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      aria-current={active ? 'page' : undefined}
      className={`no-drag flex w-full items-center gap-2.5 rounded-[var(--radius-card)] px-3 py-2 text-left text-sm transition-colors ${
        active
          ? 'bg-[var(--color-line)] font-semibold'
          : 'text-[var(--color-muted)] hover:bg-[var(--color-line)]/60'
      }`}
    >
      {dot ? (
        <span className="h-2 w-2 shrink-0 rounded-full" style={{ background: dot }} aria-hidden />
      ) : (
        <span className="h-2 w-2 shrink-0" aria-hidden />
      )}
      <span className="truncate">{label}</span>
    </button>
  )
}

export function DeviceRail({
  route,
  setRoute,
  state,
  s,
  lang,
  setLang
}: {
  route: Route
  setRoute: (r: Route) => void
  state: DeviceState
  s: Strings
  lang: Lang
  setLang: (l: Lang) => void
}) {
  return (
    <nav className="drag flex h-full w-56 shrink-0 flex-col border-r border-[var(--color-line)] bg-[var(--color-surface)]">
      <div className="h-7" />
      <Wordmark />

      <div className="flex flex-col gap-0.5 px-2">
        <Item
          label={s.nav.device}
          active={route === 'device'}
          onClick={() => setRoute('device')}
          dot={DOT[state.kind]}
        />
        <Item label={s.nav.library} active={route === 'library'} onClick={() => setRoute('library')} />
        <Item
          label={s.nav.firmware}
          active={route === 'firmware'}
          onClick={() => setRoute('firmware')}
        />
        <Item
          label={s.nav.settings}
          active={route === 'settings'}
          onClick={() => setRoute('settings')}
        />
      </div>

      <div className="mt-auto px-3 pb-3">
        <button
          type="button"
          onClick={() => setLang(lang === 'en' ? 'uk' : 'en')}
          className="no-drag rounded px-2 py-1 text-xs text-[var(--color-muted)] hover:text-[var(--color-ink)]"
        >
          {lang === 'en' ? 'Українська' : 'English'}
        </button>
      </div>
    </nav>
  )
}
