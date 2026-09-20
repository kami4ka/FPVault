/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every IPC handler, and the one guard that keeps the card safe.
 */
import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron'
import { join, relative, resolve, sep } from 'node:path'
import { defaultLibraryRoot } from './paths.js'
import { loadPrefs, prefs, savePrefs, QUALITY } from './settings.js'
import { bundledTools, licences } from './tools/bundled.js'
import type { ExportFile, LibraryClipView, LibraryView, Prefs } from '@shared/types'
import type { DeviceWatcher } from './device/watcher.js'
import { frameTable, readFrame, type FrameTable } from '@shared/avi/frames'
import { readdir, stat } from 'node:fs/promises'
import { scanCard, type CardSession } from './library/scanner.js'
import { Store } from './library/store.js'
import { inferSessionTimes, sessionLabel } from './library/timestamps.js'
import { importSession } from './jobs/import.js'
import { joinSession } from './jobs/join.js'
import { exportSessionMp4 } from './jobs/export.js'
import { ffmpegAvailable } from './tools/ffmpeg.js'
import { flashRelease } from './jobs/firmware.js'
import { dfuAvailable } from './update/dfu.js'
import { listReleases } from './update/releases.js'
import { felAvailable, recoverOverFel } from './jobs/recover.js'
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

/** Describe one export file, or null when it is no longer on disk. */
async function describeExport(rel: string): Promise<ExportFile | null> {
  const lower = rel.toLowerCase()
  const kind: ExportFile['kind'] = lower.endsWith('.avi')
    ? 'avi'
    : lower.endsWith('.mp4')
      ? 'mp4'
      : 'other'
  if (kind === 'other') return null
  try {
    const st = await stat(join(store.root, rel))
    return {
      file: rel,
      name: rel.split('/').pop() ?? rel,
      bytes: st.size,
      createdMs: st.mtimeMs,
      kind
    }
  } catch {
    return null
  }
}

/**
 * Exports a session owns, newest first.
 *
 * The store is the record, but files that predate it — or a library rebuilt
 * by scanning — are adopted by matching the name the exporter would have
 * produced, which is the session's label with the unsafe characters
 * replaced. That only guesses when there is nothing better to go on.
 */
async function exportsOf(sessionId: string, label: string): Promise<ExportFile[]> {
  const session = store.sessions().find((x) => x.id === sessionId)
  const recorded = new Set(session?.exportFiles ?? [])

  const safe = label.replace(/[^\w.-]+/g, '-').replace(/^-+|-+$/g, '') || sessionId
  try {
    for (const name of await readdir(join(store.root, 'exports'))) {
      if (name.startsWith('.') || name.endsWith('.part')) continue
      const base = name.replace(/\.[^.]+$/, '')
      if (base === safe) recorded.add(`exports/${name}`)
    }
  } catch {
    /* no exports directory yet */
  }

  const described = await Promise.all([...recorded].map(describeExport))
  return described
    .filter((e): e is ExportFile => e !== null)
    .sort((a, b) => b.createdMs - a.createdMs)
}

/** Per-session exports, refreshed whenever the library is published. */
let exportsIndex = new Map<string, ExportFile[]>()

function buildView(): LibraryView {
  const sessions = store.sessions().map((s) => {
    const clips = store.clipsOf(s.id)
    const times = s.startUtc
      ? inferSessionTimes(clips, new Date(s.startUtc), prefs().gapSeconds)
      : []
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
      exports: exportsIndex.get(s.id) ?? [],
      durationSec: clips.reduce((n, c) => n + c.durationSec, 0),
      bytes: clips.reduce((n, c) => n + c.bytes, 0)
    }
  })
  return { root: store.root, sessions }
}

async function refreshExports() {
  const next = new Map<string, ExportFile[]>()
  for (const session of store.sessions()) {
    next.set(session.id, await exportsOf(session.id, sessionLabel(session.dcfDir, session.startUtc)))
  }
  exportsIndex = next
}

/** Refresh what each session has produced, then publish the view. */
async function publishLibrary() {
  await refreshExports()
  broadcast('library:change', buildView())
}

function broadcast(channel: string, payload: unknown) {
  for (const w of BrowserWindow.getAllWindows())
    if (!w.isDestroyed()) w.webContents.send(channel, payload)
}

