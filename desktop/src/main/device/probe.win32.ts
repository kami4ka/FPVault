/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Windows probe: PnP device instance ids via PowerShell.
 *
 * This is the platform that justifies not using node-usb. A board in
 * card-reader mode is bound to `usbstor`, which libusb cannot enumerate at
 * all, so the native module would report an empty bus in the state the user
 * is in almost all the time. The PnP subsystem sees everything regardless of
 * which driver owns it.
 *
 * A composite device appears as a parent plus one child per interface, with
 * instance ids ending `&MI_00`, `&MI_01`. So the presence of the `&MI_01`
 * child *is* the DFU-capability test, and it works with no driver bound.
 *
 * The child's Service also tells us whether dfu-util will be able to open
 * it: WinUSB (or libusbK) means yes, anything else means the driver still
 * has to be bound.
 */
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { decodeVersion, EMPTY, type UsbSnapshot } from './probe.js'

const run = promisify(execFile)

/* Emit one JSON array so Node does the parsing, not a regex. ConvertTo-Json
 * collapses a single object, so -AsArray keeps the shape stable. */
const SCRIPT = `
$ErrorActionPreference='SilentlyContinue'
Get-PnpDevice -PresentOnly |
  Where-Object { $_.InstanceId -like 'USB\\VID_34B7&PID_F1C2*' -or $_.InstanceId -like 'USB\\VID_1F3A&PID_EFE8*' } |
  Select-Object InstanceId, Service, Class, HardwareID |
  ConvertTo-Json -Compress -AsArray
`

interface PnpRow {
  InstanceId: string
  Service: string | null
  Class: string | null
  /** e.g. ["USB\\VID_34B7&PID_F1C2&REV_0093", …] */
  HardwareID: string[] | string | null
}

/** Drivers that let libusb (so dfu-util and sunxi-fel) open the interface. */
const USABLE = /^(winusb|libusbk|libusb0)$/i

export async function probe(): Promise<UsbSnapshot> {
  const snap: UsbSnapshot = { ...EMPTY, firmware: { ...EMPTY.firmware } }

  const { stdout } = await run(
    'powershell.exe',
    ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', SCRIPT],
    { timeout: 10_000, windowsHide: true, maxBuffer: 4 * 1024 * 1024 }
  )

  let rows: PnpRow[]
  try {
    rows = JSON.parse(stdout.trim() || '[]')
  } catch {
    return snap
  }

  let dfuChild: PnpRow | null = null
  let felDevice: PnpRow | null = null

  for (const row of rows) {
    const id = row.InstanceId.toUpperCase()

    if (id.includes('VID_1F3A&PID_EFE8')) {
      snap.fel = true
      felDevice = row
      continue
    }
    if (!id.includes('VID_34B7&PID_F1C2')) continue

    snap.board = true

    /* bcdDevice is not a Get-PnpDevice property, but Windows puts it in the
     * hardware ID as REV_xxxx — which is the firmware version from v0.9.3
     * onwards (src/board.h FW_VERSION_BCD). */
    const hw = Array.isArray(row.HardwareID) ? row.HardwareID.join(' ') : (row.HardwareID ?? '')
    const rev = /REV_([0-9A-F]{4})/i.exec(hw)
    if (rev?.[1] && snap.firmware.bcdDevice === 0)
      snap.firmware.bcdDevice = parseInt(rev[1], 16)

    /* &MI_01 is the DFU interface; its mere presence is the capability. */
    if (id.includes('&MI_01')) {
      snap.dfuCapable = true
      dfuChild = row
    }

    /* The instance id's last segment is the serial for a device that reports
     * one; the firmware's is the constant "00000001". */
    const tail = id.split('\\').pop()
    if (tail && !tail.includes('&') && !snap.firmware.serial) snap.firmware.serial = tail
  }

  snap.firmware.version = decodeVersion(snap.firmware.bcdDevice, snap.dfuCapable)

  if (snap.fel && felDevice && !USABLE.test(felDevice.Service ?? '')) {
    snap.driverNeeded = 'fel'
  } else if (snap.dfuCapable && dfuChild && !USABLE.test(dfuChild.Service ?? '')) {
    snap.driverNeeded = 'dfu'
  }

  return snap
}
