/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The AVI walker: a port of tools/checkavi.py.
 *
 * Diagnostic wording is deliberately byte-identical to the Python so the two
 * stay comparable — a user can paste this app's report next to the bench
 * tool's and diff them, and the test suite asserts they agree. Every check
 * here has a line reference to the Python it mirrors.
 *
 * The walk trusts the bytes, not the header: it keeps going past whatever
 * movi size the header claims, because a power cut leaves that number up to
 * 30 frames stale (src/recorder.c refreshes it every 30 frames).
 */
import { open } from 'node:fs/promises'
import type { ClipHealth, ClipInfo, Diagnostic, FrameIndex } from '../types.js'
import {
  AVIF_HASINDEX,
  AVI_HDR_SIZE,
  FOURCC_00DC,
  MOVI_FOLLOWERS,
  OFF_FLAGS,
  REC_PREALLOC
} from './constants.js'
import { Reader } from './reader.js'

export interface StreamInfo {
  type: string
  handler: string
  scale: number
  rate: number
  length: number
  width: number
  height: number
  compression: string
}

export interface ParseResult extends ClipInfo {
  streams: StreamInfo[]
  /** Absolute offset of the 'movi' fourcc; idx1 offsets are relative to it. */
  moviFccOff: number
  /** Per-frame seek table: the player and the repair writer both use it. */
  frames: FrameIndex[]
  /** Raw chunk positions, including the 8-byte header. */
  chunks: { off: number; size: number }[]
  idx1: { off: number; size: number } | null
  truncated: boolean
  /** First byte past the real data — ingest copies [0, walkEnd) and no more. */
  walkEnd: number
  bytesRead: number
}

class Collector {
  readonly diagnostics: Diagnostic[] = []
  errors = 0
  warnings = 0

  err(message: string) {
    this.diagnostics.push({ severity: 'error', message })
    this.errors++
  }
  warn(message: string) {
    this.diagnostics.push({ severity: 'warning', message })
    this.warnings++
  }
}

const hex = (n: number) => `0x${n.toString(16)}`

/** checkavi.py:57 — parse avih and every strl in the hdrl LIST. */
async function parseHdrl(r: Reader, body: number, end: number) {
  let avih: { usPerFrame: number; flags: number; totalFrames: number; streams: number } | null =
    null
  const streams: StreamInfo[] = []

  let off = body + 4 /* skip the 'hdrl' fourcc */
  while (off + 8 <= end) {
    const id = await r.fourcc(off)
    const size = await r.u32(off + 4)
    const cbody = off + 8

    if (id === 'avih' && size >= 56) {
      avih = {
        usPerFrame: await r.u32(cbody),
        flags: await r.u32(cbody + 12),
        totalFrames: await r.u32(cbody + 16),
        streams: await r.u32(cbody + 24)
      }
    } else if (id === 'LIST' && (await r.fourcc(cbody)) === 'strl') {
      const s: StreamInfo = {
        type: '?',
        handler: '?',
        scale: 0,
        rate: 0,
        length: 0,
        width: 0,
        height: 0,
        compression: '?'
      }
      let soff = cbody + 4
      const send = cbody + size
      while (soff + 8 <= send) {
        const sid = await r.fourcc(soff)
        const ssz = await r.u32(soff + 4)
        const sbody = soff + 8
        if (sid === 'strh' && ssz >= 56) {
          s.type = await r.fourcc(sbody)
          s.handler = await r.fourcc(sbody + 4)
          s.scale = await r.u32(sbody + 20)
          s.rate = await r.u32(sbody + 24)
          s.length = await r.u32(sbody + 32)
        } else if (sid === 'strf' && ssz >= 40 && s.type === 'vids') {
          s.width = await r.u32(sbody + 4)
          s.height = await r.u32(sbody + 8)
          s.compression = await r.fourcc(sbody + 16)
        }
        soff = sbody + ssz + (ssz & 1)
      }
      streams.push(s)
    }
    off = cbody + size + (size & 1)
  }
  return { avih, streams }
}

