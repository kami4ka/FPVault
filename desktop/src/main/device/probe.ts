/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB presence probing, without a native module.
 *
 * The obvious choice here is node-usb, and it is the wrong one. libusb's
 * Windows backend only enumerates devices already bound to WinUSB, libusbK
 * or libusb0 — and a board in card-reader mode is bound to `usbstor`. So on
 * Windows node-usb reports *nothing* for the single most common state, which
 * is precisely where detection has to work. Add the per-Electron-ABI rebuilds
 * a native addon costs across four targets and it earns its place nowhere:
 * we never speak USB ourselves, we only ask "is it there, and does it have
 * the DFU interface". dfu-util and sunxi-fel bring their own libusb for the
 * parts that do need to talk.
 *
 * So: three small OS-native probes, all pure Node.
 */
import type { FirmwareId } from '@shared/types'

export interface UsbSnapshot {
  /** A board (34b7:f1c2) is on the bus. */
  board: boolean
  /** That board exposes the DFU interface — v0.9.2 or newer. */
  dfuCapable: boolean
  /** The Allwinner boot ROM (1f3a:efe8) is on the bus. */
  fel: boolean
  firmware: FirmwareId
  /**
   * Windows only: the device is present but no WinUSB-class driver is bound
   * to the interface we would need, so dfu-util/sunxi-fel cannot open it.
   */
  driverNeeded: 'dfu' | 'fel' | null
}

export const EMPTY: UsbSnapshot = {
  board: false,
  dfuCapable: false,
  fel: false,
  firmware: { bcdDevice: 0, serial: null, version: null },
  driverNeeded: null
}

/**
 * bcdDevice is hardcoded 0x0100 in src/usbmsc.c and carries no version, so
 * that value tells us nothing and must not be shown as one. Once firmware
 * encodes the release there (0x0092 for v0.9.2), decode it.
 */
export function decodeVersion(bcdDevice: number): string | null {
  if (bcdDevice === 0 || bcdDevice === 0x0100) return null
  const major = (bcdDevice >> 8) & 0xff
  const minor = (bcdDevice >> 4) & 0x0f
  const patch = bcdDevice & 0x0f
  return `${major}.${minor}.${patch}`
}

export type Probe = () => Promise<UsbSnapshot>

let cached: Probe | null = null

export async function probeUsb(): Promise<UsbSnapshot> {
  if (!cached) {
    const mod =
      process.platform === 'darwin'
        ? await import('./probe.darwin.js')
        : process.platform === 'win32'
          ? await import('./probe.win32.js')
          : await import('./probe.linux.js')
    cached = mod.probe
  }
  try {
    return await cached()
  } catch (err) {
    console.error('[device] USB probe failed:', err)
    return EMPTY
  }
}
