/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * macOS probe: one `ioreg` call over USB interface nodes.
 *
 * Interface nodes are used rather than device nodes because they answer both
 * questions at once — which devices are present, and whether the board is
 * exposing the DFU interface. Verified against a live board: interface 0 is
 * class 8 (mass storage) bound to IOUSBMassStorageDriver, interface 1 is
 * class 254 (DFU) with no driver bound at all, which is exactly why dfu-util
 * can claim it without detaching anything.
 *
 * The `-a` plist output would be tidier but contains data blobs that
 * `plutil -convert json` refuses, so the text form is parsed instead.
 */
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { DFU_CLASS, FEL_PID, FEL_VID, FPVAULT_PID, FPVAULT_VID } from '@shared/types'
import { decodeVersion, EMPTY, type UsbSnapshot } from './probe.js'

const run = promisify(execFile)

/** One `+-o` node's flat properties, as far as we care about them. */
interface Node {
  idVendor?: number
  idProduct?: number
  bInterfaceClass?: number
  bcdDevice?: number
  serial?: string
}

const NUM = /^\s*[|\s]*"(idVendor|idProduct|bInterfaceClass|bcdDevice)"\s*=\s*(\d+)/
const SERIAL = /"kUSBSerialNumberString"\s*=\s*"([^"]*)"/

/**
 * ioreg prints a tree; each `+-o` line starts a node and the indented
 * "key" = value lines below it belong to that node. Nested nodes inherit
 * nothing, so a flat split on `+-o` is the correct grouping.
 */
function parseNodes(text: string): Node[] {
  const nodes: Node[] = []
  let current: Node | null = null

  for (const line of text.split('\n')) {
    if (line.includes('+-o ')) {
      if (current) nodes.push(current)
      current = {}
      continue
    }
    if (!current) continue

    const num = NUM.exec(line)
    if (num?.[1] && num[2]) {
      current[num[1] as 'idVendor'] = Number(num[2])
      continue
    }
    // The serial only appears inside the "USB Device Info" blob.
    const ser = SERIAL.exec(line)
    if (ser?.[1] !== undefined) current.serial = ser[1]
  }
  if (current) nodes.push(current)
  return nodes
}

export async function probe(): Promise<UsbSnapshot> {
  const { stdout } = await run(
    'ioreg',
    ['-r', '-c', 'IOUSBHostInterface', '-l', '-w0'],
    { maxBuffer: 16 * 1024 * 1024, timeout: 5000 }
  )
  const nodes = parseNodes(stdout)
  const snap: UsbSnapshot = { ...EMPTY, firmware: { ...EMPTY.firmware } }

  for (const n of nodes) {
    if (n.idVendor === FEL_VID && n.idProduct === FEL_PID) {
      snap.fel = true
      continue
    }
    if (n.idVendor !== FPVAULT_VID || n.idProduct !== FPVAULT_PID) continue

    snap.board = true
    if (n.bInterfaceClass === DFU_CLASS) snap.dfuCapable = true
    if (n.bcdDevice !== undefined && snap.firmware.bcdDevice === 0) {
      snap.firmware.bcdDevice = n.bcdDevice
      snap.firmware.version = decodeVersion(n.bcdDevice)
    }
    if (n.serial && !snap.firmware.serial) snap.firmware.serial = n.serial
  }

  /* A FEL device has no driver, so it may not surface as an interface node.
   * Fall back to the device-level tree only when nothing was found, so the
   * common path stays a single spawn. */
  if (!snap.fel && !snap.board) {
    const { stdout: devs } = await run('ioreg', ['-r', '-c', 'IOUSBHostDevice', '-l', '-w0'], {
      maxBuffer: 16 * 1024 * 1024,
      timeout: 5000
    })
    for (const n of parseNodes(devs)) {
      if (n.idVendor === FEL_VID && n.idProduct === FEL_PID) snap.fel = true
      if (n.idVendor === FPVAULT_VID && n.idProduct === FPVAULT_PID) snap.board = true
    }
  }

  return snap
}
