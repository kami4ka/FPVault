#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Derive the app icons from the project logo.
#
#   python3 scripts/make-icons.py
#
# Source of truth is docs/img/logo.png — the same mark the repository README
# shows — so the app can never drift away from the project's own identity.
# The outputs are committed, because a build must not need Pillow; this
# script exists so the derivation is repeatable rather than a binary someone
# once made in an image editor and cannot reproduce.
#
# The logo is a mark plus a wordmark side by side. Only the mark goes in the
# icon: at 32 px a wordmark is an unreadable smudge, and the mark alone is
# what the dock, the Alt-Tab switcher and the installer all actually show.
# The split is found by looking for the widest empty column band rather than
# hardcoded, so retouching the logo does not silently crop it wrong.
#
# The mark is drawn on white and is composited onto a white tile, so no
# colour keying happens anywhere: white meets white and there is no halo to
# go wrong. The tile is a superellipse — the continuous-corner shape macOS
# uses — with a hairline in the app's own --color-line so the icon still has
# an edge when it sits on a white background.
#
# Requires Pillow and numpy. macOS additionally needs iconutil for the .icns,
# which ships with the system.

import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent.parent
LOGO = HERE.parent / "docs" / "img" / "logo.png"
OUT = HERE / "resources"

# --color-line from src/renderer/theme.css: the same hairline the UI draws.
LINE = (227, 231, 234)
TILE = (255, 255, 255)

# Apple's icon grid puts the tile in 824 of a 1024 canvas; the margin is
# where the system's own shadow goes. Everything else wants the full square.
MAC_TILE = 824 / 1024
FLAT_TILE = 0.94

# How much of the tile's height the mark fills. The shield is taller than it
# is wide, so fitting by height leaves the sides looking emptier than this
# number suggests: at 0.82 the artwork is only about 68% of the tile across.
# Pushed this far because the mark carries fine detail — propeller blades, a
# waveform, the card contacts — and below roughly this it stops resolving at
# 16 px, where a dock or a Finder list will actually show it.
MARK_OF_TILE = 0.82

# Exponent of the superellipse |x|^n + |y|^n = 1. Five is the value that
# matches the macOS corner.
SQUIRCLE_N = 5.0

SS = 4  # supersampling factor for the tile edges

# Anything at least this bright in every channel is the logo's background
# rather than its artwork, and becomes the tile's own white.
WHITE_FLOOR = 244


def mark() -> Image.Image:
    """The shield alone, cropped out of the logo."""
    logo = Image.open(LOGO).convert("RGB")
    ink = np.asarray(logo).astype(int).sum(axis=2) < 720

    columns = ink.any(axis=0)
    # Empty column bands, as (start, width). The widest one inside the
    # artwork is the space between the mark and the wordmark.
    bands, run = [], None
    for x, filled in enumerate(columns):
        if not filled and run is None:
            run = x
        elif filled and run is not None:
            bands.append((run, x - run))
            run = None

    first = int(np.argmax(columns))
    inner = [b for b in bands if b[0] > first]
    if not inner:
        raise SystemExit("logo has no gap between mark and wordmark")
    split = max(inner, key=lambda b: b[1])[0]

    left = ink[:, :split]
    rows = np.where(left.any(axis=1))[0]
    cols = np.where(left.any(axis=0))[0]
    crop = logo.crop(
        (int(cols.min()), int(rows.min()), int(cols.max()) + 1, int(rows.max()) + 1)
    )

    # The logo's background is 254, not 255, and carries a little compression
    # noise. Against a pure white tile that difference is invisible as colour
    # but perfectly visible as a rectangle, so the near-white is snapped to
    # white. The threshold is high enough that no antialiased edge of the
    # artwork is touched.
    pixels = np.asarray(crop).copy()
    pixels[pixels.min(axis=2) >= WHITE_FLOOR] = 255
    return Image.fromarray(pixels, "RGB")


def superellipse(size: int, n: float = SQUIRCLE_N) -> np.ndarray:
    """A size x size float mask, 1 inside the rounded square."""
    axis = (np.arange(size) + 0.5) / size * 2 - 1
    x, y = np.meshgrid(axis, axis)
    return (np.abs(x) ** n + np.abs(y) ** n <= 1).astype(np.float32)


def tile(canvas: int, fraction: float, art: Image.Image) -> Image.Image:
    """The finished square: hairline, white tile, mark centred on it."""
    side = int(round(canvas * fraction))
    hair = max(1, round(canvas * 0.003))

    outer = superellipse(side * SS)
    gap = int(round(hair * SS))
    inner_side = side * SS - gap * 2
    inner = np.zeros_like(outer)
    inner[gap : gap + inner_side, gap : gap + inner_side] = superellipse(inner_side)

    rgba = np.zeros((side * SS, side * SS, 4), dtype=np.float32)
    rgba[..., :3] = np.array(LINE, dtype=np.float32)
    rgba[inner > 0, :3] = np.array(TILE, dtype=np.float32)
    rgba[..., 3] = outer * 255

    plate = Image.fromarray(rgba.astype(np.uint8), "RGBA").resize(
        (side, side), Image.LANCZOS
    )

    height = int(round(side * MARK_OF_TILE))
    width = max(1, int(round(art.width * height / art.height)))
    shield = art.resize((width, height), Image.LANCZOS)
    plate.paste(shield, ((side - width) // 2, (side - height) // 2))

    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    out.paste(plate, ((canvas - side) // 2, (canvas - side) // 2), plate)
    return out


def main() -> None:
    art = mark()
    print(f"mark {art.width}x{art.height} from {LOGO.name}")
    OUT.mkdir(parents=True, exist_ok=True)

    mac = tile(1024, MAC_TILE, art)
    flat = tile(1024, FLAT_TILE, art)

    # Linux and the dev window, plus what electron-builder falls back to.
    flat.save(OUT / "icon.png")

    # Windows. 256 is the largest size the format stores losslessly per
    # entry, and every smaller one is downscaled from the same master.
    flat.save(
        OUT / "icon.ico",
        sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)],
    )

    iconset = OUT / "icon.iconset"
    for old in sorted(iconset.glob("*.png")) if iconset.exists() else []:
        old.unlink()
    iconset.mkdir(exist_ok=True)
    for size in (16, 32, 128, 256, 512):
        mac.resize((size, size), Image.LANCZOS).save(iconset / f"icon_{size}x{size}.png")
        mac.resize((size * 2, size * 2), Image.LANCZOS).save(
            iconset / f"icon_{size}x{size}@2x.png"
        )

    if sys.platform == "darwin":
        subprocess.run(
            ["iconutil", "-c", "icns", str(iconset), "-o", str(OUT / "icon.icns")],
            check=True,
        )
        for png in iconset.glob("*.png"):
            png.unlink()
        iconset.rmdir()
        print(f"wrote {OUT/'icon.icns'}")
    else:
        print(f"wrote {iconset} — run iconutil on macOS to produce icon.icns")

    print(f"wrote {OUT/'icon.png'} and {OUT/'icon.ico'}")


if __name__ == "__main__":
    main()
