/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware releases, and installing one.
 *
 * The screen is built around something the app cannot know: which firmware
 * a board is already running. The USB descriptors carry no version, the DFU
 * interface refuses uploads, and there is no serial console. So it never
 * says "up to date" or "update available" — it shows what the board can do,
 * offers to install a specific release, and makes repeating that harmless.
 */
import { useEffect, useState } from 'react'
import type { DeviceState, ReleaseInfo } from '@shared/types'
import { BoardGuide } from '../guidance/BoardGuide.js'
import type { Strings } from '../i18n/index.js'

function Capability({ device, s }: { device: DeviceState; s: Strings }) {
  /* Firmware from v0.9.3 encodes its version in bcdDevice; anything older
   * reports a constant that means nothing, and the UI says so rather than
   * inventing a number. */
  const version =
    device.kind === 'reader' || device.kind === 'legacy' ? device.firmware.version : null
  const copy =
    device.kind === 'reader'
      ? s.firmwareScreen.capableDfu
      : device.kind === 'legacy'
        ? s.firmwareScreen.capableLegacy
        : device.kind === 'fel'
          ? s.firmwareScreen.capableFel
          : s.firmwareScreen.capableNone

  const accent =
    device.kind === 'reader'
      ? 'var(--color-ok)'
      : device.kind === 'legacy'
        ? 'var(--color-warn)'
        : device.kind === 'fel'
          ? 'var(--color-busy)'
          : 'var(--color-muted)'

  return (
    <section className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-4">
      <div className="flex items-start gap-3">
        <span
          className="mt-1.5 h-2.5 w-2.5 shrink-0 rounded-full"
          style={{ background: accent }}
          aria-hidden
        />
        <div>
          <p className="text-sm font-semibold">{copy.title}</p>
          <p className="mt-1 max-w-prose text-xs leading-relaxed text-[var(--color-muted)]">
            {copy.body}
          </p>
        </div>
      </div>

      <div className="mt-4 border-t border-[var(--color-line)] pt-3">
        <p className="text-xs">
          <span className="text-[var(--color-muted)]">{s.firmwareScreen.installed}: </span>
          <span className="font-[family-name:var(--font-mono)] font-semibold">
            {version ?? s.card.firmwareUnknown}
          </span>
        </p>
        {!version && (
          <p className="mt-1 max-w-prose text-[11px] leading-relaxed text-[var(--color-muted)]">
            {s.firmwareScreen.whyUnknown}
          </p>
        )}
      </div>
    </section>
  )
}

export function Firmware({ device, s }: { device: DeviceState; s: Strings }) {
  const [releases, setReleases] = useState<ReleaseInfo[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [canFlash, setCanFlash] = useState(false)
  const [busy, setBusy] = useState(false)
  const [showFel, setShowFel] = useState(false)
  const [canRecover, setCanRecover] = useState(false)

  const load = (force = false) => {
    setBusy(true)
    setError(null)
    window.fpvault.firmware
      .releases(force)
      .then(setReleases)
      .catch((e: Error) => setError(e.message))
      .finally(() => setBusy(false))
  }

  useEffect(() => {
    load()
    void window.fpvault.firmware.canFlash().then(setCanFlash)
    void window.fpvault.firmware.canRecover().then(setCanRecover)
  }, [])

  const flashable = device.kind === 'reader' && canFlash
  /* A board with no DFU interface cannot be updated over USB at all; it
   * needs one trip through the boot ROM's recovery mode first. */
  const needsFel = device.kind === 'legacy'
  const inFel = device.kind === 'fel'

  return (
    <div className="space-y-4">
      <Capability device={device} s={s} />

      {(needsFel || showFel) && (
        <BoardGuide sequence="enterFel" device={device} s={s} />
      )}

      <section className="rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)]">
        <header className="flex items-center justify-between border-b border-[var(--color-line)] px-4 py-3">
          <h2 className="text-sm font-semibold">{s.firmwareScreen.releases}</h2>
          <button
            type="button"
            onClick={() => load(true)}
            disabled={busy}
            className="rounded px-2 py-1 text-xs text-[var(--color-muted)] hover:text-[var(--color-ink)] disabled:opacity-50"
          >
            {s.firmwareScreen.check}
          </button>
        </header>

        {error && <p className="px-4 py-4 text-xs text-[var(--color-record)]">{error}</p>}

        {releases?.map((r, i) => (
          <article key={r.tag} className="border-b border-[var(--color-line)] px-4 py-3 last:border-b-0">
            <div className="flex items-center gap-2">
              <span className="font-[family-name:var(--font-mono)] text-sm font-semibold">
                {r.tag}
              </span>
              {i === 0 && (
                <span className="rounded bg-[var(--color-brand)] px-1.5 py-0.5 text-[10px] font-semibold text-white">
                  {s.firmwareScreen.newest}
                </span>
              )}
              {r.prerelease && (
                <span className="rounded border border-[var(--color-warn)] px-1.5 py-0.5 text-[10px] text-[var(--color-warn)]">
                  {s.firmwareScreen.prerelease}
                </span>
              )}
              <span className="flex-1" />
              {inFel ? (
                <button
                  type="button"
                  disabled={!canRecover || !r.hasFirmware}
                  onClick={() => void window.fpvault.firmware.recover(r.tag, true)}
                  className="rounded-[var(--radius-card)] bg-[var(--color-busy)] px-3 py-1.5 text-xs font-semibold text-white disabled:opacity-35"
                >
                  {s.firmwareScreen.recoverWith(r.tag)}
                </button>
              ) : (
                <button
                  type="button"
                  disabled={!flashable || !r.hasFirmware}
                  onClick={() => void window.fpvault.firmware.flash(r.tag)}
                  className="rounded-[var(--radius-card)] bg-[var(--color-record)] px-3 py-1.5 text-xs font-semibold text-white disabled:opacity-35"
                  title={flashable ? undefined : s.firmwareScreen.cannotFlash}
                >
                  {s.firmwareScreen.install(r.tag)}
                </button>
              )}
            </div>
            <p className="mt-1 text-xs text-[var(--color-muted)]">{r.name}</p>
          </article>
        ))}

        {releases && !releases.length && !error && (
          <p className="px-4 py-8 text-center text-sm text-[var(--color-muted)]">
            {s.firmwareScreen.none}
          </p>
        )}
      </section>

      <div className="flex items-baseline gap-3">
        <p className="flex-1 text-[11px] leading-relaxed text-[var(--color-muted)]">
          {s.firmwareScreen.safetyNote}
        </p>
        <button
          type="button"
          onClick={() => setShowFel((v) => !v)}
          className="shrink-0 rounded px-2 py-1 text-xs text-[var(--color-busy)] hover:underline"
        >
          {s.firmwareScreen.recovery}
        </button>
      </div>
    </div>
  )
}