/** checkavi.py:110 — walk '00dc' chunks from just after the 'movi' fourcc. */
async function walkMovi(
  r: Reader,
  moviFccOff: number,
  claimedEnd: number | null,
  c: Collector
): Promise<{ chunks: { off: number; size: number }[]; truncated: boolean; walkEnd: number }> {
  const chunks: { off: number; size: number }[] = []
  let truncated = false
  let off = moviFccOff + 4
  const flen = r.size

  for (;;) {
    if (off + 8 > flen) {
      if (off !== flen) {
        truncated = true
        c.warn(`file ends inside a chunk header at ${hex(off)}`)
      }
      break
    }
    const cid = await r.fourcc(off)
    if (cid !== FOURCC_00DC) {
      if (!MOVI_FOLLOWERS.has(cid)) {
        /* This is the condition that fires on a crash-cut clip: the bytes
         * after the last frame are whatever the cluster held before, because
         * f_expand does not zero the preallocation. */
        truncated = true
        c.warn(`movi walk stopped at unknown fourcc ${pyRepr(cid)} (${hex(off)})`)
      }
      break
    }
    if (off % 4) c.err(`chunk ${chunks.length} at ${hex(off)} is not DWORD-aligned`)

    const size = await r.u32(off + 4)
    const pad = (4 - (size & 3)) & 3

    if (off + 8 + size > flen) {
      truncated = true
      c.warn(
        `chunk ${chunks.length} at ${hex(off)} cut short by EOF ` +
          `(payload ${size}, have ${flen - off - 8})`
      )
      break
    }
    if (off + 8 + size + pad > flen) {
      truncated = true
      c.warn(`chunk ${chunks.length} at ${hex(off)} missing pad bytes at EOF`)
      chunks.push({ off, size }) /* payload complete: keep the frame */
      off = flen
      break
    }
    if (pad) {
      const padBytes = await r.read(off + 8 + size, pad)
      if (padBytes.some((b) => b !== 0))
        c.warn(`chunk ${chunks.length} at ${hex(off)} has non-zero pad bytes`)
    }
    chunks.push({ off, size })
    off += 8 + size + pad
  }

  if (!truncated && claimedEnd !== null && off !== claimedEnd) {
    if (off > claimedEnd)
      c.warn("movi holds data past the header's movi size (stale header, power cut after last refresh?)")
    else c.warn(`movi ends ${hex(claimedEnd - off)} before the header's movi size`)
  }
  return { chunks, truncated, walkEnd: off }
}

/**
 * Python's repr() of a bytes literal, so the warning text is identical to
 * checkavi.py's. Printable ASCII stays literal; everything else becomes the
 * escape Python would print.
 */
function pyRepr(s: string): string {
  let out = ''
  for (const ch of s) {
    const b = ch.charCodeAt(0)
    if (ch === '\\') out += '\\\\'
    else if (ch === "'") out += "\\'"
    else if (b === 9) out += '\\t'
    else if (b === 10) out += '\\n'
    else if (b === 13) out += '\\r'
    else if (b >= 0x20 && b < 0x7f) out += ch
    else out += '\\x' + b.toString(16).padStart(2, '0')
  }
  return `b'${out}'`
}

/** checkavi.py:159 — validate idx1 against what the walk actually found. */
async function checkIdx1(
  r: Reader,
  idxOff: number,
  idxSize: number,
  chunks: { off: number; size: number }[],
  moviFccOff: number,
  c: Collector
) {
  const flen = r.size
  let n = Math.floor(idxSize / 16)
  if (idxOff + 8 + idxSize > flen) {
    const have = Math.max(0, Math.floor((flen - idxOff - 8) / 16))
    c.warn(`idx1 truncated: ${have} of ${n} entries present`)
    n = have
  }
  if (n !== chunks.length) {
    const msg = `idx1 has ${n} entries, movi has ${chunks.length} chunks`
    /* A short index (the firmware's staging RAM ran out) is still usable;
     * a long one is not. */
    if (n < chunks.length) c.warn(msg)
    else c.err(msg)
  }

  let convention: 'movi-relative' | 'absolute' | null = null
  const first = chunks[0]
  if (n && first) {
    const e0 = await r.u32(idxOff + 8 + 8)
    if (e0 === first.off - moviFccOff) convention = 'movi-relative'
    else if (e0 === first.off) {
      convention = 'absolute'
      c.warn("idx1 uses absolute offsets (not our writer's convention)")
    } else {
      c.err(`idx1 entry 0 offset ${e0} matches no known convention`)
      return
    }
  }

  const base = convention === 'absolute' ? 0 : moviFccOff
  for (let i = 0; i < Math.min(n, chunks.length); i++) {
    const e = idxOff + 8 + 16 * i
    const cid = await r.fourcc(e)
    const eoff = await r.u32(e + 8)
    const esize = await r.u32(e + 12)
    const chunk = chunks[i]
    if (!chunk) break
    if (cid !== FOURCC_00DC) c.err(`idx1 entry ${i}: fourcc ${pyRepr(cid)}`)
    if (eoff + base !== chunk.off)
      c.err(`idx1 entry ${i}: offset ${eoff}, chunk really at ${chunk.off - base}`)
    if (esize !== chunk.size) c.err(`idx1 entry ${i}: size ${esize}, chunk stores ${chunk.size}`)
  }
}

/**
 * A finalized clip is always f_truncate'd to its real size, and a full
 * 9000-frame segment is ~229 MB — larger than the 200 MB preallocation. So a
 * file that is exactly REC_PREALLOC bytes can only be one that was never
 * truncated, which means the power went away mid-clip.
 */
function classify(
  fileLen: number,
  hasIdx1: boolean,
  flags: number,
  truncated: boolean,
  errors: number
): ClipHealth {
  if (errors > 0) return 'damaged'
  if (fileLen === REC_PREALLOC && !hasIdx1 && truncated) return 'crashCut'
  if (!hasIdx1 || truncated) return 'crashCut'
  return 'clean'
}

