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

/** Mirrors main/jobs/queue.ts; duplicated here to keep this file import-free. */
export interface JobState {
  id: string
  kind: 'import' | 'repair' | 'join' | 'export' | 'firmware' | 'recover'
  label: string
  phase: 'queued' | 'running' | 'done' | 'failed' | 'cancelled'
  progress: number | null
  detail: string
  error?: string
}

export interface CardClipInfo {
  path: string
  name: string
  dcfIndex: number
  bytes: number
  looksCrashCut: boolean
}

export interface CardSessionInfo {
  dcfDir: number
  dirName: string
  clips: CardClipInfo[]
  bytes: number
}

export interface CardContentsInfo {
  volumePath: string
  sessions: CardSessionInfo[]
  totalClips: number
  totalBytes: number
  reclaimableBytes: number
}

export interface ExportFile {
  /** Path relative to the library root, which is all the renderer may name. */
  file: string
  name: string
  bytes: number
  createdMs: number
  /** 'avi' plays in the built-in player; 'mp4' opens in the system player. */
  kind: 'avi' | 'mp4' | 'other'
}

export interface LibraryView {
  root: string
  sessions: {
    id: string
    dcfDir: number
    label: string
    startUtc: string | null
    clips: LibraryClipView[]
    /** What join and export produced from this session. */
    exports: ExportFile[]
    durationSec: number
    bytes: number
  }[]
}

export interface LibraryClipView {
  id: string
  dcfIndex: number
  cardName: string
  file: string
  bytes: number
  sourceBytes: number
  health: ClipHealth
  frames: number
  drops: number
  droppedTornFrame: boolean
  durationSec: number
  startUtc: string | null
  gapUncertain: boolean
}

export interface ClipMedia {
  frames: number
  width: number
  height: number
  fps: number
  durationSec: number
}

export interface ReleaseInfo {
  tag: string
  name: string
  prerelease: boolean
  publishedAt: string
  notes: string
  hasFirmware: boolean
}

/* ---- preferences --------------------------------------------------------
 * Only values a person has a real opinion about. Everything else that looks
 * like a setting is either derived from the hardware or is a fact about the
 * format, and making those adjustable would only invite someone to break
 * their own footage.
 */

export type ExportQuality = 'high' | 'balanced' | 'small'

export interface Prefs {
  /** Library root. Null means the default under the user's videos folder. */
  libraryRoot: string | null
  /**
   * The source is interlaced analogue video off the TVD, so deinterlacing is
   * usually right — but it softens the picture, and anyone comparing output
   * will want it off.
   */
  deinterlace: boolean
  /** Maps to an x264 CRF and preset in main/settings.ts. */
  quality: ExportQuality
  /**
   * Seconds assumed between two clips that did not end in a segment
   * rollover. The firmware needs 5 s of lost signal to close a clip and 1 s
   * of stable signal to open the next, so six is a floor rather than a
   * measurement — someone who knows their own flight can say better.
   */
  gapSeconds: number
}

/** One bundled executable, as this build actually has it. */
export interface BundledToolInfo {
  id: 'ffmpeg' | 'sunxi-fel'
  /** Resolved path, or null when this build does not have it. */
  path: string | null
  version: string | null
  license: string
  homepage: string
  /** Where corresponding source can be obtained, for the GPL binaries. */
  sourceUrl: string
}

export interface LicenceInfo {
  id: string
  name: string
  version: string | null
  license: string
  homepage: string
  sourceUrl: string | null
}

export interface Api {
  device: {
    get(): Promise<DeviceState>
    onChange(fn: (s: DeviceState) => void): () => void
    rescan(): Promise<DeviceState>
  }
  card: {
    scan(volumePath: string): Promise<CardContentsInfo>
  }
  library: {
    get(): Promise<LibraryView>
    onChange(fn: (l: LibraryView) => void): () => void
    setSessionStart(sessionId: string, startUtc: string | null): Promise<LibraryView>
    chooseRoot(): Promise<LibraryView>
    reveal(file: string): Promise<void>
    /** Hand a file to whatever the system uses for it. */
    open(file: string): Promise<string>
  }
  clip: {
    /**
     * Seek table and geometry. Takes a clip id, or a library-relative path
     * so joined exports can be played with the same viewer.
     */
    media(clipOrPath: string): Promise<ClipMedia | null>
    /** One frame as JPEG bytes. Empty means a dropout: hold the last image. */
    frame(clipOrPath: string, index: number): Promise<Uint8Array>
  }
  jobs: {
    list(): Promise<JobState[]>
    onChange(fn: (j: JobState) => void): () => void
    importSessions(volumePath: string, dcfDirs: number[]): Promise<string[]>
    joinSession(sessionId: string): Promise<string>
    exportSession(sessionId: string): Promise<string>
    cancel(id: string): Promise<void>
  }
  firmware: {
    releases(force?: boolean): Promise<ReleaseInfo[]>
    /** False when USB access is unavailable: offer FEL instead. */
    canFlash(): Promise<boolean>
    flash(tag: string): Promise<string>
    /** FEL recovery: writes flash through the boot ROM. */
    canRecover(): Promise<boolean>
    recover(tag: string, withUboot: boolean): Promise<string>
  }
  settings: {
    get(): Promise<Prefs>
    /** Everything but the library root, which moves via library.chooseRoot. */
    set(patch: Partial<Omit<Prefs, 'libraryRoot'>>): Promise<Prefs>
  }
  app: {
    versions(): Promise<{ app: string; electron: string; node: string; chrome: string }>
    /** False when ffmpeg was not bundled: the MP4 export is then unavailable. */
    canExport(): Promise<boolean>
    /** What was bundled, so a missing button can be explained. */
    tools(): Promise<BundledToolInfo[]>
    licences(): Promise<LicenceInfo[]>
    /** Open an https page in the system browser. */
    openUrl(url: string): Promise<void>
  }
}
