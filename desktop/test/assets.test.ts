/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Release assets are found by name, and the names changed at v0.9.8. Getting
 * this wrong is quiet in both directions: miss the new names and no release
 * from now on can be installed; drop the old ones and no earlier release can.
 */
import { describe, expect, it } from 'vitest'
import { firmwareAsset, ubootAsset } from '../src/shared/assets'

const named = (...names: string[]) => names.map((name) => ({ name }))

describe('release assets', () => {
  it('finds the unversioned names every release up to v0.9.7 used', () => {
    const a = named('fpvault.bin', 'u-boot-sunxi-with-spl.bin')
    expect(firmwareAsset(a, 'v0.9.7')?.name).toBe('fpvault.bin')
    expect(ubootAsset(a, 'v0.9.7')?.name).toBe('u-boot-sunxi-with-spl.bin')
  })

  it('finds names that carry the release tag', () => {
    const a = named('fpvault-v0.9.8.bin', 'u-boot-sunxi-with-spl-v0.9.8.bin')
    expect(firmwareAsset(a, 'v0.9.8')?.name).toBe('fpvault-v0.9.8.bin')
    expect(ubootAsset(a, 'v0.9.8')?.name).toBe('u-boot-sunxi-with-spl-v0.9.8.bin')
  })

  it('refuses a file named for a different version than the release', () => {
    const a = named('fpvault-v0.9.7.bin', 'u-boot-sunxi-with-spl-v0.9.7.bin')
    expect(firmwareAsset(a, 'v0.9.8')).toBeUndefined()
    expect(ubootAsset(a, 'v0.9.8')).toBeUndefined()
  })

  it('prefers the versioned file when a release carries both', () => {
    const a = named('fpvault.bin', 'fpvault-v0.9.8.bin')
    expect(firmwareAsset(a, 'v0.9.8')?.name).toBe('fpvault-v0.9.8.bin')
  })

  it('does not mistake one image for the other, or a near miss for either', () => {
    const a = named('u-boot-sunxi-with-spl-v0.9.8.bin', 'fpvault-v0.9.8.bin.sig', 'fpvault-v0.9.8.zip')
    expect(firmwareAsset(a, 'v0.9.8')).toBeUndefined()
  })

  it('never installs the debug build, which logs to the card', () => {
    const a = named(
      'fpvault-v0.9.9-debug.bin',
      'fpvault-v0.9.9.bin',
      'u-boot-sunxi-with-spl-v0.9.9.bin'
    )
    expect(firmwareAsset(a, 'v0.9.9')?.name).toBe('fpvault-v0.9.9.bin')
    // Nor as a fallback, in a release that somehow has nothing else.
    expect(firmwareAsset(named('fpvault-v0.9.9-debug.bin'), 'v0.9.9')).toBeUndefined()
    expect(firmwareAsset(named('fpvault-debug.bin'), 'v0.9.9')).toBeUndefined()
  })

  it('reports a release with no images as having none', () => {
    expect(firmwareAsset([], 'v0.9.8')).toBeUndefined()
    expect(ubootAsset(named('notes.txt'), 'v0.9.8')).toBeUndefined()
  })
})
