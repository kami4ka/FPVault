/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Types shared across main, preload and renderer. Keep this file free of
 * imports so it can be pulled into any of the three processes.
 */

/* ---- device identity ----------------------------------------------------
 * From src/usbmsc.c: the board enumerates as one composite device with a
 * mass-storage interface and, since v0.9.2, a DFU interface. A board in FEL
 * recovery enumerates as the Allwinner boot ROM instead, under its own IDs.
 */
export const FPVAULT_VID = 0x34b7
export const FPVAULT_PID = 0xf1c2
export const FEL_VID = 0x1f3a
export const FEL_PID = 0xefe8

/** DFU interface descriptor triple (bInterfaceClass/SubClass/Protocol). */
export const DFU_CLASS = 0xfe
export const DFU_SUBCLASS = 0x01
export const DFU_PROTOCOL_MODE = 0x02

/** The board's own limits, from src/spinor.h and src/recorder.c. */
export const FW_SLOT_SIZE = 0x40000 /* 256 KB NOR slot U-Boot reads */
export const CLIP_PREALLOC = 209_715_200 /* f_expand size; a crash-cut clip is exactly this */

export type DeviceState =
  /** Nothing plugged in that we recognise. */
  | { kind: 'absent' }
  /**
   * Board present and exporting the card, with a DFU interface so it can be
   * updated in place. `volume` is null until the OS finishes mounting.
   */
  | { kind: 'reader'; firmware: FirmwareId; volume: CardVolume | null }
  /**
   * Board present but running firmware with no DFU interface (v0.9.1 or
   * earlier). Updating it needs one trip through FEL.
   */
  | { kind: 'legacy'; firmware: FirmwareId; volume: CardVolume | null }
  /** Board sitting in the boot ROM's FEL recovery mode. */
  | { kind: 'fel' }
  /** No board, but a FPVault card is in a native reader. */
  | { kind: 'cardOnly'; volume: CardVolume }

export type DeviceKind = DeviceState['kind']

/**
 * What we can learn about the installed firmware over USB alone. There is no
 * serial console, so today this is thin: `bcdDevice` is hardcoded 0x0100 in
 * src/usbmsc.c and carries no version. `version` stays null until firmware
 * starts encoding it there; `dfuCapable` is the only real signal we have.
 */
export interface FirmwareId {
  bcdDevice: number
  serial: string | null
  /** Parsed from bcdDevice once firmware encodes it, e.g. 0x0092 -> "0.9.2". */
  version: string | null
}

export interface CardVolume {
  /** Mount point: "/Volumes/F1CSD", "E:\\", "/media/user/F1CSD". */
  path: string
  label: string | null
  totalBytes: number | null
  freeBytes: number | null
  /** Session directories found under /DCIM, e.g. ["129FCDVR"]. */
  sessions: string[]
}

/* ---- clip model --------------------------------------------------------- */

export type ClipHealth =
  /** idx1 present and header counts agree: closed cleanly by the firmware. */
  | 'clean'
  /** Exactly CLIP_PREALLOC bytes, no idx1, walk stops early: power cut. */
  | 'crashCut'
  /** Structurally wrong in a way a power cut does not explain. */
  | 'damaged'

export interface ClipInfo {
  path: string
  sizeBytes: number
  health: ClipHealth
  width: number
  height: number
  /** AVI timebase: fps = rate / scale. NTSC 30000/1001, PAL 25/1. */
  rate: number
  scale: number
  /** Frames the header claims; stale by up to 30 on a crash-cut clip. */
  headerFrames: number
  /** Frames actually present, found by walking the movi chunks. */
  realFrames: number
  /** Zero-length '00dc' chunks the firmware wrote for dropped frames. */
  emptyFrames: number
  /** First byte past the real data. Ingest copies [0, trueEnd) and no more. */
  trueEnd: number
  hasIndex: boolean
  flags: number
  durationSec: number
  diagnostics: Diagnostic[]
}

export interface Diagnostic {
  severity: 'error' | 'warning'
  /** Wording mirrors tools/checkavi.py so the two stay comparable. */
  message: string
}

/** One '00dc' chunk: the seek table the player and the repair writer use. */
export interface FrameIndex {
  /** Absolute file offset of the JPEG payload, not of the chunk header. */
  offset: number
  size: number
}

/* ---- IPC ---------------------------------------------------------------- */

export interface Api {
  device: {
    get(): Promise<DeviceState>
    onChange(fn: (s: DeviceState) => void): () => void
    rescan(): Promise<DeviceState>
  }
  app: {
    versions(): Promise<{ app: string; electron: string; node: string; chrome: string }>
  }
}
