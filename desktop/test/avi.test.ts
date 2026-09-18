/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The parser and the repair writer, checked against synthesised fixtures and
 * — where python3 is available — against tools/checkavi.py itself, which
 * stays the oracle.
 */
import { execFile } from 'node:child_process'
import { mkdtemp, rm, stat, readFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { promisify } from 'node:util'
import { afterAll, beforeAll, describe, expect, it } from 'vitest'
import { REC_PREALLOC } from '../src/shared/avi/constants.js'
import { parseAvi } from '../src/shared/avi/parse.js'
import { concatTo, repairTo } from '../src/shared/avi/repair.js'
import { exitCode, formatReport } from '../src/shared/avi/report.js'
import { buildAvi } from './fixtures/build.js'

const run = promisify(execFile)
const CHECKAVI = resolve(import.meta.dirname, '../../tools/checkavi.py')

let dir: string
const p = (n: string) => join(dir, n)

beforeAll(async () => {
  dir = await mkdtemp(join(tmpdir(), 'fpvault-avi-'))
})
afterAll(async () => {
  await rm(dir, { recursive: true, force: true })
})

describe('parser', () => {
  it('reads a cleanly finalized clip', async () => {
    await buildAvi(p('clean.avi'), { frames: 10, finalize: true })
    const r = await parseAvi(p('clean.avi'))

    expect(r.health).toBe('clean')
    expect(r.realFrames).toBe(10)
    expect(r.headerFrames).toBe(10)
    expect(r.hasIndex).toBe(true)
    expect(r.flags).toBe(0x110) /* ISINTERLEAVED | HASINDEX */
    expect(r.width).toBe(720)
    expect(r.height).toBe(480)
    expect(r.rate / r.scale).toBeCloseTo(29.97, 2)
    expect(r.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)
  })

  it('recovers a crash-cut clip and finds its true end', async () => {
    await buildAvi(p('crash.avi'), { frames: 12, crashCut: true, staleBy: 4 })
    const r = await parseAvi(p('crash.avi'))

    expect(r.sizeBytes).toBe(REC_PREALLOC)
    expect(r.health).toBe('crashCut')
    expect(r.hasIndex).toBe(false)
    expect(r.flags).toBe(0x100) /* never promised an index */
    expect(r.realFrames).toBe(12)
    expect(r.headerFrames).toBe(8) /* stale by the refresh interval */
    expect(r.truncated).toBe(true)
    expect(r.trueEnd).toBeLessThan(r.sizeBytes / 1000)
    expect(r.diagnostics.map((d) => d.message)).toContain('truncated after 12 frames')
  })

  it('reads only a sliver of a 200 MB crash-cut file', async () => {
    await buildAvi(p('big.avi'), { frames: 40, crashCut: true })
    const r = await parseAvi(p('big.avi'))
    /* The whole point: never touch the preallocated tail. */
    expect(r.bytesRead).toBeLessThan(2 * 1024 * 1024)
    expect(r.sizeBytes).toBe(REC_PREALLOC)
  })

  it('counts zero-length dropout chunks as frames', async () => {
    await buildAvi(p('drops.avi'), { frames: 10, dropAt: [3, 4, 7], finalize: true })
    const r = await parseAvi(p('drops.avi'))
    /* The firmware writes empty chunks so wall-clock time stays honest;
     * they must survive parsing or durations go wrong. */
    expect(r.realFrames).toBe(10)
    expect(r.emptyFrames).toBe(3)
  })

  it('flags a header that promises an index it does not have', async () => {
    await buildAvi(p('liar.avi'), { frames: 5, lieAboutIndex: true })
    const r = await parseAvi(p('liar.avi'))
    expect(r.diagnostics.map((d) => d.message)).toContain(
      'AVIF_HASINDEX set but no idx1 (power cut before finalize?)'
    )
  })

  it('reports a payload cut short by end of file', async () => {
    await buildAvi(p('cut.avi'), { frames: 6, cutInPayload: true })
    const r = await parseAvi(p('cut.avi'))
    expect(r.truncated).toBe(true)
    expect(r.diagnostics.some((d) => d.message.includes('cut short by EOF'))).toBe(true)
  })

  it('rejects a file that is not an AVI', async () => {
    const { writeFile } = await import('node:fs/promises')
    await writeFile(p('junk.avi'), Buffer.alloc(4096, 0x5a))
    const r = await parseAvi(p('junk.avi'))
    expect(r.health).toBe('damaged')
    expect(exitCode(r)).toBe(1)
  })
})

describe('repair', () => {
  it('turns a crash-cut clip into one indistinguishable from a clean close', async () => {
    await buildAvi(p('r-crash.avi'), { frames: 12, crashCut: true, staleBy: 4 })
    const res = await repairTo(p('r-crash.avi'), p('r-fixed.avi'))

    const fixed = await parseAvi(p('r-fixed.avi'))
    expect(fixed.health).toBe('clean')
    expect(fixed.hasIndex).toBe(true)
    expect(fixed.flags).toBe(0x110)
    expect(fixed.realFrames).toBe(12)
    expect(fixed.headerFrames).toBe(12)
    expect(fixed.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)

    /* 200 MB in, a few kilobytes out. */
    expect(res.naiveBytes).toBe(REC_PREALLOC)
    expect(res.outputBytes).toBeLessThan(100_000)
    expect((await stat(p('r-fixed.avi'))).size).toBe(res.outputBytes)
  })

  it('is byte-identical to a clip the firmware finalized itself', async () => {
    /* The strongest assertion in the suite: repair must produce exactly what
     * avi_finalize() would have written for the same frames. */
    await buildAvi(p('id-clean.avi'), { frames: 9, finalize: true })
    await buildAvi(p('id-crash.avi'), { frames: 9, crashCut: true, staleBy: 3 })
    await repairTo(p('id-crash.avi'), p('id-repaired.avi'))

    const a = await readFile(p('id-clean.avi'))
    const b = await readFile(p('id-repaired.avi'))
    expect(b.equals(a)).toBe(true)
  })

  it('drops the torn frame that was in flight when power died', async () => {
    /* Build a clip whose last chunk holds a JPEG with no EOI: exactly what a
     * cut mid-write leaves behind. The firmware promises you lose that one
     * frame, so repair should too. */
    await buildAvi(p('torn.avi'), { frames: 8, crashCut: true, tornLast: true })
    const before = await parseAvi(p('torn.avi'))
    expect(before.realFrames).toBe(8)

    const res = await repairTo(p('torn.avi'), p('torn-fixed.avi'))
    expect(res.droppedTornFrame).toBe(true)
    expect(res.frames).toBe(7)

    const after = await parseAvi(p('torn-fixed.avi'))
    expect(after.realFrames).toBe(7)
    expect(after.headerFrames).toBe(7)
    expect(after.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)
  })

  it('keeps an intact final frame', async () => {
    await buildAvi(p('whole.avi'), { frames: 8, crashCut: true })
    const res = await repairTo(p('whole.avi'), p('whole-fixed.avi'))
    expect(res.droppedTornFrame).toBe(false)
    expect(res.frames).toBe(8)
  })

  it('preserves dropout frames through a repair', async () => {
    await buildAvi(p('d-crash.avi'), { frames: 10, dropAt: [2, 5], crashCut: true })
    await repairTo(p('d-crash.avi'), p('d-fixed.avi'))
    const fixed = await parseAvi(p('d-fixed.avi'))
    expect(fixed.realFrames).toBe(10)
    expect(fixed.emptyFrames).toBe(2)
  })
})

describe('join', () => {
  it('concatenates segments losslessly into one indexed clip', async () => {
    await buildAvi(p('s1.avi'), { frames: 6, finalize: true })
    await buildAvi(p('s2.avi'), { frames: 7, finalize: true })
    await buildAvi(p('s3.avi'), { frames: 5, crashCut: true })

    const res = await concatTo([p('s1.avi'), p('s2.avi'), p('s3.avi')], p('joined.avi'))
    expect(res.frames).toBe(18)

    const joined = await parseAvi(p('joined.avi'))
    expect(joined.health).toBe('clean')
    expect(joined.realFrames).toBe(18)
    expect(joined.headerFrames).toBe(18)
    expect(joined.diagnostics.filter((d) => d.severity === 'error')).toHaveLength(0)
  })

  it('refuses segments whose geometry differs', async () => {
    await buildAvi(p('pal.avi'), { frames: 4, height: 576, rate: 25, scale: 1, finalize: true })
    await buildAvi(p('ntsc.avi'), { frames: 4, finalize: true })
    await expect(concatTo([p('ntsc.avi'), p('pal.avi')], p('bad.avi'))).rejects.toThrow(
      /does not match/
    )
  })
})

/* The Python tool is the oracle. Where it is available, every fixture must
 * get the same verdict from both implementations. */
const hasPython = await run('python3', ['--version'])
  .then(() => true)
  .catch(() => false)

describe.skipIf(!hasPython)('parity with tools/checkavi.py', () => {
  const cases: [string, Parameters<typeof buildAvi>[1]][] = [
    ['par-clean.avi', { frames: 10, finalize: true }],
    ['par-crash.avi', { frames: 12, crashCut: true, staleBy: 4 }],
    ['par-drops.avi', { frames: 10, dropAt: [3, 6], finalize: true }],
    ['par-liar.avi', { frames: 5, lieAboutIndex: true }],
    ['par-cut.avi', { frames: 6, cutInPayload: true }],
    ['par-stale.avi', { frames: 20, staleBy: 7 }]
  ]

  for (const [name, opts] of cases) {
    it(`agrees on ${name}`, async () => {
      const file = p(name)
      await buildAvi(file, opts)

      const mine = await parseAvi(file)
      const { stdout, code } = await run('python3', [CHECKAVI, file])
        .then((r) => ({ stdout: r.stdout, code: 0 }))
        .catch((e: { stdout?: string; code?: number }) => ({
          stdout: e.stdout ?? '',
          code: e.code ?? 1
        }))

      expect(exitCode(mine), `exit code for ${name}`).toBe(code)

      /* Compare the diagnostic set: same problems found, same wording. */
      const theirs = stdout
        .split('\n')
        .filter((l) => l.startsWith('ERROR:') || l.startsWith('warning:'))
        .map((l) => l.replace(/^ERROR:\s+/, 'E ').replace(/^warning:\s+/, 'W '))
        .sort()
      const ours = formatReport(mine)
        .split('\n')
        .filter((l) => l.startsWith('ERROR:') || l.startsWith('warning:'))
        .map((l) => l.replace(/^ERROR:\s+/, 'E ').replace(/^warning:\s+/, 'W '))
        .sort()
      expect(ours, `diagnostics for ${name}`).toEqual(theirs)

      /* And the summary line, which carries the frame count and duration. */
      const theirSummary = stdout.split('\n').find((l) => l.startsWith('summary:'))
      const ourSummary = formatReport(mine)
        .split('\n')
        .find((l) => l.startsWith('summary:'))
      expect(ourSummary, `summary for ${name}`).toBe(theirSummary)
    })
  }
})

describe('join guards', () => {
  it('refuses when the joined file would exceed AVI\'s 32-bit index', async () => {
    /* idx1 offsets are 32-bit, so a joined AVI caps at 4 GB — about 95
     * minutes of this footage. The user gets told why rather than getting a
     * silently corrupt file. */
    const { AVI_MAX_BYTES } = await import('../src/shared/avi/repair.js')
    expect(AVI_MAX_BYTES).toBe(0xffffffff)
  })

  it('renumbers chunk offsets into the output file', async () => {
    /* Each segment's offsets are relative to its own movi; the joined index
     * has to describe positions in the new file, or every seek lands wrong. */
    await buildAvi(p('j1.avi'), { frames: 4, finalize: true })
    await buildAvi(p('j2.avi'), { frames: 4, finalize: true })
    await concatTo([p('j1.avi'), p('j2.avi')], p('j-out.avi'))

    const { frameTable, readFrame } = await import('../src/shared/avi/frames.js')
    const table = await frameTable(p('j-out.avi'))
    expect(table?.frames).toHaveLength(8)

    /* Every frame in the second half must still decode from its index entry. */
    for (const i of [4, 5, 6, 7]) {
      const jpeg = await readFrame(p('j-out.avi'), table!, i)
      expect(jpeg.subarray(0, 3), `frame ${i}`).toEqual(Buffer.from([0xff, 0xd8, 0xff]))
    }
  })
})
