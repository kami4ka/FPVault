#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""checkavi.py - structural verifier for the DVR's AVI 1.0 MJPEG files.

Built for power-pull testing: the recorder patches its header about once a
second and appends chunks until the power dies, so a pulled file usually has
a stale (too small) header and no idx1. This tool walks the actual bytes,
independently of the header, and reports what is really recoverable.

Checks: RIFF/AVI structure, stream headers, per-chunk DWORD alignment,
movi/frame-count consistency between header and the walked chunks, idx1
entries against the real chunk offsets (ffmpeg convention: dwOffset relative
to the 'movi' fourcc), and - when PIL is importable - a full decode of every
non-empty JPEG payload.

Exit codes:
  0  structurally sound; power-loss truncation or a stale header is reported
     as a warning ("truncated after N frames"), not an error, as long as
     every complete chunk is intact
  1  structural error (bad fourcc, misalignment, idx1 mismatch, undecodable
     JPEG payload, ...)
  2  usage error / unreadable file

stdlib only; PIL (pillow) is optional and only enables the payload decode.
"""

import io
import struct
import sys

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

errors = []
warnings = []


def err(msg):
    errors.append(msg)
    print("ERROR:   " + msg)


def warn(msg):
    warnings.append(msg)
    print("warning: " + msg)


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def fcc(buf, off):
    return buf[off:off + 4]


def parse_hdrl(buf, off, end):
    """Parse the hdrl LIST: returns (avih dict, [stream dicts])."""
    avih = None
    streams = []
    while off + 8 <= end:
        cid = fcc(buf, off)
        csz = u32(buf, off + 4)
        body = off + 8
        if body + csz > end:
            warn("hdrl chunk %r at 0x%x overruns its LIST" % (cid, off))
            break
        if cid == b"avih" and csz >= 56:
            avih = {
                "usperframe": u32(buf, body + 0),
                "maxbps": u32(buf, body + 4),
                "padgran": u32(buf, body + 8),
                "flags": u32(buf, body + 12),
                "totalframes": u32(buf, body + 16),
                "streams": u32(buf, body + 24),
                "width": u32(buf, body + 32),
                "height": u32(buf, body + 36),
            }
        elif cid == b"LIST" and fcc(buf, body) == b"strl":
            s = {}
            soff = body + 4
            while soff + 8 <= body + csz:
                sid = fcc(buf, soff)
                ssz = u32(buf, soff + 4)
                sbody = soff + 8
                if sid == b"strh" and ssz >= 56:
                    s.update({
                        "type": fcc(buf, sbody).decode("ascii", "replace"),
                        "handler": fcc(buf, sbody + 4).decode("ascii", "replace"),
                        "scale": u32(buf, sbody + 20),
                        "rate": u32(buf, sbody + 24),
                        "length": u32(buf, sbody + 32),
                    })
                elif sid == b"strf" and ssz >= 40 and s.get("type") == "vids":
                    s["biwidth"] = u32(buf, sbody + 4)
                    s["biheight"] = u32(buf, sbody + 8)
                    s["compression"] = fcc(buf, sbody + 16).decode("ascii", "replace")
                soff = sbody + ssz + (ssz & 1)
            streams.append(s)
        off = body + csz + (csz & 1)
    return avih, streams


def walk_movi(buf, flen, movi_fcc_off, claimed_end):
    """Walk '00dc' chunks starting right after the 'movi' fourcc; the walk
    trusts the bytes, not the header, so it keeps going past claimed_end
    (stale header after a power pull) until EOF or a non-chunk fourcc.
    Returns (chunks [(abs_off, size)], truncated_flag, walk_end)."""
    chunks = []
    truncated = False
    off = movi_fcc_off + 4
    while True:
        if off + 8 > flen:
            if off != flen:
                truncated = True
                warn("file ends inside a chunk header at 0x%x" % off)
            break
        cid = fcc(buf, off)
        if cid != b"00dc":
            if cid not in (b"idx1", b"LIST", b"JUNK"):
                # not a known follower: garbage or a torn write
                truncated = True
                warn("movi walk stopped at unknown fourcc %r (0x%x)" % (cid, off))
            break
        if off % 4:
            err("chunk %d at 0x%x is not DWORD-aligned" % (len(chunks), off))
        size = u32(buf, off + 4)
        pad = (4 - (size & 3)) & 3
        if off + 8 + size > flen:
            truncated = True
            warn("chunk %d at 0x%x cut short by EOF (payload %d, have %d)"
                 % (len(chunks), off, size, flen - off - 8))
            break
        if off + 8 + size + pad > flen:
            truncated = True
            warn("chunk %d at 0x%x missing pad bytes at EOF" % (len(chunks), off))
            # payload complete: keep the frame
            chunks.append((off, size))
            off = flen
            break
        if any(buf[off + 8 + size:off + 8 + size + pad]):
            warn("chunk %d at 0x%x has non-zero pad bytes" % (len(chunks), off))
        chunks.append((off, size))
        off += 8 + size + pad
    if not truncated and claimed_end is not None and off != claimed_end:
        if off > claimed_end:
            warn("movi holds data past the header's movi size "
                 "(stale header, power cut after last refresh?)")
        else:
            warn("movi ends 0x%x before the header's movi size" % (claimed_end - off))
    return chunks, truncated, off


def check_idx1(buf, flen, idx_off, idx_size, chunks, movi_fcc_off):
    n = idx_size // 16
    if idx_off + 8 + idx_size > flen:
        have = max(0, (flen - idx_off - 8) // 16)
        warn("idx1 truncated: %d of %d entries present" % (have, n))
        n = have
    if n != len(chunks):
        # a short index (staging RAM ran out) is usable; a long one is not
        (warn if n < len(chunks) else err)(
            "idx1 has %d entries, movi has %d chunks" % (n, len(chunks)))
    convention = None  # 'movi-relative' (ours/ffmpeg) or 'absolute'
    if n and len(chunks):
        first = u32(buf, idx_off + 8 + 8)
        if first == chunks[0][0] - movi_fcc_off:
            convention = "movi-relative"
        elif first == chunks[0][0]:
            convention = "absolute"
            warn("idx1 uses absolute offsets (not our writer's convention)")
        else:
            err("idx1 entry 0 offset %d matches no known convention" % first)
            return
    base = 0 if convention == "absolute" else movi_fcc_off
    for i in range(min(n, len(chunks))):
        e = idx_off + 8 + 16 * i
        cid = fcc(buf, e)
        eoff = u32(buf, e + 8)
        esize = u32(buf, e + 12)
        aoff, asize = chunks[i]
        if cid != b"00dc":
            err("idx1 entry %d: fourcc %r" % (i, cid))
        if eoff + base != aoff:
            err("idx1 entry %d: offset %d, chunk really at %d"
                % (i, eoff, aoff - base))
        if esize != asize:
            err("idx1 entry %d: size %d, chunk stores %d" % (i, esize, asize))


def decode_payloads(buf, chunks):
    bad = 0
    for i, (off, size) in enumerate(chunks):
        if size == 0:
            continue
        try:
            im = Image.open(io.BytesIO(buf[off + 8:off + 8 + size]))
            im.load()
        except Exception as e:
            err("chunk %d: JPEG decode failed: %s" % (i, e))
            bad += 1
    return bad


def main(argv):
    if len(argv) != 2:
        print("usage: checkavi.py <file.avi>", file=sys.stderr)
        return 2
    try:
        with open(argv[1], "rb") as f:
            buf = f.read()
    except OSError as e:
        print("cannot read %s: %s" % (argv[1], e), file=sys.stderr)
        return 2
    flen = len(buf)
    print("%s: %d bytes" % (argv[1], flen))

    if flen < 12 or fcc(buf, 0) != b"RIFF" or fcc(buf, 8) != b"AVI ":
        err("not a RIFF/AVI file (header truncated or foreign)")
        return 1
    riffsz = u32(buf, 4)
    if riffsz + 8 > flen:
        warn("RIFF size 0x%x overruns the file (truncated)" % riffsz)
    elif riffsz + 8 < flen:
        warn("%d bytes past the RIFF end (stale header after power cut?)"
             % (flen - riffsz - 8))

    # top-level walk: find hdrl, movi, idx1
    avih = None
    streams = []
    movi_fcc_off = None
    claimed_movi_end = None
    idx_off = idx_size = None
    off = 12
    while off + 8 <= flen:
        cid = fcc(buf, off)
        csz = u32(buf, off + 4)
        body = off + 8
        if cid == b"LIST" and body + 4 <= flen:
            ltype = fcc(buf, body)
            if ltype == b"hdrl":
                avih, streams = parse_hdrl(buf, body + 4, min(body + csz, flen))
            elif ltype == b"movi":
                movi_fcc_off = body
                claimed_movi_end = body + csz
        elif cid == b"idx1":
            idx_off, idx_size = off, csz
        off = body + csz + (csz & 1)

    if avih is None:
        err("no avih header found")
        return 1
    if movi_fcc_off is None:
        err("no movi LIST found")
        return 1

    for i, s in enumerate(streams):
        print("stream %d: %s/%s scale=%d rate=%d length=%d %s" % (
            i, s.get("type", "?"), s.get("handler", "?"),
            s.get("scale", 0), s.get("rate", 0), s.get("length", 0),
            "%dx%d %s" % (s.get("biwidth", 0), s.get("biheight", 0),
                          s.get("compression", "?"))
            if s.get("type") == "vids" else ""))
    if len(streams) != avih["streams"]:
        err("avih says %d streams, hdrl defines %d"
            % (avih["streams"], len(streams)))

    chunks, truncated, walk_end = walk_movi(
        buf, flen, movi_fcc_off,
        claimed_movi_end if claimed_movi_end and claimed_movi_end <= flen else None)

    nframes = len(chunks)
    drops = sum(1 for _, s in chunks if s == 0)
    sizes = [s for _, s in chunks if s]

    if truncated:
        warn("truncated after %d frames" % nframes)
    if avih["totalframes"] != nframes:
        (warn if avih["totalframes"] < nframes else err)(
            "header counts %d frames, movi walk found %d%s"
            % (avih["totalframes"], nframes,
               " (stale header, expected after a power cut)"
               if avih["totalframes"] < nframes else ""))

    if idx_off is not None:
        check_idx1(buf, flen, idx_off, idx_size, chunks, movi_fcc_off)
    elif avih["flags"] & 0x10:
        warn("AVIF_HASINDEX set but no idx1 (power cut before finalize?)")
    else:
        print("no idx1 (not promised by header)")

    if HAVE_PIL:
        bad = decode_payloads(buf, chunks)
        print("payload decode: %d/%d JPEGs ok (PIL)" % (len(sizes) - bad, len(sizes)))
    else:
        print("payload decode: skipped (PIL not installed)")

    vid = next((s for s in streams if s.get("type") == "vids"), {})
    rate, scale = vid.get("rate", 0), vid.get("scale", 0)
    fps = rate / scale if scale else (1e6 / avih["usperframe"]
                                      if avih["usperframe"] else 0.0)
    print("summary: %d frames (%d drops), %.3f fps, %.2f s, "
          "frame size max %d avg %d" % (
              nframes, drops, fps, nframes / fps if fps else 0.0,
              max(sizes) if sizes else 0,
              sum(sizes) // len(sizes) if sizes else 0))
    if errors:
        print("RESULT: %d error(s), %d warning(s)" % (len(errors), len(warnings)))
        return 1
    print("RESULT: ok (%d warning(s))" % len(warnings))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
