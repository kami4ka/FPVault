/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Finding and fetching firmware releases.
 *
 * Two things about this project's releases shape the client. Every release
 * so far is flagged pre-release, so /releases/latest returns nothing useful
 * and the list endpoint has to be used instead. And the assets have flat,
 * unversioned names — fpvault.bin is called that in every release — so the
 * download cache has to be keyed by tag or one version would overwrite
 * another.
 */
import { createHash } from 'node:crypto'
import { mkdir, readFile, rename, writeFile } from 'node:fs/promises'
import { join } from 'node:path'

const REPO = 'kami4ka/FPVault'
const API = `https://api.github.com/repos/${REPO}/releases`

/** Unauthenticated GitHub allows 60 requests an hour; this stays far under. */
const CACHE_MS = 6 * 60 * 60 * 1000

export interface FirmwareAsset {
  name: string
  url: string
  size: number
  /** "sha256:…" from the API, verified after download. */
  digest: string | null
}

export interface Release {
  tag: string
  name: string
  prerelease: boolean
  publishedAt: string
  notes: string
  assets: FirmwareAsset[]
}

interface CacheEntry {
  fetchedAt: number
  etag: string | null
  releases: Release[]
}

let cache: CacheEntry | null = null

/** Tags that are firmware, not the desktop app's own releases. */
const FIRMWARE_TAG = /^v\d+\.\d+\.\d+$/

function toRelease(raw: Record<string, unknown>): Release {
  const assets = Array.isArray(raw['assets']) ? (raw['assets'] as Record<string, unknown>[]) : []
  return {
    tag: String(raw['tag_name'] ?? ''),
    name: String(raw['name'] ?? raw['tag_name'] ?? ''),
    prerelease: Boolean(raw['prerelease']),
    publishedAt: String(raw['published_at'] ?? ''),
    notes: String(raw['body'] ?? ''),
    assets: assets.map((a) => ({
      name: String(a['name'] ?? ''),
      url: String(a['browser_download_url'] ?? ''),
      size: Number(a['size'] ?? 0),
      digest: typeof a['digest'] === 'string' ? a['digest'] : null
    }))
  }
}

/** Newest firmware releases first. Cached, conditional, and rate-limit aware. */
export async function listReleases(force = false): Promise<Release[]> {
  if (!force && cache && Date.now() - cache.fetchedAt < CACHE_MS) return cache.releases

  const headers: Record<string, string> = {
    Accept: 'application/vnd.github+json',
    'User-Agent': 'FPVault-Desktop'
  }
  if (cache?.etag) headers['If-None-Match'] = cache.etag

  const res = await fetch(`${API}?per_page=30`, { headers })

  if (res.status === 304 && cache) {
    cache.fetchedAt = Date.now()
    return cache.releases
  }

  if (res.status === 403 || res.status === 429) {
    const reset = Number(res.headers.get('x-ratelimit-reset') ?? 0)
    if (cache) return cache.releases
    const when = reset ? new Date(reset * 1000).toLocaleTimeString() : 'later'
    throw new Error(`GitHub is rate-limiting this machine; try again after ${when}`)
  }
  if (!res.ok) {
    if (cache) return cache.releases
    throw new Error(`could not reach GitHub (HTTP ${res.status})`)
  }

  const raw = (await res.json()) as Record<string, unknown>[]
  const releases = raw
    .map(toRelease)
    /* Drop the desktop app's own releases, which share this repo. */
    .filter((r) => FIRMWARE_TAG.test(r.tag))
    .sort((a, b) => b.publishedAt.localeCompare(a.publishedAt))

  cache = { fetchedAt: Date.now(), etag: res.headers.get('etag'), releases }
  return releases
}

export function newestRelease(releases: Release[]): Release | null {
  return releases[0] ?? null
}

/**
 * Download an asset into `dir/<tag>/<name>` and verify its digest. Assets
 * are cached by tag because their names carry no version.
 */
export async function downloadAsset(
  release: Release,
  asset: FirmwareAsset,
  dir: string,
  onProgress?: (received: number, total: number) => void
): Promise<{ path: string; bytes: Buffer }> {
  const destDir = join(dir, release.tag)
  await mkdir(destDir, { recursive: true })
  const dest = join(destDir, asset.name)

  const expected = asset.digest?.startsWith('sha256:') ? asset.digest.slice(7) : null

  /* A verified copy from an earlier run is reused rather than refetched. */
  try {
    const have = await readFile(dest)
    if (!expected || createHash('sha256').update(have).digest('hex') === expected)
      return { path: dest, bytes: have }
  } catch {
    /* not cached yet */
  }

  const res = await fetch(asset.url, { redirect: 'follow' })
  if (!res.ok || !res.body) throw new Error(`could not download ${asset.name} (HTTP ${res.status})`)

  const chunks: Uint8Array[] = []
  let received = 0
  for await (const chunk of res.body as unknown as AsyncIterable<Uint8Array>) {
    chunks.push(chunk)
    received += chunk.length
    onProgress?.(received, asset.size || received)
  }
  const bytes = Buffer.concat(chunks)

  if (expected) {
    const got = createHash('sha256').update(bytes).digest('hex')
    if (got !== expected)
      throw new Error(
        `${asset.name} does not match the digest GitHub published. ` +
          `Expected ${expected}, got ${got}. Refusing to flash it.`
      )
  }
  if (asset.size && bytes.length !== asset.size)
    throw new Error(`${asset.name}: expected ${asset.size} bytes, received ${bytes.length}`)

  const tmp = `${dest}.part`
  await writeFile(tmp, bytes)
  await rename(tmp, dest)
  return { path: dest, bytes }
}
