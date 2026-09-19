/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * FEL recovery: writing flash through the SoC's boot ROM.
 *
 * This is the path for a board that cannot be reached any other way — blank
 * flash, a firmware too old to have a DFU interface, or a bad image that
 * bricked the normal boot. The boot ROM enumerates as its own device and
 * accepts uploads, which is why a FPVault board is always recoverable with
 * nothing but a USB cable (docs/BRINGUP.md).
 *
 * sunxi-fel does the talking rather than a reimplementation here. Its
 * spiflash-write uploads a small ARM payload to drive the SPI controller on
 * the target; porting that is real work for a path a user hits once, and the
 * proven tool is the right answer on a recovery route.
 */
import { spawn } from 'node:child_process'
import { app } from 'electron'
import { join } from 'node:path'
import { resolveBin } from '../tools/resolveBin.js'
import { downloadAsset, listReleases, type Release } from '../update/releases.js'
import { validateImage } from '../update/dfu.js'
import type { JobContext } from './queue.js'

/** NOR layout, from docs/BRINGUP.md and uboot/f1c200s_dvr_defconfig. */
const UBOOT_OFFSET = '0'
const FIRMWARE_OFFSET = '0x100000'

export interface RecoverResult {
  tag: string
  wroteUboot: boolean
}

export class FelUnavailable extends Error {
  constructor() {
    super('sunxi-fel was not bundled with this build')
    this.name = 'FelUnavailable'
  }
}

/** Run sunxi-fel, forwarding its `-p` percentage bar as job progress. */
function runFel(
  args: string[],
  onLine: (line: string) => void,
  onPercent: (pct: number) => void
): Promise<void> {
  return new Promise((resolve_, reject) => {
    void resolveBin('sunxi-fel').then((bin) => {
      if (!bin) return reject(new FelUnavailable())

      const child = spawn(bin, args, { stdio: ['ignore', 'pipe', 'pipe'] })
      const tail: string[] = []

      const absorb = (chunk: string) => {
        for (const raw of chunk.split(/[\r\n]+/)) {
          const line = raw.trim()
          if (!line) continue
          tail.push(line)
          if (tail.length > 20) tail.shift()
          const pct = /(\d{1,3})%/.exec(line)
          if (pct?.[1]) onPercent(Number(pct[1]) / 100)
          else onLine(line)
        }
      }

      child.stdout.setEncoding('utf8')
      child.stdout.on('data', absorb)
      child.stderr.setEncoding('utf8')
      child.stderr.on('data', absorb)

      child.on('error', reject)
      child.on('close', (code) => {
        if (code === 0) return resolve_()
        reject(new Error(tail.slice(-4).join('\n') || `sunxi-fel exited with code ${code}`))
      })
    })
  })
}

/** Confirm the boot ROM is there and is the SoC we expect. */
export async function felProbe(): Promise<string | null> {
  const bin = await resolveBin('sunxi-fel')
  if (!bin) throw new FelUnavailable()

  let out = ''
  try {
    await runFel(['ver'], (l) => (out += l + '\n'), () => {})
  } catch {
    return null
  }
  /* docs/BRINGUP.md: expect AWUSBFEX soc=00001663 for the F1C100s/F1C200s. */
  return /AWUSBFEX/.test(out) ? out.trim() : null
}

/**
 * Write a release to a board sitting in FEL.
 *
 * `withUboot` is for a board whose flash is blank or damaged; a board that
 * merely has old firmware already has a working U-Boot and only needs the
 * firmware slot rewritten.
 */
export async function recoverOverFel(
  tag: string,
  withUboot: boolean,
  ctx: JobContext
): Promise<RecoverResult> {
  const releases = await listReleases()
  const release: Release | undefined = releases.find((r) => r.tag === tag)
  if (!release) throw new Error(`no release tagged ${tag}`)

  const ver = await felProbe()
  if (!ver)
    throw new Error(
      'no board in recovery mode. Unplug it, hold SW2, plug it back in while still holding, then try again.'
    )
  ctx.report(0.02, ver)

  const dir = join(app.getPath('userData'), 'firmware')

  /* U-Boot is 414,840 bytes against the firmware's 80,096, so it dominates
   * the time; weight the bar accordingly rather than showing two bars. */
  const fwAsset = release.assets.find((a) => a.name === 'fpvault.bin')
  if (!fwAsset) throw new Error(`release ${tag} has no fpvault.bin`)
  const ubAsset = release.assets.find((a) => a.name === 'u-boot-sunxi-with-spl.bin')
  if (withUboot && !ubAsset) throw new Error(`release ${tag} has no u-boot-sunxi-with-spl.bin`)

  ctx.report(0.05, `downloading ${tag}`)
  const fw = await downloadAsset(release, fwAsset, dir)
  const bad = validateImage(fw.bytes)
  if (bad) throw new Error(bad)

  const ub = withUboot && ubAsset ? await downloadAsset(release, ubAsset, dir) : null

  const ubShare = ub ? 0.7 : 0
  let base = 0.1

  if (ub) {
    ctx.report(base, 'writing U-Boot — do not unplug the board')
    await runFel(
      ['-p', 'spiflash-write', UBOOT_OFFSET, ub.path],
      (l) => ctx.report(base, l),
      (pct) => {
        ctx.throwIfCancelled()
        ctx.report(base + pct * ubShare, `U-Boot ${Math.round(pct * 100)}%`)
      }
    )
    base += ubShare
  }

  const fwShare = 0.88 - base
  ctx.report(base, 'writing firmware — do not unplug the board')
  await runFel(
    ['-p', 'spiflash-write', FIRMWARE_OFFSET, fw.path],
    (l) => ctx.report(base, l),
    (pct) => {
      ctx.throwIfCancelled()
      ctx.report(base + pct * fwShare, `firmware ${Math.round(pct * 100)}%`)
    }
  )

  ctx.report(1, `${tag} written. Unplug the board and plug it back in.`)
  return { tag, wroteUboot: Boolean(ub) }
}

export async function felAvailable(): Promise<boolean> {
  return (await resolveBin('sunxi-fel')) !== null
}
