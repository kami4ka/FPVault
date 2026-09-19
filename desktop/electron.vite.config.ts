import { defineConfig, externalizeDepsPlugin } from 'electron-vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { resolve } from 'node:path'

export default defineConfig({
  main: {
    /* externalizeDepsPlugin only externalises `dependencies`, so `usb` has
     * to be named here: it lives in optionalDependencies (the app works
     * without it — see src/main/device/probe.ts) and a native module cannot
     * be bundled. Without this the import silently resolves to inlined
     * JavaScript and firmware updates quietly stop being offered. */
    plugins: [externalizeDepsPlugin({ exclude: [] })],
    resolve: { alias: { '@shared': resolve('src/shared') } },
    build: {
      rollupOptions: {
        input: resolve('src/main/index.ts'),
        external: ['usb']
      }
    }
  },
  preload: {
    plugins: [externalizeDepsPlugin()],
    resolve: { alias: { '@shared': resolve('src/shared') } },
    build: { rollupOptions: { input: resolve('src/preload/index.ts') } }
  },
  renderer: {
    root: resolve('src/renderer'),
    plugins: [react(), tailwindcss()],
    resolve: { alias: { '@shared': resolve('src/shared') } },
    build: { rollupOptions: { input: resolve('src/renderer/index.html') } }
  }
})
