/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware update over USB, speaking DFU 1.1 directly.
 *
 * This is the plain DFU dialect the board implements in src/usbdfu.c, not
 * ST's DfuSe: blocks arrive as DNLOAD control writes with an increasing
 * block number, a zero-length DNLOAD ends the image, and the host polls
 * GETSTATUS through manifestation while the board burns its flash.
 *
 * Implementing it here rather than bundling dfu-util means real byte-level
 * progress instead of scraping a progress bar off another process's stderr,
 * proper cancellation, and one less 80 MB executable per platform.
 *
 * node-usb is an *optional* dependency and is used only for this. Device
 * detection deliberately does not touch it (see device/probe.ts), so a
 * machine where the native module will not load still imports, repairs and
 * plays clips - it just cannot flash firmware over USB, and the app says so
 * and offers FEL instead.
 */
import { DFU_CLASS, DFU_PROTOCOL_MODE, DFU_SUBCLASS, FPVAULT_PID, FPVAULT_VID } from '@shared/types'

/** src/usbdfu.h: wTransferSize in the DFU functional descriptor. */
export const DFU_XFER_SIZE = 4096

/* DFU 1.1 requests (src/usbdfu.c:19-25). */
const DNLOAD = 1
const GETSTATUS = 3
const CLRSTATUS = 4
const ABORT = 6

/* DFU states, named as the spec does (src/usbdfu.c:28-35). */
export const enum DfuState {
  Idle = 2,
  DownloadSync = 3,
  DownloadBusy = 4,
  DownloadIdle = 5,
  ManifestSync = 6,
  Manifest = 7,
  ManifestWaitReset = 8,
  Error = 10
}

const STATUS_TEXT: Record<number, string> = {
  0x00: 'ok',
  0x02: 'the board rejected the file',
  0x03: 'the board could not write its flash',
  0x04: 'the board could not erase its flash',
  0x08: 'the board rejected the address',
  0x09: 'the transfer was not finished',
  0x0a: 'the board judged the firmware invalid',
  0x0f: 'the board received an unexpected packet'
}

export interface DfuProgress {
  bytesSent: number
  bytesTotal: number
  phase: 'sending' | 'burning' | 'rebooting'
}

export class DfuUnavailable extends Error {
  constructor(reason: string) {
    super(reason)
    this.name = 'DfuUnavailable'
  }
}

/** Minimal shape of the WebUSB device node-usb hands back. */
interface UsbDevice {
  open(): Promise<void>
  close(): Promise<void>
  selectConfiguration(n: number): Promise<void>
  claimInterface(n: number): Promise<void>
  releaseInterface(n: number): Promise<void>
  configuration?: { configurationValue: number } | undefined
  configurations: {
    configurationValue: number
    interfaces: {
      interfaceNumber: number
      alternates: {
        interfaceClass: number
        interfaceSubclass: number
        interfaceProtocol: number
      }[]
    }[]
  }[]
  controlTransferIn(
    setup: Record<string, unknown>,
    length: number
  ): Promise<{ status: string; data?: DataView }>
  controlTransferOut(
    setup: Record<string, unknown>,
    data?: Uint8Array
  ): Promise<{ status: string }>
}

async function loadUsb(): Promise<{ findDeviceByIds(v: number, p: number): Promise<UsbDevice | undefined> }> {
  try {
    const mod = (await import('usb')) as unknown as {
      usb: { findDeviceByIds(v: number, p: number): Promise<UsbDevice | undefined> }
    }
    return mod.usb
  } catch (err) {
    throw new DfuUnavailable(
      `USB access is unavailable on this machine (${err instanceof Error ? err.message : String(err)})`
    )
  }
}

/** The interface number carrying class FE / subclass 01 / protocol 02. */
function findDfuInterface(dev: UsbDevice): number | null {
  for (const cfg of dev.configurations)
    for (const itf of cfg.interfaces)
      for (const alt of itf.alternates)
        if (
          alt.interfaceClass === DFU_CLASS &&
          alt.interfaceSubclass === DFU_SUBCLASS &&
          alt.interfaceProtocol === DFU_PROTOCOL_MODE
        )
          return itf.interfaceNumber
  return null
}

interface DfuStatus {
  status: number
  pollMs: number
  state: number
}

async function getStatus(dev: UsbDevice, iface: number): Promise<DfuStatus> {
  const res = await dev.controlTransferIn(
    { requestType: 'class', recipient: 'interface', request: GETSTATUS, value: 0, index: iface },
    6
  )
  const d = res.data
  if (!d || d.byteLength < 6) throw new Error('the board returned a short DFU status')
  return {
    status: d.getUint8(0),
    pollMs: d.getUint8(1) | (d.getUint8(2) << 8) | (d.getUint8(3) << 16),
    state: d.getUint8(4)
  }
}

