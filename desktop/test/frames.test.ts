/* SPDX-License-Identifier: GPL-3.0-or-later */
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterAll, beforeAll, describe, expect, it } from 'vitest'
import { frameTable, posterIndex, readFrame } from '../src/shared/avi/frames.js'
import { buildAvi, TINY_JPEG } from './fixtures/build.js'

let dir: string
const p = (n: string) => join(dir, n)

beforeAll(async () => {
  dir = await mkdtemp(join(tmpdir(), 'fpvault-frames-'))
})
afterAll(async () => {
  await rm(dir, { recursive: true, force: true })
})

describe('frame access', () => {
  it('builds a seek table from idx1 without walking', async () => {
    await buildAvi(p('clean.avi'), { frames: 12, finalize: true })
    const t = await frameTable(p('clean.avi'))
    expect(t?.frames).toHaveLength(12)
    expect(t?.width).toBe(720)
    expect(t?.rate).toBe(30000)
  })

  it('falls back to a walk when there is no index', async () => {
    await buildAvi(p('crash.avi'), { frames: 9, crashCut: true })
    const t = await frameTable(p('crash.avi'))
    expect(t?.frames).toHaveLength(9)
  })

  it('returns a complete standalone JPEG per frame', async () => {
    await buildAvi(p('c.avi'), { frames: 6, finalize: true })
    const t = await frameTable(p('c.avi'))
    const jpeg = await readFrame(p('c.avi'), t!, 3)

    /* This is the whole reason the player needs no transcoding. */
    expect(jpeg.subarray(0, 3)).toEqual(Buffer.from([0xff, 0xd8, 0xff]))
    expect(jpeg.subarray(-2)).toEqual(Buffer.from([0xff, 0xd9]))
    expect(jpeg.equals(TINY_JPEG)).toBe(true)
  })

  it('returns nothing for a dropout frame', async () => {
    await buildAvi(p('d.avi'), { frames: 8, dropAt: [2], finalize: true })
    const t = await frameTable(p('d.avi'))
    expect((await readFrame(p('d.avi'), t!, 2)).length).toBe(0)
    expect((await readFrame(p('d.avi'), t!, 3)).length).toBeGreaterThan(0)
  })

  it('picks a poster frame past the first moments', async () => {
    expect(posterIndex(9000)).toBe(45)
    expect(posterIndex(30)).toBe(10)
  })
})
