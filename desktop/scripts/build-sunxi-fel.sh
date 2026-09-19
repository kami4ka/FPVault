#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build a fully static sunxi-fel for the current platform.
#
# Unlike ffmpeg there is no prebuilt sunxi-fel for any platform this app
# ships to — docs/BRINGUP.md already notes that macOS has no Homebrew
# formula — so it is built from a pinned commit instead of downloaded.
# Linking libusb and libfdt statically matters: a binary that reaches for
# Homebrew dylibs works on the machine that built it and nowhere else, and
# macOS notarization is far simpler without library validation exemptions.
#
#   ./scripts/build-sunxi-fel.sh            # into resources/bin/<platform>/
#
# Prerequisites: a C compiler, plus static libusb-1.0, libfdt and zlib.
#   macOS  brew install libusb dtc pkgconf
#   Debian apt install libusb-1.0-0-dev libfdt-dev zlib1g-dev
set -euo pipefail

COMMIT="${SUNXI_TOOLS_COMMIT:-d7bbd17}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="$(node -p 'process.platform + "-" + process.arch')"
OUT="$HERE/resources/bin/$PLATFORM"
WORK="${TMPDIR:-/tmp}/sunxi-tools-build"

mkdir -p "$OUT"
[ -d "$WORK" ] || git clone https://github.com/linux-sunxi/sunxi-tools.git "$WORK"
git -C "$WORK" fetch --all --quiet || true
git -C "$WORK" checkout --quiet "$COMMIT" 2>/dev/null || echo "warning: commit $COMMIT not found, building HEAD"

cd "$WORK"
SRC="fel.c fit_image.c progress.c soc_info.c fel_lib.c fel-spiflash.c"

case "$(uname -s)" in
  Darwin)
    PREFIX="$(brew --prefix)"
    cc -std=c99 -O2 -Wall -Iinclude/ \
       -I"$PREFIX/opt/libusb/include/libusb-1.0" -I"$PREFIX/opt/dtc/include" \
       -o sunxi-fel $SRC \
       "$PREFIX/opt/libusb/lib/libusb-1.0.a" "$PREFIX/opt/dtc/lib/libfdt.a" -lz \
       -framework IOKit -framework CoreFoundation -framework Security
    ;;
  *)
    cc -std=c99 -O2 -Wall -Iinclude/ \
       $(pkg-config --cflags libusb-1.0 libfdt) \
       -o sunxi-fel $SRC \
       -Wl,-Bstatic $(pkg-config --libs --static libusb-1.0 libfdt) -lz -Wl,-Bdynamic -lpthread
    ;;
esac

cp sunxi-fel "$OUT/sunxi-fel"
chmod 0755 "$OUT/sunxi-fel"
echo "built $OUT/sunxi-fel"