export async function registerIpc(watcher: DeviceWatcher): Promise<void> {
  const saved = await loadPrefs()
  store = await Store.open(saved.libraryRoot ?? defaultLibraryRoot())
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
  ipcMain.handle('library:get', async () => {
    await refreshExports()
    return buildView()
  })
  ipcMain.handle('library:setSessionStart', async (_e, id: string, startUtc: string | null) => {
    await store.setSessionStart(id, startUtc)
    await refreshExports()
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
      await savePrefs({ libraryRoot: chosen })
    }
    await refreshExports()
    const view = buildView()
    broadcast('library:change', view)
    return view
  })
  ipcMain.handle('library:reveal', (_e, file: string) => {
    const full = assertUnder(join(store.root, file), [store.root])
    shell.showItemInFolder(full)
  })
  ipcMain.handle('library:open', (_e, file: string) => {
    const full = assertUnder(join(store.root, file), [store.root])
    return shell.openPath(full)
  })

  /* ---- clip media ----
   * A clip's seek table is cached: opening the player then scrubbing it
   * would otherwise re-read the index on every single frame. */
  const tables = new Map<string, { path: string; table: FrameTable }>()

  const tableFor = async (clipId: string) => {
    const hit = tables.get(clipId)
    if (hit) return hit
    /* A clip id, or a library-relative path so a joined export plays in the
     * same viewer. Either way the path guard is what decides. */
    const clip = store.snapshot().clips[clipId]
    const rel = clip?.file ?? clipId
    const path = assertUnder(join(store.root, rel), [store.root])
    const table = await frameTable(path)
    if (!table) return null
    const entry = { path, table }
    tables.set(clipId, entry)
    return entry
  }

  ipcMain.handle('clip:media', async (_e, clipId: string) => {
    const entry = await tableFor(clipId)
    if (!entry) return null
    const { table } = entry
    const fps = table.scale ? table.rate / table.scale : 0
    return {
      frames: table.frames.length,
      width: table.width,
      height: table.height,
      fps,
      durationSec: fps ? table.frames.length / fps : 0
    }
  })

  ipcMain.handle('clip:frame', async (_e, clipId: string, index: number) => {
    const entry = await tableFor(clipId)
    if (!entry) return new Uint8Array()
    return new Uint8Array(await readFrame(entry.path, entry.table, index))
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

  ipcMain.handle('jobs:joinSession', (_e, sessionId: string) => {
    const session = store.sessions().find((x) => x.id === sessionId)
    if (!session) throw new Error(`no such session: ${sessionId}`)
    const label = sessionLabel(session.dcfDir, session.startUtc)
    return queue.add('join', label, async (ctx) => {
      const res = await joinSession(sessionId, label, store, ctx)
      await store.addExport(sessionId, relative(store.root, res.output))
      await publishLibrary()
      return res
    })
  })

  ipcMain.handle('jobs:exportSession', (_e, sessionId: string) => {
    const session = store.sessions().find((x) => x.id === sessionId)
    if (!session) throw new Error(`no such session: ${sessionId}`)
    const label = sessionLabel(session.dcfDir, session.startUtc)
    const p = prefs()
    const q = QUALITY[p.quality]
    return queue.add('export', `${label} (MP4)`, async (ctx) => {
      const res = await exportSessionMp4(sessionId, label, store, ctx, {
        deinterlace: p.deinterlace,
        crf: q.crf,
        preset: q.preset
      })
      await store.addExport(sessionId, relative(store.root, res.output))
      await publishLibrary()
      return res
    })
  })

  /* ---- firmware ---- */
  ipcMain.handle('firmware:releases', async (_e, force?: boolean) => {
    const releases = await listReleases(force ?? false)
    return releases.map((r) => ({
      tag: r.tag,
      name: r.name,
      prerelease: r.prerelease,
      publishedAt: r.publishedAt,
      notes: r.notes,
      hasFirmware: r.assets.some((a) => a.name === 'fpvault.bin'),
      hasUboot: r.assets.some((a) => a.name === 'u-boot-sunxi-with-spl.bin')
    }))
  })
  ipcMain.handle('firmware:canFlash', () => dfuAvailable())
  ipcMain.handle('firmware:flash', (_e, tag: string) =>
    queue.add('firmware', `Firmware ${tag}`, (ctx) => flashRelease(tag, ctx))
  )

  ipcMain.handle('firmware:canRecover', () => felAvailable())
  ipcMain.handle('firmware:recover', (_e, tag: string, withUboot: boolean) =>
    queue.add('recover', `Recovery ${tag}`, (ctx) => recoverOverFel(tag, withUboot, ctx))
  )

  /* ---- settings ---- */
  ipcMain.handle('settings:get', () => loadPrefs())
  ipcMain.handle('settings:set', async (_e, patch: Partial<Prefs>) => {
    const before = prefs().gapSeconds
    /* The library root moves through library:chooseRoot, which has to open
     * the new store as well as remember it; accepting it here too would let
     * the renderer point the store somewhere without that happening. */
    const { libraryRoot: _ignored, ...rest } = patch
    const next = await savePrefs(rest)
    /* Timestamps are derived, so a changed gap changes every session that
     * has a start time. Republish rather than make the user reopen a tab. */
    if (next.gapSeconds !== before) await publishLibrary()
    return next
  })

  /* ---- app ---- */
  ipcMain.handle('app:canExport', () => ffmpegAvailable())
  ipcMain.handle('app:versions', () => ({
    app: app.getVersion(),
    electron: process.versions.electron,
    node: process.versions.node,
    chrome: process.versions.chrome
  }))
  ipcMain.handle('app:tools', () => bundledTools())
  ipcMain.handle('app:licences', () => licences(app.getVersion()))
  /* Only ever a web page, and only ever https: this hands a string to the
   * OS, so a file: or a custom scheme would be someone else's program. */
  ipcMain.handle('app:openUrl', (_e, url: string) => {
    if (!/^https:\/\//i.test(url)) throw new Error(`refusing to open: ${url}`)
    return shell.openExternal(url)
  })

  watcher.on('change', (state) => broadcast('device:change', state))
}
