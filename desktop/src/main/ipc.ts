/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every IPC handler, and the one guard that keeps the card safe.
 */
import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron'
import { join, resolve, sep } from 'node:path'
import type { LibraryClipView, LibraryView } from '@shared/types'
import type { DeviceWatcher } from './device/watcher.js'
import { scanCard, type CardSession } from './library/scanner.js'
import { Store } from './library/store.js'
import { inferSessionTimes, sessionLabel } from './library/timestamps.js'
import { importSession } from './jobs/import.js'
import { JobQueue } from './jobs/queue.js'

let store: Store
let queue: JobQueue

/**
 * Anything the renderer names must live inside the library root or on a
 * volume the watcher currently reports. That makes "the card is never
 * written" a property of the code rather than a rule someone has to
 * remember, and stops a path from the renderer reaching anywhere else.
 */
function assertUnder(path: string, roots: string[]): string {
  const target = resolve(path)
  for (const root of roots) {
    const base = resolve(root)
    if (target === base || target.startsWith(base + sep)) return target
  }
  throw new Error(`refusing to touch a path outside the library or card: ${path}`)
}

function cardRoots(watcher: DeviceWatcher): string[] {
  const s = watcher.state
  const vol = 'volume' in s ? s.volume : null
  return vol ? [vol.path] : []
}

function buildView(): LibraryView {
  const sessions = store.sessions().map((s) => {
    const clips = store.clipsOf(s.id)
    const times = s.startUtc ? inferSessionTimes(clips, new Date(s.startUtc)) : []
    const timeById = new Map(times.map((t) => [t.id, t]))

    const view: LibraryClipView[] = clips.map((c) => {
      const t = timeById.get(c.id)
      return {
        id: c.id,
        dcfIndex: c.dcfIndex,
        cardName: c.cardName,
        file: c.file,
        bytes: c.bytes,
        sourceBytes: c.sourceBytes,
        health: c.health,
        frames: c.frames,
        drops: c.drops,
        droppedTornFrame: c.droppedTornFrame,
        durationSec: c.durationSec,
        startUtc: t?.startUtc ?? c.startUtc,
        gapUncertain: t?.gapUncertain ?? false
      }
    })

    return {
      id: s.id,
      dcfDir: s.dcfDir,
      label: sessionLabel(s.dcfDir, s.startUtc),
      startUtc: s.startUtc,
      clips: view,
      durationSec: clips.reduce((n, c) => n + c.durationSec, 0),
      bytes: clips.reduce((n, c) => n + c.bytes, 0)
    }
  })
  return { root: store.root, sessions }
}

function broadcast(channel: string, payload: unknown) {
  for (const w of BrowserWindow.getAllWindows())
    if (!w.isDestroyed()) w.webContents.send(channel, payload)
}

export async function registerIpc(watcher: DeviceWatcher): Promise<void> {
  const defaultRoot = join(app.getPath('videos'), 'FPVault')
  store = await Store.open(defaultRoot)
  queue = new JobQueue()
  queue.on('change', (job) => broadcast('jobs:change', job))

  /* ---- device ---- */
  ipcMain.handle('device:get', () => watcher.state)
  ipcMain.handle('device:rescan', () => watcher.rescan())

  /* ---- card ---- */
  ipcMain.handle('card:scan', async (_e, volumePath: string) => {
    assertUnder(volumePath, cardRoots(watcher))
    return scanCard(volumePath)
  })

  /* ---- library ---- */
  ipcMain.handle('library:get', () => buildView())
  ipcMain.handle('library:setSessionStart', async (_e, id: string, startUtc: string | null) => {
    await store.setSessionStart(id, startUtc)
    const view = buildView()
    broadcast('library:change', view)
    return view
  })
  ipcMain.handle('library:chooseRoot', async () => {
    const res = await dialog.showOpenDialog({
      title: 'Choose a library folder',
      properties: ['openDirectory', 'createDirectory']
    })
    const chosen = res.filePaths[0]
    if (!res.canceled && chosen) {
      store = await Store.open(chosen)
    }
    const view = buildView()
    broadcast('library:change', view)
    return view
  })
  ipcMain.handle('library:reveal', (_e, file: string) => {
    const full = assertUnder(join(store.root, file), [store.root])
    shell.showItemInFolder(full)
  })

  /* ---- jobs ---- */
  ipcMain.handle('jobs:list', () => queue.list())
  ipcMain.handle('jobs:cancel', (_e, id: string) => queue.cancel(id))
  ipcMain.handle('jobs:importSessions', async (_e, volumePath: string, dcfDirs: number[]) => {
    assertUnder(volumePath, cardRoots(watcher))
    const contents = await scanCard(volumePath)
    const wanted: CardSession[] = contents.sessions.filter((s) => dcfDirs.includes(s.dcfDir))

    return wanted.map((session) =>
      queue.add('import', `Session ${session.dcfDir}`, async (ctx) => {
        const res = await importSession(session, store, ctx)
        broadcast('library:change', buildView())
        return res
      })
    )
  })

  /* ---- app ---- */
  ipcMain.handle('app:versions', () => ({
    app: app.getVersion(),
    electron: process.versions.electron,
    node: process.versions.node,
    chrome: process.versions.chrome
  }))

  watcher.on('change', (state) => broadcast('device:change', state))
}
