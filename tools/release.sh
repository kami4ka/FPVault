#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cut a FPVault release.
#
#   tools/release.sh v0.9.8 notes.md path/to/u-boot-sunxi-with-spl.bin \
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

# board.h states the version twice: the string the banner prints, and the
# three numbers that become bcdDevice, which is what the board reports over
# USB and what FPVault Desktop reads. v0.9.6 shipped with only the string
# bumped, so it announced itself as 0.9.5 to every host that asked.
num() { sed -n "s/^#define FW_VERSION_$1 \\([0-9]*\\)$/\\1/p" "$HERE/src/board.h"; }
NUMERIC="$(num MAJOR).$(num MINOR).$(num PATCH)"
[ "$NUMERIC" = "$VERSION" ] || die "FW_VERSION_MAJOR/MINOR/PATCH say $NUMERIC but FW_VERSION_STR says $VERSION"

# The binary carries the commit it was built from in its banner. That is only
# true if it was built from a commit: v0.9.7 was compiled a moment before its
# version bump was committed, and make saw nothing to rebuild afterwards, so
# it names the commit before the one its tag points at. Refuse to build from
# a tree that differs from HEAD, and build from nothing.
git -C "$HERE" diff --quiet HEAD -- src vendor Makefile uboot \
  || die "src/, vendor/, uboot/ or the Makefile differ from HEAD; commit first"

echo "==> building firmware $VERSION from $(git -C "$HERE" rev-parse --short HEAD)"
rm -rf "$HERE/build" "$HERE/build-debug"
make -C "$HERE" >/dev/null
make -C "$HERE" debug >/dev/null
BIN="$HERE/build/fpvault.bin"
# The debug build: the same firmware, plus a text log written to the card in
# USB mode (src/dlog.h). For sending to someone whose board misbehaves where
# there is no serial console to watch.
DBG="$HERE/build-debug/fpvault-debug.bin"

# The same two checks the board makes before it burns anything, so a bad
# image is caught here rather than by every person who downloads it.
#
# And each build has to be the one its name says. A debug image published as
# the normal one would write a log file to every card it met; the reverse
# would send a tester away to collect a log that is never written.
for image in "$BIN:normal" "$DBG:debug"; do
python3 - "${image%:*}" "${image##*:}" <<'PY'
import os, sys
path, kind = sys.argv[1], sys.argv[2]
b = open(path, 'rb').read()
if len(b) > 0x40000:
    sys.exit(f'firmware is {len(b)} bytes, over the 256 KB NOR slot')
if b[3] != 0xEA:
    sys.exit(f'firmware byte 3 is {b[3]:#x}, not the 0xEA branch the loader expects')
logs = b'FPVLOG.TXT' in b
if logs != (kind == 'debug'):
    sys.exit(f'{os.path.basename(path)} is meant to be the {kind} build but '
             f'{"contains" if logs else "does not contain"} the debug log')
print(f'    {os.path.basename(path)} {len(b)} bytes, load header ok, {kind} build')
PY
done

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

# Every file a release publishes says which release it is from. With the same
# two names in every release, a file in a downloads folder could be any
# version, and "the update did not help" could mean the old file was written
# again. The U-Boot image gets the tag too even when it has not changed: it is
# the name of the release it shipped in, not a claim that it is new.
STAGE="$(mktemp -d)"
trap 'rm -f "$BODY"; rm -rf "$STAGE"' EXIT
FW_OUT="$STAGE/fpvault-$TAG.bin"
# The tag comes before "debug" so that the name FPVault Desktop looks for,
# fpvault-<tag>.bin, can never be this file.
DBG_OUT="$STAGE/fpvault-$TAG-debug.bin"
UB_OUT="$STAGE/u-boot-sunxi-with-spl-$TAG.bin"
cp "$BIN" "$FW_OUT"
cp "$DBG" "$DBG_OUT"
cp "$UBOOT" "$UB_OUT"
echo "==> assets: $(basename "$FW_OUT"), $(basename "$DBG_OUT"), $(basename "$UB_OUT")"

echo "==> creating $TAG"
gh release create "$TAG" \
  --title "$(head -1 "$NOTES" | sed 's/^#* *//')" \
  --notes-file "$BODY" \
  --prerelease \
  "$FW_OUT" "$DBG_OUT" "$UB_OUT"
