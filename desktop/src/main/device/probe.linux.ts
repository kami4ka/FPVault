/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linux probe: read sysfs directly. No tools, no permissions, no parsing of
 * anyone's output format — /sys/bus/usb/devices/<dev>/idVendor is a file
 * containing four hex digits, and <dev>:<cfg>.<iface>/bInterfaceClass is the
 * interface class. It is the fastest and most reliable of the three probes.
 */
import { readdir, readFile } from 'node:fs/promises'
import { join } from 'node:path'
import { DFU_CLASS, FEL_PID, FEL_VID, FPVAULT_PID, FPVAULT_VID } from '@shared/types'
import { decodeVersion, EMPTY, type UsbSnapshot } from './probe.js'

const SYSFS = '/sys/bus/usb/devices'

async function readHex(path: string): Promise<number | null> {
  try {
    return parseInt((await readFile(path, 'utf8')).trim(), 16)
  } catch {
    return null
  }
}

async function readText(path: string): Promise<string | null> {
  try {
    return (await readFile(path, 'utf8')).trim()
  } catch {
    return null
  }
}

export async function probe(): Promise<UsbSnapshot> {
  const snap: UsbSnapshot = { ...EMPTY, firmware: { ...EMPTY.firmware } }

  let entries: string[]
  try {
    entries = await readdir(SYSFS)
  } catch {
    return snap
  }

  for (const name of entries) {
    /* Interface directories are "<dev>:<cfg>.<iface>"; device directories are
     * not. Only device directories carry idVendor. */
    if (name.includes(':')) continue
    const dir = join(SYSFS, name)

    const vid = await readHex(join(dir, 'idVendor'))
    const pid = await readHex(join(dir, 'idProduct'))
    if (vid === null || pid === null) continue

    if (vid === FEL_VID && pid === FEL_PID) {
      snap.fel = true
      continue
    }
    if (vid !== FPVAULT_VID || pid !== FPVAULT_PID) continue

    snap.board = true
    const bcd = await readHex(join(dir, 'bcdDevice'))
    if (bcd !== null) snap.firmware.bcdDevice = bcd
    snap.firmware.serial = await readText(join(dir, 'serial'))

    /* This device's interfaces are the sibling directories prefixed with its
     * own name plus a colon. */
    for (const iface of entries.filter((e) => e.startsWith(`${name}:`))) {
      const cls = await readHex(join(SYSFS, iface, 'bInterfaceClass'))
      if (cls === DFU_CLASS) snap.dfuCapable = true
    }

    /* Decoding needs the DFU answer: the legacy 0x0100 sentinel is only
     * unambiguous alongside it. */
    snap.firmware.version = decodeVersion(snap.firmware.bcdDevice, snap.dfuCapable)
  }

  return snap
}
