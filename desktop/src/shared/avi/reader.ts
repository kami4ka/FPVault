/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bounded random access over a file handle.
 *
 * checkavi.py reads the whole file into memory, which is fine for a bench
 * tool and not fine here: a crash-cut clip is 200 MB, an import queue holds
 * sixteen of them, and the bytes arrive over a ~7 MB/s link. The walk only
 * ever needs 8-byte chunk headers scattered through the file, so a small
 * page cache reads a few percent of it instead of all of it.
 *
 * Page size matters more than it looks. Chunks average ~25 KB, so a 64 KiB
 * page pulls two or three whole frames per header read and ends up touching
 * the entire file; 4 KiB pages touch roughly a sixth of it.
 */
import type { FileHandle } from 'node:fs/promises'

/* 512 bytes, not 4 KiB: the walk needs 8 bytes every ~21 KB (one frame
 * apart), so the page size is pure overhead per header. Dropping from 4 KiB
 * to one sector cut a real 200 MB clip's parse from 25 MB read to ~4 MB. */
export const PAGE_SIZE = 512
const MAX_PAGES = 256

export class Reader {
  private pages = new Map<number, Buffer>()
  /** Bytes actually pulled off disk — reported so the UI can show the saving. */
  bytesRead = 0

  constructor(
    private readonly fh: FileHandle,
    readonly size: number
  ) {}

  private async page(index: number): Promise<Buffer> {
    const hit = this.pages.get(index)
    if (hit) {
      // Refresh LRU position.
      this.pages.delete(index)
      this.pages.set(index, hit)
      return hit
    }
    const start = index * PAGE_SIZE
    const len = Math.min(PAGE_SIZE, this.size - start)
    const buf = Buffer.allocUnsafe(Math.max(0, len))
    if (len > 0) {
      const { bytesRead } = await this.fh.read(buf, 0, len, start)
      this.bytesRead += bytesRead
    }
    this.pages.set(index, buf)
    if (this.pages.size > MAX_PAGES) {
      const oldest = this.pages.keys().next().value
      if (oldest !== undefined) this.pages.delete(oldest)
    }
    return buf
  }

  /** Read `len` bytes at `off`. Returns fewer only at EOF. */
  async read(off: number, len: number): Promise<Buffer> {
    if (off < 0 || len <= 0 || off >= this.size) return Buffer.alloc(0)
    const clamped = Math.min(len, this.size - off)

    const firstPage = Math.floor(off / PAGE_SIZE)
    const lastPage = Math.floor((off + clamped - 1) / PAGE_SIZE)

    if (firstPage === lastPage) {
      const page = await this.page(firstPage)
      const start = off - firstPage * PAGE_SIZE
      return page.subarray(start, start + clamped)
    }

    /* Spanning read: for anything large, bypass the cache entirely rather
     * than evicting the whole working set for one payload. */
    if (clamped > PAGE_SIZE * 4) {
      const buf = Buffer.allocUnsafe(clamped)
      const { bytesRead } = await this.fh.read(buf, 0, clamped, off)
      this.bytesRead += bytesRead
      return buf.subarray(0, bytesRead)
    }

    const out = Buffer.allocUnsafe(clamped)
    let written = 0
    for (let p = firstPage; p <= lastPage; p++) {
      const page = await this.page(p)
      const pageStart = p * PAGE_SIZE
      const from = Math.max(0, off - pageStart)
      const to = Math.min(page.length, off + clamped - pageStart)
      if (to > from) {
        page.copy(out, written, from, to)
        written += to - from
      }
    }
    return out.subarray(0, written)
  }

  async u32(off: number): Promise<number> {
    const b = await this.read(off, 4)
    return b.length === 4 ? b.readUInt32LE(0) : 0
  }

  /**
   * A four-character code as latin1, i.e. one character per byte with no
   * loss. Comparing against '00dc' works, and the exact bytes survive for
   * the Python-style repr used in diagnostics.
   */
  async fourcc(off: number): Promise<string> {
    const b = await this.read(off, 4)
    return b.length < 4 ? '' : b.toString('latin1')
  }
}
