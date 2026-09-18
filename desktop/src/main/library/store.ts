/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The library index.
 *
 * Plain JSON, not SQLite. A heavy user has a few thousand clips, not
 * millions, the only query is "group by session, order by DCF index", and
 * better-sqlite3 is a native addon — the exact cost the device layer was
 * built to avoid. Writes go through a temp file and a rename so a crash
 * never leaves a half-written index.
 *
 * Each session also carries a copy of its own entries on disk, so the whole
 * library can be rebuilt by scanning directories if the index is ever lost.
 * That mirrors the firmware's own stance: the files are the truth, and
 * dcf_boot_scan rebuilds its state by listing them.
 */
import { mkdir, readFile, rename, writeFile } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import type { ClipHealth } from '@shared/types'

export const LIBRARY_VERSION = 1

export interface LibraryClip {
  id: string
  sessionId: string
  /** DCF numbers: directory 100..999, index 1..9999, both from the card. */
  dcfDir: number
  dcfIndex: number
  /** Original name on the card, so a re-import is recognised as the same clip. */
  cardName: string
  /** Path inside the library root. */
  file: string
  sourceBytes: number
  bytes: number
  sha256: string
  health: ClipHealth
  frames: number
  drops: number
  droppedTornFrame: boolean
  width: number
  height: number
  rate: number
  scale: number
  durationSec: number
  /** Real wall-clock start, once known. Null until a session start is set. */
  startUtc: string | null
  startSource: 'user' | 'inferred' | 'unknown'
  importedAt: string
}

export interface LibrarySession {
  id: string
  /** The NNN of the card's NNNFCDVR directory. */
  dcfDir: number
  /** User-supplied real start time for the session, if given. */
  startUtc: string | null
  clipIds: string[]
}

export interface Library {
  version: number
  root: string
  sessions: Record<string, LibrarySession>
  clips: Record<string, LibraryClip>
}

function empty(root: string): Library {
  return { version: LIBRARY_VERSION, root, sessions: {}, clips: {} }
}

async function writeAtomic(path: string, data: string): Promise<void> {
  await mkdir(dirname(path), { recursive: true })
  const tmp = `${path}.tmp`
  await writeFile(tmp, data, 'utf8')
  await rename(tmp, path)
}

export class Store {
  private data: Library
  private readonly indexPath: string
  private queue: Promise<void> = Promise.resolve()

  private constructor(root: string, data: Library) {
    this.data = data
    this.indexPath = join(root, 'library.json')
  }

  static async open(root: string): Promise<Store> {
    const indexPath = join(root, 'library.json')
    let data: Library
    try {
      const parsed = JSON.parse(await readFile(indexPath, 'utf8')) as Library
      /* Accept only what we wrote; a future version reopens as empty rather
       * than silently dropping fields it does not understand. */
      data = parsed.version === LIBRARY_VERSION ? { ...parsed, root } : empty(root)
    } catch {
      data = empty(root)
    }
    await mkdir(root, { recursive: true })
    return new Store(root, data)
  }

  get root(): string {
    return this.data.root
  }

  snapshot(): Library {
    return structuredClone(this.data)
  }

  /** Is this card clip already in the library, unchanged? */
  has(dcfDir: number, dcfIndex: number, sourceBytes: number): boolean {
    return Object.values(this.data.clips).some(
      (c) => c.dcfDir === dcfDir && c.dcfIndex === dcfIndex && c.sourceBytes === sourceBytes
    )
  }

  clipsOf(sessionId: string): LibraryClip[] {
    const s = this.data.sessions[sessionId]
    if (!s) return []
    return s.clipIds
      .map((id) => this.data.clips[id])
      .filter((c): c is LibraryClip => c !== undefined)
      .sort((a, b) => a.dcfIndex - b.dcfIndex)
  }

  sessions(): LibrarySession[] {
    return Object.values(this.data.sessions).sort((a, b) => a.dcfDir - b.dcfDir)
  }

  async addClip(clip: LibraryClip): Promise<void> {
    this.data.clips[clip.id] = clip
    const session = (this.data.sessions[clip.sessionId] ??= {
      id: clip.sessionId,
      dcfDir: clip.dcfDir,
      startUtc: null,
      clipIds: []
    })
    if (!session.clipIds.includes(clip.id)) session.clipIds.push(clip.id)
    session.clipIds.sort(
      (a, b) => (this.data.clips[a]?.dcfIndex ?? 0) - (this.data.clips[b]?.dcfIndex ?? 0)
    )
    await this.flush()
  }

  async setSessionStart(sessionId: string, startUtc: string | null): Promise<void> {
    const s = this.data.sessions[sessionId]
    if (!s) return
    s.startUtc = startUtc
    await this.flush()
  }

  async updateClip(id: string, patch: Partial<LibraryClip>): Promise<void> {
    const c = this.data.clips[id]
    if (!c) return
    Object.assign(c, patch)
    await this.flush()
  }

  /** Serialise writes so two concurrent jobs cannot interleave a rename. */
  private flush(): Promise<void> {
    this.queue = this.queue.then(async () => {
      await writeAtomic(this.indexPath, JSON.stringify(this.data, null, 1))
    })
    return this.queue
  }
}
