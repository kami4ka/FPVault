/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The entire surface the renderer is allowed to reach. Keep it explicit:
 * no generic invoke(), no channel passed in from the renderer side.
 */
import { contextBridge, ipcRenderer } from 'electron'
import type { Api, DeviceState } from '@shared/types'

const api: Api = {
  device: {
    get: () => ipcRenderer.invoke('device:get'),
    rescan: () => ipcRenderer.invoke('device:rescan'),
    onChange: (fn: (s: DeviceState) => void) => {
      const listener = (_e: unknown, s: DeviceState) => fn(s)
      ipcRenderer.on('device:change', listener)
      return () => ipcRenderer.removeListener('device:change', listener)
    }
  },
  app: {
    versions: () => ipcRenderer.invoke('app:versions')
  }
}

contextBridge.exposeInMainWorld('fpvault', api)
