/* SPDX-License-Identifier: GPL-3.0-or-later */
import { useEffect, useMemo, useState } from 'react'
import type { JobState, LibraryView } from '@shared/types'
import { DeviceRail, type Route } from './components/DeviceRail.js'
import { StatusCard } from './components/StatusCard.js'
import { TaskTiles } from './components/TaskTiles.js'
import { Import } from './routes/Import.js'
import { Library } from './routes/Library.js'
import { detectLang, setLang as persistLang, strings, type Lang } from './i18n/index.js'
import { useDevice } from './useDevice.js'

function Placeholder({ title }: { title: string }) {
  return (
    <div className="rounded-[var(--radius-card)] border border-dashed border-[var(--color-line)] p-10 text-center text-sm text-[var(--color-muted)]">
      {title}
    </div>
  )
}

export function App() {
  const [route, setRoute] = useState<Route>('device')
  const [lang, setLangState] = useState<Lang>(() => detectLang())
  const { state, rescan, busy } = useDevice()
  const s = useMemo(() => strings(lang), [lang])

  const [library, setLibrary] = useState<LibraryView | null>(null)
  const [jobs, setJobs] = useState<JobState[]>([])

  useEffect(() => {
    void window.fpvault.library.get().then(setLibrary)
    void window.fpvault.jobs.list().then(setJobs)
    const offLib = window.fpvault.library.onChange(setLibrary)
    const offJob = window.fpvault.jobs.onChange((job) =>
      setJobs((prev) => {
        const next = prev.filter((j) => j.id !== job.id)
        next.push(job)
        return next
      })
    )
    return () => {
      offLib()
      offJob()
    }
  }, [])

  const switchLang = (l: Lang) => {
    persistLang(l)
    setLangState(l)
  }

  return (
    <div className="flex h-full">
      <DeviceRail
        route={route}
        setRoute={setRoute}
        state={state}
        s={s}
        lang={lang}
        setLang={switchLang}
      />

      <main className="flex-1 overflow-y-auto">
        <header className="drag flex h-11 items-center justify-end px-5">
          <button
            type="button"
            onClick={rescan}
            disabled={busy}
            className="no-drag rounded px-2 py-1 text-xs text-[var(--color-muted)] hover:text-[var(--color-ink)] disabled:opacity-50"
          >
            {s.common.rescan}
          </button>
        </header>

        <div className="mx-auto max-w-4xl px-6 pb-10">
          {route === 'device' && (
            <>
              <StatusCard state={state} s={s} onAction={() => setRoute('import')} />
              <TaskTiles
                state={state}
                s={s}
                onOpen={(task) =>
                  setRoute(
                    task === 'update' ? 'firmware' : task === 'import' ? 'import' : 'library'
                  )
                }
              />
            </>
          )}
          {route === 'import' && <Import device={state} jobs={jobs} s={s} />}
          {route === 'library' && <Library library={library} s={s} />}
          {route === 'firmware' && <Placeholder title={s.nav.firmware} />}
          {route === 'settings' && <Placeholder title={s.nav.settings} />}
        </div>
      </main>
    </div>
  )
}
