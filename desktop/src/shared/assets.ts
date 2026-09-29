/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Which file in a release is which.
 *
 * Up to v0.9.7 the two images had the same names in every release,
 * fpvault.bin and u-boot-sunxi-with-spl.bin, so a file sitting in a
 * downloads folder said nothing about what it was - and an update that
 * "did not take" was as likely to be last month's file written again as
 * anything wrong with the board. From v0.9.8 the release tag is part of the
 * name: fpvault-v0.9.8.bin.
 *
 * Both spellings are accepted, because older releases still exist and still
 * have to install. A versioned name is only accepted when its version is the
 * release's own: a file named for one version sitting in another version's
 * release is a mistake in the release, and installing it would be a second
 * one.
 */

export interface NamedAsset {
  name: string
}

function pick<T extends NamedAsset>(assets: T[], stem: string, tag: string): T | undefined {
  return (
    assets.find((a) => a.name === `${stem}-${tag}.bin`) ??
    assets.find((a) => a.name === `${stem}.bin`)
  )
}

/** The firmware image of a release, or undefined if it has none. */
export function firmwareAsset<T extends NamedAsset>(assets: T[], tag: string): T | undefined {
  return pick(assets, 'fpvault', tag)
}

/** The U-Boot image of a release, or undefined if it has none. */
export function ubootAsset<T extends NamedAsset>(assets: T[], tag: string): T | undefined {
  return pick(assets, 'u-boot-sunxi-with-spl', tag)
}
