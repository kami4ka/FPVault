#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
vedump.py - pull a hardware-encoded JPEG off the DVR console.

Serial mode (default): send 'j', capture the base64 between the BEGIN/END
markers, decode, save, and (if PIL is available) actually decode the image
as the pass/fail check for milestone M1.

    vedump.py /dev/cu.usbserial-0001 [--out ve.jpg]
    vedump.py --stdin < captured_console_log.txt

Opens the port with DTR/RTS held low (same reason as loader.py: don't reset
an in-line ESP32 bridge).
"""
import argparse
import base64
import re
import sys
import time


def extract(text):
    m = re.search(r"-----BEGIN JPEG (\d+)-----\r?\n(.*?)-----END JPEG-----",
                  text, re.S)
    if not m:
        raise SystemExit("no BEGIN/END JPEG block found")
    want = int(m.group(1))
    data = base64.b64decode(re.sub(r"[^A-Za-z0-9+/=]", "", m.group(2)))
    if len(data) != want:
        print(f"WARN: length {len(data)} != announced {want}")
    return data


def verify(data, path):
    if not (data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"):
        print("FAIL: missing SOI/EOI")
        return 1
    try:
        from PIL import Image
        import io
        img = Image.open(io.BytesIO(data))
        img.load()
        print(f"OK: decodes as {img.size[0]}x{img.size[1]} {img.mode}, "
              f"{len(data)} bytes -> {path}")
        return 0
    except ImportError:
        print(f"saved {len(data)} bytes -> {path} (PIL not available, "
              "structure-only check passed)")
        return 0
    except Exception as e:
        print(f"FAIL: JPEG does not decode: {e}")
        return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("--out", default="ve.jpg")
    ap.add_argument("--stdin", action="store_true")
    ap.add_argument("--timeout", type=float, default=30.0)
    a = ap.parse_args()

    if a.stdin:
        text = sys.stdin.read()
    else:
        if not a.port:
            ap.error("port required unless --stdin")
        import serial
        s = serial.Serial()
        s.port = a.port
        s.baudrate = 115200
        s.timeout = 0.5
        s.dtr = False
        s.rts = False
        s.open()
        time.sleep(0.8)
        s.reset_input_buffer()
        s.write(b"j")
        chunks, end = [], time.time() + a.timeout
        while time.time() < end:
            chunk = s.read(4096)
            if chunk:
                chunks.append(chunk)
                if b"-----END JPEG-----" in b"".join(chunks[-2:]):
                    break
        s.close()
        text = b"".join(chunks).decode(errors="replace")

    data = extract(text)
    with open(a.out, "wb") as f:
        f.write(data)
    sys.exit(verify(data, a.out))


if __name__ == "__main__":
    main()