function assertOk(st: DfuStatus): void {
  if (st.status !== 0)
    throw new Error(STATUS_TEXT[st.status] ?? `the board reported DFU status 0x${st.status.toString(16)}`)
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

/**
 * The same checks src/usbdfu.c:60 makes, done before a byte is sent so a bad
 * file produces a clear message instead of an opaque rejection.
 */
export function validateImage(image: Buffer): string | null {
  if (image.length < 4096) return 'that file is too small to be FPVault firmware'
  if (image.length > 0x40000)
    return `that file is ${image.length} bytes; the board's firmware slot holds ${0x40000}`
  if (image[3] !== 0xea)
    return 'that file does not start with the ARM branch a FPVault image begins with'
  return null
}

/**
 * Send an image to a board that is already running DFU-capable firmware.
 *
 * Nothing touches the board's flash until the whole image has arrived: the
 * firmware stages it in DRAM, validates it, and only then erases, programs,
 * reads back and compares (src/usbdfu.c). So a cable pulled mid-transfer is
 * harmless; the vulnerable window is the second or so of the burn itself.
 */
export async function dfuDownload(
  image: Buffer,
  onProgress?: (p: DfuProgress) => void,
  shouldCancel?: () => void
): Promise<void> {
  const bad = validateImage(image)
  if (bad) throw new Error(bad)

  const usb = await loadUsb()
  const dev = await usb.findDeviceByIds(FPVAULT_VID, FPVAULT_PID)
  if (!dev) throw new DfuUnavailable('no FPVault board is connected')

  await dev.open()
  let claimed: number | null = null
  try {
    if (!dev.configuration) await dev.selectConfiguration(1)

    const iface = findDfuInterface(dev)
    if (iface === null)
      throw new DfuUnavailable(
        'this board has no DFU interface, so its firmware predates v0.9.2 and must be recovered over FEL first'
      )

    await dev.claimInterface(iface)
    claimed = iface

    /* Put the board back to a known state before sending anything.
     *
     * This matters more than it looks. The firmware only resets its block
     * counter when a DNLOAD arrives in dfuIDLE (src/usbdfu.c:71), and its
     * state survives the host closing the interface — so an update that was
     * interrupted leaves the board in dfuDNLOAD-IDLE expecting block N, and
     * the next attempt's block 0 is rejected as a bad address. Without this,
     * one cancelled update would poison every later one until a replug.
     */
    const put = (request: number) =>
      dev.controlTransferOut(
        { requestType: 'class', recipient: 'interface', request, value: 0, index: iface },
        /* node-usb's WebUSB layer dereferences the data argument even for a
         * zero-length request, so it must be an empty array, not omitted. */
        new Uint8Array(0)
      )

    let st = await getStatus(dev, iface)
    if (st.state === DfuState.Error) {
      await put(CLRSTATUS)
      st = await getStatus(dev, iface)
    }
    if (st.state !== DfuState.Idle) {
      await put(ABORT)
      st = await getStatus(dev, iface)
    }
    assertOk(st)
    if (st.state !== DfuState.Idle)
      throw new Error(
        `the board is in DFU state ${st.state} and will not return to idle; unplug it, wait five seconds and plug it back in`
      )

    /* Blocks, numbered from zero, exactly as the firmware expects. */
    let block = 0
    for (let sent = 0; sent < image.length; sent += DFU_XFER_SIZE) {
      shouldCancel?.()
      const chunk = image.subarray(sent, Math.min(sent + DFU_XFER_SIZE, image.length))
      const out = await dev.controlTransferOut(
        {
          requestType: 'class',
          recipient: 'interface',
          request: DNLOAD,
          value: block,
          index: iface
        },
        /* A fresh Uint8Array over its own buffer: a Buffer view into a
         * pooled allocation would send the wrong bytes. */
        Uint8Array.from(chunk)
      )
      if (out.status !== 'ok') throw new Error(`the board refused block ${block}`)

      /* GETSTATUS is what advances the firmware's state machine from
       * DNLOAD_SYNC back to DNLOAD_IDLE, so it is required, not optional. */
      const s = await getStatus(dev, iface)
      assertOk(s)
      if (s.pollMs) await sleep(s.pollMs)

      block++
      onProgress?.({
        bytesSent: Math.min(sent + chunk.length, image.length),
        bytesTotal: image.length,
        phase: 'sending'
      })
    }

    /* Zero-length DNLOAD: end of image. */
    await dev.controlTransferOut(
      {
        requestType: 'class',
        recipient: 'interface',
        request: DNLOAD,
        value: block,
        index: iface
      },
      new Uint8Array(0)
    )

    onProgress?.({ bytesSent: image.length, bytesTotal: image.length, phase: 'burning' })

    /* Poll through manifestation. The board erases, programs, reads back and
     * compares here; when it reaches MANIFEST-WAIT-RESET it reboots itself,
     * which is why the device disappearing is success, not failure. */
    const deadline = Date.now() + 30_000
    for (;;) {
      let s: DfuStatus
      try {
        s = await getStatus(dev, iface)
      } catch {
        /* The board rebooted out from under us: that is the happy ending. */
        onProgress?.({ bytesSent: image.length, bytesTotal: image.length, phase: 'rebooting' })
        return
      }
      assertOk(s)
      if (s.state === DfuState.ManifestWaitReset) {
        onProgress?.({ bytesSent: image.length, bytesTotal: image.length, phase: 'rebooting' })
        return
      }
      if (Date.now() > deadline) throw new Error('the board never finished writing its flash')
      await sleep(Math.max(50, s.pollMs))
    }
  } finally {
    try {
      if (claimed !== null) await dev.releaseInterface(claimed)
      await dev.close()
    } catch {
      /* The board reboots itself at the end, so these throw routinely. */
    }
  }
}

/** Whether USB access is available at all on this machine. */
export async function dfuAvailable(): Promise<boolean> {
  try {
    await loadUsb()
    return true
  } catch {
    return false
  }
}
