/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Main process: one window, one device watcher, a narrow IPC surface.
 */
import { app, BrowserWindow, shell } from 'electron'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { DeviceWatcher } from './device/watcher.js'
import { registerIpc } from './ipc.js'

const dirname = fileURLToPath(new URL('.', import.meta.url))
const isDev = !app.isPackaged

const watcher = new DeviceWatcher()
let win: BrowserWindow | null = null

function createWindow(): BrowserWindow {
  const w = new BrowserWindow({
    width: 1180,
    height: 780,
    minWidth: 940,
    minHeight: 620,
    show: false,
    backgroundColor: '#f7f8f9',
    titleBarStyle: process.platform === 'darwin' ? 'hiddenInset' : 'default',
    webPreferences: {
      preload: join(dirname, '../preload/index.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  w.once('ready-to-show', () => w.show())

  /* Nothing in this app should ever navigate away or open a second window;
   * external links belong in the user's browser. */
  w.webContents.setWindowOpenHandler(({ url }) => {
    void shell.openExternal(url)
    return { action: 'deny' }
  })

  if (isDev && process.env['ELECTRON_RENDERER_URL']) {
    void w.loadURL(process.env['ELECTRON_RENDERER_URL'])
  } else {
    void w.loadFile(join(dirname, '../renderer/index.html'))
  }
  return w
}

void app.whenReady().then(async () => {
  await registerIpc(watcher)
  win = createWindow()
  await watcher.start()
  /* The window may have finished loading before the first scan completed. */
  if (win && !win.isDestroyed()) win.webContents.send('device:change', watcher.state)

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) win = createWindow()
  })
})

app.on('window-all-closed', () => {
  watcher.stop()
  if (process.platform !== 'darwin') app.quit()
})
