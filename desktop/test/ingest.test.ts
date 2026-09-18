/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The fused card-to-library pass. Its output must be identical to the
 * two-pass parse-then-repair path — it is an optimisation, not a different
 * behaviour, and this suite is what keeps that true.
 */
import { mkdtemp, readFile, rm, stat } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterAll, beforeAll, describe, expect, it } from 'vitest'
import { REC_PREALLOC } from '../src/shared/avi/constants.js'
import { ingestClip } from '../src/shared/avi/ingest.js'
import { parseAvi } from '../src/shared/avi/parse.js'
import { repairTo } from '../src/shared/avi/repair.js'
import { buildAvi } from './fixtures/build.js'

let dir: string
const p = (n: string) => join(dir, n)

beforeAll(async () => {
  dir = await mkdtemp(join(tmpdir(), 'fpvault-ingest-'))
})
afterAll(async () => {
  await rm(dir, { recursive: true, force: true })
})

describe('ingest', () => {
  it('produces the same bytes as parse-then-repair', async () => {
    await buildAvi(p('src.avi'), { frames: 14, crashCut: true, staleBy: 5 })

    await repairTo(p('src.avi'), p('via-repair.avi'))
    await ingestClip(p('src.avi'), p('via-ingest.avi'))

    const a = await readFile(p('via-repair.avi'))
    const b = await readFile(p('via-ingest.avi'))
    expect(b.equals(a)).toBe(true)
  })

  it('reads only the real data, not the 200 MB preallocation', async () => {
    await buildAvi(p('big.avi'), { frames: 30, crashCut: true })
    const res = await ingestClip(p('big.avi'), p('big-out.avi'))

    expect(res.sourceBytes).toBe(REC_PREALLOC)
    /* One 4 MiB block is enough to reach the garbage and stop. */
    expect(res.bytesRead).toBeLessThan(8 * 1024 * 1024)
    expect(res.outputBytes).toBeLessThan(200_000)
    expect(res.health).toBe('crashCut')
  })

  it('leaves a cleanly closed clip intact', async () => {
    await buildAvi(p('clean.avi'), { frames: 10, finalize: true })
    const res = await ingestClip(p('clean.avi'), p('clean-out.avi'))

    expect(res.health).toBe('clean')
    expect(res.frames).toBe(10)
    expect(res.droppedTornFrame).toBe(false)

    const out = await parseAvi(p('clean-out.avi'))
    expect(out.health).toBe('clean')
    expect(out.realFrames).toBe(10)
    expect(out.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)
  })

  it('keeps dropout frames and drops a torn final frame', async () => {
    await buildAvi(p('mix.avi'), {
      frames: 12,
      dropAt: [4, 9],
      crashCut: true,
      tornLast: true
    })
    const res = await ingestClip(p('mix.avi'), p('mix-out.avi'))

    expect(res.droppedTornFrame).toBe(true)
    expect(res.frames).toBe(11)
    expect(res.drops).toBe(2)

    const out = await parseAvi(p('mix-out.avi'))
    expect(out.realFrames).toBe(11)
    expect(out.emptyFrames).toBe(2)
    expect(out.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)
  })

  it('reports a digest of what is actually on disk', async () => {
    await buildAvi(p('hash.avi'), { frames: 8, crashCut: true })
    const res = await ingestClip(p('hash.avi'), p('hash-out.avi'))

    const { createHash } = await import('node:crypto')
    const actual = createHash('sha256')
      .update(await readFile(p('hash-out.avi')))
      .digest('hex')
    expect(res.sha256).toBe(actual)
    expect((await stat(p('hash-out.avi'))).size).toBe(res.outputBytes)
  })

  it('survives a chunk that straddles the read block boundary', async () => {
    /* Frames are ~25 KB and blocks are 4 MiB, so a straddle happens roughly
     * every 160 frames in real footage. Build enough to cross one. */
    await buildAvi(p('straddle.avi'), { frames: 900, finalize: true })
    const res = await ingestClip(p('straddle.avi'), p('straddle-out.avi'))
    expect(res.frames).toBe(900)

    const out = await parseAvi(p('straddle-out.avi'))
    expect(out.realFrames).toBe(900)
    expect(out.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)
  })

  it('refuses a file that is not an AVI', async () => {
    const { writeFile } = await import('node:fs/promises')
    await writeFile(p('nope.bin'), Buffer.alloc(1024, 7))
    await expect(ingestClip(p('nope.bin'), p('nope-out.avi'))).rejects.toThrow(/not a RIFF/)
  })
})
