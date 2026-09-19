/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings.
 *
 * Deliberately short. Only three things here change what the app does — the
 * library folder, how the MP4 export encodes, and what to assume for a gap
 * the board did not record — and each one exists because the right answer
 * genuinely depends on the person rather than on the hardware.
 *
 * The other two panels are not settings at all. Bundled tools explains why a
 * button is missing, which is the one thing a hidden button cannot do for
 * itself, and licences is the GPL obligation that comes with shipping those
 * tools.
 */
import { useEffect, useState } from 'react'
import type { BundledToolInfo, ExportQuality, LicenceInfo, Prefs } from '@shared/types'
import type { Lang, Strings } from '../i18n/index.js'

function Section({
  title,
  body,
  children
}: {
  title: string
  body?: string
  children: React.ReactNode
}) {
  return (
    <section className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)]">
      <header className="border-b border-[var(--color-line)] px-4 py-3">
        <h2 className="text-sm font-semibold">{title}</h2>
        {body && (
          <p className="mt-1 max-w-prose text-xs leading-relaxed text-[var(--color-muted)]">
            {body}
          </p>
        )}
      </header>
      <div className="px-4 py-3">{children}</div>
    </section>
  )
}

function Chip({
  label,
  hint,
  active,
  onClick
}: {
  label: string
  hint: string
  active: boolean
  onClick: () => void
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      aria-pressed={active}
      className={`flex-1 rounded-[var(--radius-card)] border px-3 py-2.5 text-left transition-colors ${
        active
          ? 'border-[var(--color-brand)] bg-[var(--color-brand)]/10'
          : 'border-[var(--color-line)] hover:bg-[var(--color-line)]/50'
      }`}
    >
      <span className="block text-xs font-semibold">{label}</span>
      <span className="mt-1 block text-[11px] leading-relaxed text-[var(--color-muted)]">
        {hint}
      </span>
    </button>
  )
}

function Toggle({
  label,
  hint,
  on,
  onChange
}: {
  label: string
  hint: string
  on: boolean
  onChange: (v: boolean) => void
}) {
  return (
    <label className="flex cursor-pointer items-start gap-3">
      <input
        type="checkbox"
        checked={on}
        onChange={(e) => onChange(e.target.checked)}
        className="mt-0.5 h-4 w-4 shrink-0 accent-[var(--color-brand)]"
      />
      <span>
        <span className="block text-xs font-semibold">{label}</span>
        <span className="mt-1 block max-w-prose text-[11px] leading-relaxed text-[var(--color-muted)]">
          {hint}
        </span>
      </span>
    </label>
  )
}

function Tools({ tools, s }: { tools: BundledToolInfo[]; s: Strings }) {
  const t = s.settingsScreen.tools
  return (
    <div className="space-y-3">
      {tools.map((tool) => (
        <div key={tool.id} className="flex items-start gap-3">
          <span
            className="mt-1.5 h-2 w-2 shrink-0 rounded-full"
            style={{ background: tool.path ? 'var(--color-ok)' : 'var(--color-muted)' }}
            aria-hidden
          />
          <div className="min-w-0 flex-1">
            <p className="text-xs">
              <span className="font-[family-name:var(--font-mono)] font-semibold">{tool.id}</span>
              <span className="text-[var(--color-muted)]">
                {' · '}
                {tool.path ? t.present : t.missing}
                {tool.version ? ` ${tool.version}` : ''}
                {' · '}
                {t.needs[tool.id]}
              </span>
            </p>
            {tool.path ? (
              <p className="mt-0.5 truncate font-[family-name:var(--font-mono)] text-[11px] text-[var(--color-muted)]">
                {tool.path}
              </p>
            ) : (
              <p className="mt-0.5 text-[11px] leading-relaxed text-[var(--color-muted)]">
                {t.fetchHint}
              </p>
            )}
          </div>
        </div>
      ))}
      <p className="max-w-prose border-t border-[var(--color-line)] pt-3 text-[11px] leading-relaxed text-[var(--color-muted)]">
        {t.pureNote}
      </p>
    </div>
  )
}

function Licences({ items, s }: { items: LicenceInfo[]; s: Strings }) {
  const l = s.settingsScreen.licences
  const what = l.what as Record<string, string | undefined>
  const open = (url: string) => void window.fpvault.app.openUrl(url)

  return (
    <div className="space-y-3">
      {items.map((item) => (
        <div key={item.id} className="flex flex-wrap items-baseline gap-x-2 gap-y-1">
          <span className="text-xs font-semibold">{item.name}</span>
          {item.version && (
            <span className="font-[family-name:var(--font-mono)] text-[11px] text-[var(--color-muted)]">
              {item.version}
            </span>
          )}
          <span className="rounded bg-[var(--color-line)] px-1.5 py-0.5 text-[10px] font-semibold text-[var(--color-muted)]">
            {item.license}
          </span>
          <span className="w-full text-[11px] leading-relaxed text-[var(--color-muted)]">
            {what[item.id] ?? ''}
            {item.sourceUrl && (
              <>
                {' '}
                <button
                  type="button"
                  onClick={() => item.sourceUrl && open(item.sourceUrl)}
                  className="text-[var(--color-brand)] underline-offset-2 hover:underline"
                >
                  {l.source}
                </button>
              </>
            )}
          </span>
        </div>
      ))}
    </div>
  )
}

