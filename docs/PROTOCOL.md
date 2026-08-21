# Control interfaces

English | [Українська](PROTOCOL.uk.md)

## RunCam Device Protocol v1.0 (device side)

The only record-control protocol Betaflight, INAV and ArduPilot all speak.
UART1 (PA2 TX / PA3 RX), 115200 8N1, non-inverted, 3.3 V.

Frames FC → device (`[0xCC][cmd][params][crc8]`, no length byte; CRC8
DVB-S2 poly 0xD5 over every byte including the header):

| Frame | Meaning | Our response |
|---|---|---|
| `CC 00 crc` | GET_DEVICE_INFO | `CC 01 C5 00 crc` (proto 1, features 0x00C5) |
| `CC 01 01 E7` | power button | toggle recording, **no reply** |
| `CC 01 03 98` | start recording | start (idempotent), no reply |
| `CC 01 04 CC` | stop recording | stop (idempotent), no reply |
| `CC 01 00/02 …` | wifi btn / change mode | parsed, ignored |
| `CC 02/03/04 …` | 5-key OSD emulation | parsed, ignored (feature not advertised) |

Features 0x00C5 = SIMULATE_POWER_BUTTON | CHANGE_MODE | START_RECORDING |
STOP_RECORDING. Bits 6/7 make ArduPilot use explicit start/stop; Betaflight
and INAV only ever send the power-button toggle. Replying to CAMERA_CONTROL
would desync Betaflight's parser — the device stays silent by design. The
FC probes for ~7 s after boot (ArduPilot default); repeated GET_DEVICE_INFO
is normal.

FC configuration:
- **Betaflight/INAV**: Ports → Peripherals → "RunCam Device" on the wired
  UART; Modes → `CAMERA POWER` on an AUX switch (put it on the ARM range
  for record-on-arm).
- **ArduPilot**: `SERIALx_PROTOCOL=26`, `CAM1_TYPE=8`, `CAM1_RC_TYPE=2`,
  `RCx_OPTION=78` (record start/stop).

## GPIO record pin (FC-less fallback)

One input pin, three usable signal styles (planned, M6):
- level: high = record, low = stop;
- pulse > 100 ms = toggle (matches Betaflight PINIO pulse mode);
- RC PWM 1000–2000 µs: > ~1700 µs = record, with ~60 ms hysteresis.

## Default behavior

Auto-record on valid video: recording starts after ~1 s of stable V_LOCK
and stops on sustained (≥5 s) signal loss or an explicit stop; short
dropouts stay inside the running clip as timing-preserving empty frames.

## LED (PE3)

solid = recording · slow blink = armed, waiting for signal/command ·
double blink = no/full card · fast blink = error.
