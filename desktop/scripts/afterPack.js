/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ad-hoc sign the macOS bundle after packing.
 *
 * An arm64 Mac refuses to run a bundle whose signature does not cover it,
 * and what electron-builder leaves behind when signing is off is only the
 * linker's own signature on the main executable — enough for `codesign -dv`
 * to report `adhoc`, not enough for the app to launch. `spctl` says so
 * plainly: "code has no resources but signature indicates they must be
 * present".
 *
 * So the bundle is signed here with the `-` identity, which needs no
 * certificate and asserts no identity. It makes the app runnable on the
 * machine that built it and nowhere else, which is exactly right while
 * FPVault has no Developer ID: the alternative is electron-builder picking
 * up whatever certificate is in the keychain and shipping a build signed by
 * an unrelated project.
 *
 * The real entitlements and the hardened runtime are applied even so. A
 * local build should fail the same way a release would if one of them is
 * wrong, rather than working here and breaking after notarization.
 */
import { execFileSync } from 'node:child_process'
import { join } from 'node:path'

export default async function afterPack(context) {
  if (context.electronPlatformName !== 'darwin') return

  const app = join(context.appOutDir, `${context.packager.appInfo.productFilename}.app`)
  const entitlements = join(context.packager.info.projectDir, 'resources/entitlements.mac.plist')

  execFileSync(
    'codesign',
    [
      '--force',
      '--deep',
      '--sign',
      '-',
      '--options',
      'runtime',
      '--entitlements',
      entitlements,
      app
    ],
    { stdio: 'inherit' }
  )

  console.log(`  • ad-hoc signed ${app} — runs locally, not distributable`)
}
