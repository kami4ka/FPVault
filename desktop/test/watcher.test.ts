/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The watcher's one genuinely subtle rule: a probe that could not answer is
 * not evidence that the board went away.
 *
 * This was a real defect, found by running the packaged app rather than by
 * reading it. macOS evaluates a bundle's signature on first launch, which
 * stalls spawning long enough for `ioreg` to hit its timeout, and the probe
 * reported that failure as an empty result. The app then announced "No board
 * detected" with a board plugged in and a card mounted — the worst wrong
 * answer it has, since every screen keys off this state.
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'
import type { CardVolume } from '../src/shared/types.js'
/* Type-only, so it is erased and does not defeat the mock below. */
import type { UsbSnapshot } from '../src/main/device/probe.js'

const EMPTY: UsbSnapshot = {
  board: false,
  dfuCapable: false,
  fel: false,
  firmware: { bcdDevice: 0, serial: null, version: null },
  driverNeeded: null,
  ok: true
}

let snapshot: UsbSnapshot = { ...EMPTY }
let volume: CardVolume | null = null

vi.mock('../src/main/device/probe.js', () => ({
  EMPTY,
  probeUsb: () => Promise.resolve(snapshot)
}))

vi.mock('../src/main/device/volumes.js', () => ({
  findCardVolume: () => Promise.resolve(volume)
}))

const { DeviceWatcher } = await import('../src/main/device/watcher.js')

const board: UsbSnapshot = {
  ...EMPTY,
  board: true,
  dfuCapable: true,
  firmware: { bcdDevice: 0x0093, serial: null, version: '0.9.3' }
}

const card: CardVolume = {
  path: '/Volumes/F1CSD',
  label: 'F1CSD',
  totalBytes: null,
  freeBytes: null,
  sessions: ['129FCDVR']
}

describe('device watcher', () => {
  beforeEach(() => {
    snapshot = { ...EMPTY }
    volume = null
  })

  it('reports a board with a DFU interface as a reader', async () => {
    snapshot = board
    volume = card
    const w = new DeviceWatcher()
    expect((await w.rescan()).kind).toBe('reader')
  })

  it('holds the last state when a probe cannot answer', async () => {
    snapshot = board
    volume = card
    const w = new DeviceWatcher()
    expect((await w.rescan()).kind).toBe('reader')

    /* The exact shape a timed-out ioreg produces: nothing found, and no
     * claim that nothing is there. */
    snapshot = { ...EMPTY, ok: false }
    expect((await w.rescan()).kind).toBe('reader')
    expect((await w.rescan()).kind).toBe('reader')
  })

  it('gives up the held state rather than insisting forever', async () => {
    snapshot = board
    const w = new DeviceWatcher()
    await w.rescan()

    snapshot = { ...EMPTY, ok: false }
    for (let i = 0; i < 8; i++) await w.rescan()
    expect(w.state.kind).toBe('reader')

    expect((await w.rescan()).kind).toBe('absent')
  })

  it('still reports an absent board when the probe genuinely found nothing', async () => {
    snapshot = board
    const w = new DeviceWatcher()
    await w.rescan()

    snapshot = { ...EMPTY }
    expect((await w.rescan()).kind).toBe('absent')
  })

  it('forgives a blind spell once the probe answers again', async () => {
    snapshot = board
    const w = new DeviceWatcher()
    await w.rescan()

    snapshot = { ...EMPTY, ok: false }
    for (let i = 0; i < 6; i++) await w.rescan()
    snapshot = board
    await w.rescan()

    /* The counter reset, so a second spell gets the full allowance again. */
    snapshot = { ...EMPTY, ok: false }
    for (let i = 0; i < 8; i++) await w.rescan()
    expect(w.state.kind).toBe('reader')
  })

  it('reports a card in a native reader when no board is on the bus', async () => {
    snapshot = { ...EMPTY }
    volume = card
    const w = new DeviceWatcher()
    expect((await w.rescan()).kind).toBe('cardOnly')
  })
})