export async function parseAvi(path: string): Promise<ParseResult> {
  const fh = await open(path, 'r')
  try {
    const { size } = await fh.stat()
    const r = new Reader(fh, size)
    const c = new Collector()

    if (size < 12 || (await r.fourcc(0)) !== 'RIFF' || (await r.fourcc(8)) !== 'AVI ') {
      c.err('not a RIFF/AVI file (header truncated or foreign)')
      return emptyResult(path, size, c, r)
    }

    const riffsz = await r.u32(4)
    if (riffsz + 8 > size) c.warn(`RIFF size ${hex(riffsz)} overruns the file (truncated)`)
    else if (riffsz + 8 < size)
      c.warn(`${size - riffsz - 8} bytes past the RIFF end (stale header after power cut?)`)

    /* Top-level walk for hdrl, movi and idx1 (checkavi.py:240). Doing the
     * real walk rather than trusting the fixed 224-byte layout means foreign
     * files and any future header change still parse. */
    let avih: Awaited<ReturnType<typeof parseHdrl>>['avih'] = null
    let streams: StreamInfo[] = []
    let moviFccOff: number | null = null
    let claimedMoviEnd: number | null = null
    let idx: { off: number; size: number } | null = null

    let off = 12
    while (off + 8 <= size) {
      const id = await r.fourcc(off)
      const csz = await r.u32(off + 4)
      const body = off + 8
      if (id === 'LIST') {
        const kind = await r.fourcc(body)
        if (kind === 'hdrl') {
          const parsed = await parseHdrl(r, body, Math.min(body + csz, size))
          avih = parsed.avih
          streams = parsed.streams
        } else if (kind === 'movi') {
          moviFccOff = body
          claimedMoviEnd = body + csz
        }
      } else if (id === 'idx1') {
        idx = { off, size: csz }
      }
      off = body + csz + (csz & 1)
    }

    if (!avih) {
      c.err('no avih header found')
      return emptyResult(path, size, c, r)
    }
    if (moviFccOff === null) {
      c.err('no movi LIST found')
      return emptyResult(path, size, c, r)
    }

    if (streams.length !== avih.streams)
      c.err(`avih says ${avih.streams} streams, hdrl defines ${streams.length}`)

    const claimed = claimedMoviEnd !== null && claimedMoviEnd <= size ? claimedMoviEnd : null
    const { chunks, truncated, walkEnd } = await walkMovi(r, moviFccOff, claimed, c)

    const nframes = chunks.length
    const drops = chunks.filter((k) => k.size === 0).length

    if (truncated) c.warn(`truncated after ${nframes} frames`)
    if (avih.totalFrames !== nframes) {
      const stale = avih.totalFrames < nframes
      const msg =
        `header counts ${avih.totalFrames} frames, movi walk found ${nframes}` +
        (stale ? ' (stale header, expected after a power cut)' : '')
      if (stale) c.warn(msg)
      else c.err(msg)
    }

    if (idx) await checkIdx1(r, idx.off, idx.size, chunks, moviFccOff, c)
    else if (avih.flags & AVIF_HASINDEX)
      c.warn('AVIF_HASINDEX set but no idx1 (power cut before finalize?)')

    const vid = streams.find((s) => s.type === 'vids')
    const rate = vid?.rate ?? 0
    const scale = vid?.scale ?? 0
    const fps = scale ? rate / scale : avih.usPerFrame ? 1e6 / avih.usPerFrame : 0

    return {
      path,
      sizeBytes: size,
      health: classify(size, idx !== null, avih.flags, truncated, c.errors),
      width: vid?.width ?? 0,
      height: vid?.height ?? 0,
      rate,
      scale,
      headerFrames: avih.totalFrames,
      realFrames: nframes,
      emptyFrames: drops,
      trueEnd: walkEnd,
      hasIndex: idx !== null,
      flags: avih.flags,
      durationSec: fps ? nframes / fps : 0,
      diagnostics: c.diagnostics,
      streams,
      moviFccOff,
      frames: chunks.map((k) => ({ offset: k.off + 8, size: k.size })),
      chunks,
      idx1: idx,
      truncated,
      walkEnd,
      bytesRead: r.bytesRead
    }
  } finally {
    await fh.close()
  }
}

function emptyResult(path: string, size: number, c: Collector, r: Reader): ParseResult {
  return {
    path,
    sizeBytes: size,
    health: 'damaged',
    width: 0,
    height: 0,
    rate: 0,
    scale: 0,
    headerFrames: 0,
    realFrames: 0,
    emptyFrames: 0,
    trueEnd: 0,
    hasIndex: false,
    flags: 0,
    durationSec: 0,
    diagnostics: c.diagnostics,
    streams: [],
    moviFccOff: AVI_HDR_SIZE - 4,
    frames: [],
    chunks: [],
    idx1: null,
    truncated: true,
    walkEnd: 0,
    bytesRead: r.bytesRead
  }
}

export { OFF_FLAGS }
