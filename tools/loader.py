#!/usr/bin/env python3
"""
loader.py PORT BIN [--addr 0x80000000] [--no-go]

Load a bare-metal .bin into an F1C200s via U-Boot `loady` (YMODEM), then `go`.
Opens the serial port with DTR/RTS held LOW so it does NOT reset an in-line
ESP32 bridge (the CP2102 auto-reset would otherwise reboot the bridge and
corrupt the transfer). Pure-python YMODEM-1K sender; no external deps.
"""
import sys, time, os, argparse, serial

SOH, STX, EOT, ACK, NAK, CAN, CRC = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def open_noreset(port, baud=115200):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 1.0
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.8)          # let an in-line ESP32 finish any boot
    return s


def _wait(s, wanted, timeout):
    end = time.time() + timeout
    while time.time() < end:
        b = s.read(1)
        if b and b[0] in wanted:
            return b[0]
    return None


def _block(s, ctrl, seq, data):
    c = crc16(data)
    s.write(bytes([ctrl, seq & 0xFF, (255 - (seq & 0xFF)) & 0xFF]) + data
            + bytes([c >> 8, c & 0xFF]))
    s.flush()


def _send_block_retry(s, ctrl, seq, data, tries=8):
    for _ in range(tries):
        _block(s, ctrl, seq, data)
        r = _wait(s, (ACK, NAK, CAN), 3.0)
        if r == ACK:
            return True
        if r == CAN:
            return False
    return False


def ymodem_send(s, path):
    name = os.path.basename(path).encode()
    data = open(path, "rb").read()
    # receiver kicks off with 'C'
    if _wait(s, (CRC,), 8.0) is None:
        raise RuntimeError("no 'C' from receiver (loady not ready)")
    # block 0: filename + size, padded to 128
    hdr = name + b"\x00" + str(len(data)).encode() + b"\x00"
    hdr = hdr.ljust(128, b"\x00")
    if not _send_block_retry(s, SOH, 0, hdr):
        raise RuntimeError("header block rejected")
    if _wait(s, (CRC,), 5.0) is None:
        raise RuntimeError("no 'C' after header")
    # data in 1024-byte STX blocks
    seq, off = 1, 0
    while off < len(data):
        chunk = data[off:off + 1024].ljust(1024, b"\x1a")
        if not _send_block_retry(s, STX, seq, chunk):
            raise RuntimeError(f"data block {seq} rejected")
        seq = (seq + 1) & 0xFF
        off += 1024
    # EOT (receiver NAKs first, ACKs second)
    s.write(bytes([EOT])); s.flush()
    if _wait(s, (ACK, NAK), 3.0) == NAK:
        s.write(bytes([EOT])); s.flush(); _wait(s, (ACK,), 3.0)
    # end-of-batch: empty header
    if _wait(s, (CRC,), 3.0) is not None:
        _send_block_retry(s, SOH, 0, b"\x00" * 128)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("bin")
    ap.add_argument("--addr", default="0x80000000")
    ap.add_argument("--no-go", action="store_true")
    a = ap.parse_args()

    s = open_noreset(a.port)
    # clean U-Boot line, confirm prompt
    s.reset_input_buffer()
    s.write(b"\x03\r\n"); time.sleep(0.4)
    s.reset_input_buffer()
    s.write(b"\r\n"); time.sleep(0.4)
    resp = s.read(400)
    if b"=>" not in resp:
        print("WARN: no '=>' prompt seen; response:", resp[-120:])
    s.write(f"loady {a.addr}\r\n".encode()); time.sleep(0.5)
    n = ymodem_send(s, a.bin)
    time.sleep(0.5)
    print(s.read(400).decode(errors="replace").strip()[-160:])
    print(f"[loader] sent {n} bytes to {a.addr}")
    if not a.no_go:
        s.reset_input_buffer()
        s.write(f"go {a.addr}\r\n".encode()); time.sleep(2.5)
        print("[app]", s.read(600).decode(errors="replace").strip()[-300:])
    s.close()


if __name__ == "__main__":
    main()
