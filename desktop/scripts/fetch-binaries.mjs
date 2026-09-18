#!/usr/bin/env node
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Download the bundled third-party executables named in
 * resources/binaries.lock.json, verify their sha256, and put them where
 * resolveBin.ts expects to find them.
 *
 * The binaries are not committed: they are ~45-80 MB each and four platforms
 * of them would dwarf a firmware repo. Pinning by digest means a build is
 * still reproducible and a tampered download fails loudly rather than
 * quietly shipping.
 *
 *   node scripts/fetch-binaries.mjs              # this machine's platform
 *   node scripts/fetch-binaries.mjs --all        # every platform, for CI
 *   node scripts/fetch-binaries.mjs --platform darwin-arm64
 */
import { createHash } from 'node:crypto'
import { chmod, mkdir, readFile, rename, stat, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '..')
const lockPath = join(root, 'resources', 'binaries.lock.json')
const binRoot = join(root, 'resources', 'bin')

const args = process.argv.slice(2)
const wantAll = args.includes('--all')
const explicit = args[args.indexOf('--platform') + 1]
const thisPlatform = `${process.platform}-${process.arch}`

function sha256(buf) {
  return createHash('sha256').update(buf).digest('hex')
}

async function alreadyGood(path, expected) {
  try {
    await stat(path)
    return sha256(await readFile(path)) === expected
  } catch {
    return false
  }
}

async function fetchOne(tool, platform, spec) {
  const name = spec.exe ? `${tool}.exe` : tool
  const dir = join(binRoot, platform)
  const dest = join(dir, name)

  if (await alreadyGood(dest, spec.sha256)) {
    console.log(`  ${platform}/${name}: already present and verified`)
    return
  }

  process.stdout.write(`  ${platform}/${name}: downloading… `)
  const res = await fetch(spec.url, { redirect: 'follow' })
  if (!res.ok) throw new Error(`${spec.url}: HTTP ${res.status}`)
  const buf = Buffer.from(await res.arrayBuffer())

  const got = sha256(buf)
  if (got !== spec.sha256) {
    throw new Error(
      `${tool} for ${platform} does not match the lockfile.\n` +
        `  expected sha256 ${spec.sha256}\n  got      sha256 ${got}\n` +
        `Refusing to use it.`
    )
  }
  if (spec.size && buf.length !== spec.size)
    throw new Error(`${tool} for ${platform}: expected ${spec.size} bytes, got ${buf.length}`)

  await mkdir(dir, { recursive: true })
  const tmp = `${dest}.part`
  await writeFile(tmp, buf)
  await chmod(tmp, 0o755)
  await rename(tmp, dest)
  console.log(`ok (${(buf.length / 1024 ** 2).toFixed(1)} MB, sha256 verified)`)
}

const lock = JSON.parse(await readFile(lockPath, 'utf8'))

for (const [tool, entry] of Object.entries(lock)) {
  if (tool.startsWith('_')) continue
  const platforms = Object.entries(entry.platforms)
  const targets = wantAll
    ? platforms
    : platforms.filter(([p]) => p === (explicit ?? thisPlatform))

  if (!targets.length) {
    console.log(`${tool}: nothing to fetch for ${explicit ?? thisPlatform}`)
    continue
  }

  console.log(`${tool} ${entry.version} (${entry.license})`)
  for (const [platform, spec] of targets) await fetchOne(tool, platform, spec)
}

console.log('binaries ready')
