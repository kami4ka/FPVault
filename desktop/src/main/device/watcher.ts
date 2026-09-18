/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Device watcher: turns the USB probe plus volume discovery into the single
 * DeviceState the UI renders.
 *
 * Two facts about the hardware shape this file:
 *
 *  - The board is bus-powered, and main() only waits 2.5 s after power-on for
 *    a host to configure it (src/main.c:113). A host can therefore only ever
 *    exist from power-on: the board cannot be talked into reader mode later,
 *    and changing its mode always means a physical replug. Nothing here
 *    should sit retrying for a mode that cannot arrive.
 *
 *  - A USB attach happens well before the OS has mounted the card, so the
 *    volume is polled for a few seconds after the board appears rather than
 *    read once and reported missing.
 *
 * There is no USB event subscription: node-usb is deliberately not a
 * dependency (see probe.ts), so state comes from a cheap poll. The probes
 * cost single-digit milliseconds on Linux and macOS.
 */
import { EventEmitter } from 'node:events'
import type { DeviceState } from '@shared/types'
import { EMPTY, probeUsb, type UsbSnapshot } from './probe.js'
import { findCardVolume } from './volumes.js'

/** Fast enough to feel instant when plugging in, cheap enough to leave on. */
const POLL_MS = 1500
/** How long to wait for the OS to mount the card after the board appears. */
const MOUNT_GRACE_MS = 8000

export class DeviceWatcher extends EventEmitter {
  private current: DeviceState = { kind: 'absent' }
  private timer: NodeJS.Timeout | null = null
  private scanning = false
  /** When the board was first seen without a mounted volume. */
  private boardSeenAt: number | null = null

  get state(): DeviceState {
    return this.current
  }

  async start(): Promise<DeviceState> {
    const first = await this.rescan()
    this.timer = setInterval(() => void this.rescan(), POLL_MS)
    return first
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer)
    this.timer = null
  }

  async rescan(): Promise<DeviceState> {
    if (this.scanning) return this.current
    this.scanning = true
    try {
      this.setState(await this.computeState())
    } catch (err) {
      console.error('[device] rescan failed:', err)
    } finally {
      this.scanning = false
    }
    return this.current
  }

  private async computeState(): Promise<DeviceState> {
    let usb: UsbSnapshot = EMPTY
    try {
      usb = await probeUsb()
    } catch {
      /* probeUsb already logs; a failed probe degrades to card-only. */
    }

    if (usb.fel) {
      this.boardSeenAt = null
      return { kind: 'fel' }
    }

    const volume = await findCardVolume()

    if (usb.board) {
      if (volume) this.boardSeenAt = null
      else this.boardSeenAt ??= Date.now()
      return usb.dfuCapable
        ? { kind: 'reader', firmware: usb.firmware, volume }
        : { kind: 'legacy', firmware: usb.firmware, volume }
    }

    this.boardSeenAt = null
    if (volume) return { kind: 'cardOnly', volume }
    return { kind: 'absent' }
  }

  /**
   * True while the board is on the bus but the card has not mounted yet and
   * the grace period has not expired — the UI shows "waiting" rather than
   * "no card" so a slow mount does not read as a fault.
   */
  get awaitingMount(): boolean {
    return this.boardSeenAt !== null && Date.now() - this.boardSeenAt < MOUNT_GRACE_MS
  }

  private setState(next: DeviceState) {
    if (JSON.stringify(next) === JSON.stringify(this.current)) return
    this.current = next
    this.emit('change', next)
  }
}
