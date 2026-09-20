#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cut a FPVault release.
#
#   tools/release.sh v0.9.5 notes.md path/to/u-boot-sunxi-with-spl.bin \
#       [--requires-recovery]
#
# Releases were hand-assembled before this, which is how v0.9.4 nearly went
# out without saying the thing that mattered most about it: its fix is in
# U-Boot, and the in-app USB update cannot write U-Boot.
#
# --requires-recovery appends a trailer to the notes:
#
#     FPVault-Requires-Recovery: yes
#
# FPVault Desktop parses that, strips it from the notes it displays, and
# warns beside the Install button that this release has to go on over
# recovery instead. Pass it whenever the release changes anything outside the
# firmware slot at 0x100000 - U-Boot at offset 0 above all.
#
# It cannot be inferred from the assets. Every release ships a U-Boot image
# whether or not that image changed, and two builds of identical source
# differ anyway because U-Boot stamps its build date in. So the release has
# to declare it, and this script is where that decision gets recorded.
set -euo pipefail

TRAILER="FPVault-Requires-Recovery: yes"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

die() { echo "error: $*" >&2; exit 1; }

[ $# -ge 3 ] || die "usage: $0 <tag> <notes.md> <u-boot-sunxi-with-spl.bin> [--requires-recovery]"

TAG="$1"; NOTES="$2"; UBOOT="$3"; shift 3
REQUIRES_RECOVERY=0
for arg in "$@"; do
  case "$arg" in
    --requires-recovery) REQUIRES_RECOVERY=1 ;;
    *) die "unknown option: $arg" ;;
  esac
done

[ -f "$NOTES" ]  || die "no such notes file: $NOTES"
[ -f "$UBOOT" ]  || die "no such U-Boot image: $UBOOT"

# The tag and the version the board will report over USB must agree, or the
# desktop app names the build something the release does not.
VERSION="$(sed -n 's/^#define FW_VERSION_STR "\(.*\)"$/\1/p' "$HERE/src/board.h")"
[ -n "$VERSION" ] || die "could not read FW_VERSION_STR from src/board.h"
[ "$TAG" = "v$VERSION" ] || die "tag $TAG does not match src/board.h version $VERSION"

echo "==> building firmware $VERSION"
make -C "$HERE" >/dev/null
BIN="$HERE/build/fpvault.bin"

# The same two checks the board makes before it burns anything, so a bad
# image is caught here rather than by every person who downloads it.
python3 - "$BIN" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
if len(b) > 0x40000:
    sys.exit(f'firmware is {len(b)} bytes, over the 256 KB NOR slot')
if b[3] != 0xEA:
    sys.exit(f'firmware byte 3 is {b[3]:#x}, not the 0xEA branch the loader expects')
print(f'    fpvault.bin {len(b)} bytes, load header ok')
PY

python3 - "$UBOOT" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
if b[4:12] != b'eGON.BT0':
    sys.exit('U-Boot image has no eGON.BT0 header; is this really u-boot-sunxi-with-spl.bin?')
print(f'    u-boot      {len(b)} bytes, eGON header ok')
PY

BODY="$(mktemp)"
trap 'rm -f "$BODY"' EXIT
cat "$NOTES" > "$BODY"
if [ "$REQUIRES_RECOVERY" = 1 ]; then
  printf '\n%s\n' "$TRAILER" >> "$BODY"
  echo "==> marked as requiring recovery"
else
  echo "==> NOT marked as requiring recovery"
  echo "    If this release changes U-Boot, stop and re-run with"
  echo "    --requires-recovery. Install over USB writes only the firmware"
  echo "    slot at 0x100000 and would deliver half of it."
fi

echo "==> creating $TAG"
gh release create "$TAG" \
  --title "$(head -1 "$NOTES" | sed 's/^#* *//')" \
  --notes-file "$BODY" \
  --prerelease \
  "$BIN" "$UBOOT"
