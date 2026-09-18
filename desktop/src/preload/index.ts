/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The entire surface the renderer is allowed to reach. Keep it explicit:
 * no generic invoke(), no channel passed in from the renderer side.
 */
import { contextBridge, ipcRenderer } from 'electron'
import type { Api, DeviceState, JobState, LibraryView } from '@shared/types'

/** Subscribe to a main-process broadcast, returning an unsubscribe. */
function on<T>(channel: string, fn: (v: T) => void): () => void {
  const listener = (_e: unknown, v: T) => fn(v)
  ipcRenderer.on(channel, listener)
  return () => ipcRenderer.removeListener(channel, listener)
}

const api: Api = {
  device: {
    get: () => ipcRenderer.invoke('device:get'),
    rescan: () => ipcRenderer.invoke('device:rescan'),
    onChange: (fn) => on<DeviceState>('device:change', fn)
  },
  card: {
    scan: (volumePath) => ipcRenderer.invoke('card:scan', volumePath)
  },
  library: {
    get: () => ipcRenderer.invoke('library:get'),
    onChange: (fn) => on<LibraryView>('library:change', fn),
    setSessionStart: (id, startUtc) =>
      ipcRenderer.invoke('library:setSessionStart', id, startUtc),
    chooseRoot: () => ipcRenderer.invoke('library:chooseRoot'),
    reveal: (file) => ipcRenderer.invoke('library:reveal', file)
  },
  jobs: {
    list: () => ipcRenderer.invoke('jobs:list'),
    onChange: (fn) => on<JobState>('jobs:change', fn),
    importSessions: (volumePath, dcfDirs) =>
      ipcRenderer.invoke('jobs:importSessions', volumePath, dcfDirs),
    cancel: (id) => ipcRenderer.invoke('jobs:cancel', id)
  },
  app: {
    versions: () => ipcRenderer.invoke('app:versions')
  }
}

contextBridge.exposeInMainWorld('fpvault', api)
