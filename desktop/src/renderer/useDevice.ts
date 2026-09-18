/* SPDX-License-Identifier: GPL-3.0-or-later */
import { useEffect, useState } from 'react'
import type { Api, DeviceState } from '@shared/types'

declare global {
  interface Window {
    fpvault: Api
  }
}

export function useDevice(): { state: DeviceState; rescan: () => void; busy: boolean } {
  const [state, setState] = useState<DeviceState>({ kind: 'absent' })
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    let alive = true
    void window.fpvault.device.get().then((s) => {
      if (alive) setState(s)
    })
    const off = window.fpvault.device.onChange((s) => {
      if (alive) setState(s)
    })
    return () => {
      alive = false
      off()
    }
  }, [])

  const rescan = () => {
    setBusy(true)
    void window.fpvault.device
      .rescan()
      .then(setState)
      .finally(() => setBusy(false))
  }

  return { state, rescan, busy }
}