export function Settings({
  s,
  lang,
  setLang,
  libraryRoot
}: {
  s: Strings
  lang: Lang
  setLang: (l: Lang) => void
  /** Published by the library view, which is the store's own answer. */
  libraryRoot: string | null
}) {
  const [prefs, setPrefs] = useState<Prefs | null>(null)
  const [tools, setTools] = useState<BundledToolInfo[]>([])
  const [licences, setLicences] = useState<LicenceInfo[]>([])
  const [version, setVersion] = useState<string | null>(null)
  const c = s.settingsScreen

  useEffect(() => {
    void window.fpvault.settings.get().then(setPrefs)
    void window.fpvault.app.tools().then(setTools)
    void window.fpvault.app.licences().then(setLicences)
    void window.fpvault.app.versions().then((v) => setVersion(v.app))
  }, [])

  /* Written through immediately: there is no Save button, so the file is
   * the state and the screen only mirrors it. */
  const update = (patch: Partial<Omit<Prefs, 'libraryRoot'>>) => {
    setPrefs((p) => (p ? { ...p, ...patch } : p))
    void window.fpvault.settings.set(patch).then(setPrefs)
  }

  const quality = (q: ExportQuality) => update({ quality: q })

  return (
    <div className="space-y-4">
      <Section title={c.library.title} body={c.library.body}>
        <p className="truncate font-[family-name:var(--font-mono)] text-xs">
          {libraryRoot ?? s.common.unknown}
        </p>
        <div className="mt-3 flex gap-2">
          <button
            type="button"
            onClick={() => void window.fpvault.library.chooseRoot()}
            className="rounded-[var(--radius-card)] bg-[var(--color-brand)] px-3 py-1.5 text-xs font-semibold text-white"
          >
            {c.library.change}
          </button>
          <button
            type="button"
            onClick={() => void window.fpvault.library.reveal('')}
            className="rounded-[var(--radius-card)] border border-[var(--color-line)] px-3 py-1.5 text-xs hover:bg-[var(--color-line)]/50"
          >
            {c.library.show}
          </button>
        </div>
        <p className="mt-3 max-w-prose text-[11px] leading-relaxed text-[var(--color-muted)]">
          {c.library.note}
        </p>
      </Section>

      <Section title={c.exports.title} body={c.exports.body}>
        <Toggle
          label={c.exports.deinterlace}
          hint={c.exports.deinterlaceHint}
          on={prefs?.deinterlace ?? true}
          onChange={(v) => update({ deinterlace: v })}
        />
        <p className="mt-4 mb-2 text-xs font-semibold">{c.exports.quality}</p>
        <div className="flex flex-wrap gap-2">
          <Chip
            label={c.exports.high}
            hint={c.exports.highHint}
            active={prefs?.quality === 'high'}
            onClick={() => quality('high')}
          />
          <Chip
            label={c.exports.balanced}
            hint={c.exports.balancedHint}
            active={prefs?.quality === 'balanced'}
            onClick={() => quality('balanced')}
          />
          <Chip
            label={c.exports.small}
            hint={c.exports.smallHint}
            active={prefs?.quality === 'small'}
            onClick={() => quality('small')}
          />
        </div>
      </Section>

      <Section title={c.gap.title} body={c.gap.body}>
        <label className="flex items-center gap-2 text-xs">
          <span>{c.gap.label}</span>
          <input
            type="number"
            min={0}
            max={3600}
            value={prefs?.gapSeconds ?? 6}
            onChange={(e) => update({ gapSeconds: Number(e.target.value) })}
            className="w-20 rounded-[var(--radius-card)] border border-[var(--color-line)] bg-transparent px-2 py-1 text-right font-[family-name:var(--font-mono)] text-xs"
          />
          <span className="text-[var(--color-muted)]">{c.gap.unit}</span>
        </label>
        <p className="mt-3 max-w-prose text-[11px] leading-relaxed text-[var(--color-muted)]">
          {c.gap.note}
        </p>
      </Section>

      <Section title={c.tools.title} body={c.tools.body}>
        <Tools tools={tools} s={s} />
      </Section>

      <Section title={c.licences.title} body={c.licences.body}>
        <Licences items={licences} s={s} />
      </Section>

      <Section title={c.language.title} body={c.language.body}>
        <div className="flex gap-2">
          {(['en', 'uk'] as Lang[]).map((l) => (
            <button
              key={l}
              type="button"
              onClick={() => setLang(l)}
              aria-pressed={lang === l}
              className={`rounded-[var(--radius-card)] border px-3 py-1.5 text-xs transition-colors ${
                lang === l
                  ? 'border-[var(--color-brand)] bg-[var(--color-brand)]/10 font-semibold'
                  : 'border-[var(--color-line)] hover:bg-[var(--color-line)]/50'
              }`}
            >
              {l === 'en' ? 'English' : 'Українська'}
            </button>
          ))}
        </div>
        <p className="mt-4 border-t border-[var(--color-line)] pt-3 text-[11px] text-[var(--color-muted)]">
          {c.about.version} {version ?? ''}
        </p>
      </Section>
    </div>
  )
}
