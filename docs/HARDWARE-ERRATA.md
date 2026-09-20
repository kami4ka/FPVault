# Hardware errata

English | [Українська](HARDWARE-ERRATA.uk.md)

Observations from bring-up that firmware cannot fix. Sections 1–3 are the
generic F1C200s development board; the last section is the FPVault board.

## 1. Power rails have thin brownout margins

Two symptoms, one cause:

- **SD hot-insert resets the board.** The card's inrush on insertion dips
  the 3V3 rail below the SoC's reset threshold. Workaround: insert the
  card before power-up (harmless now that boot is autonomous - the board
  simply reboots into recording). Fix: bulk capacitance (>=100 uF) close
  to the SD socket plus a series ferrite/soft-start.

- **Touching the EA3059 PMIC resets the board.** The regulator's feedback
  and enable nodes are high-impedance and exposed; fingertip capacitance
  perturbs the sensed voltage enough to glitch a rail. Fix for the spin:
  short FB divider traces away from the board edge and hand-contact areas,
  ground pour/keep-out around the FB nodes, optionally conformal coating.
  Bench workaround: do not handle the PMIC corner while powered.

Both failure modes are absorbed by the firmware's crash-safe design
(preallocated clips, per-second header refresh, autonomous reboot into
recording), but a flight-worthy board should not brown out this easily -
vibration and connector strain are the airborne equivalents of a finger.

Soak data (2026-08-21, revised by a timestamped repro): ~35 minutes on a
USB power bank produced 5 spontaneous resets, a wall charger produced 2
more, a computer's USB port produced none. The timestamped capture shows
the true sequence: the recording is healthy, then 11 s of silence, then
U-Boot - and U-Boot itself logs "Card did not respond to voltage select!
-110". So the write-current spike dips the rail at the CARD, the card's
controller latches up, the firmware hangs on the dead card until the
watchdog reboots the SoC. Sometimes the card un-latches during the
reboot (recording resumes in a new session), sometimes it stays latched
until power is physically removed (see #2). Every event cost exactly one
video frame plus the re-boot gap - the crash-safe design absorbed all of
them - but the fix is the board's: bulk capacitance (>=100 uF) at the SD
socket. A bench retrofit of 220 uF across SD VDD-GND is the confirming
experiment. (Firmware debt noted separately: a card dying mid-write
should degrade to NO_CARD without tripping the watchdog.)

The retrofit's verdict, same day: with 220 uF soldered at the SD socket,
a 16-segment / 80-minute recording soak on a power bank ran clean
(previous best on the same supply: ~12 minutes), and 5.1 GB of sustained
USB mass-storage reads served without a hiccup - the same read load that
had crashed the firmware twice at 15 MB and 130 MB before the capacitor.
All three observed failure classes (recording resets, card latch-up,
USB-read crashes) trace to the one root cause and stop with the one fix.

## 2. A SoC reset does not power-cycle the SD card

The watchdog/brownout reset line restarts the SoC but SD VDD never drops.
A card that a mid-write reset leaves in a wedged state stays wedged
through any number of reboots — `sdcard_detect` fails (`mount FAILED
fr=3`) forever, and only physically removing power revives it. Observed
once at the end of the power-bank soak: the board rebooted into a card it
could never re-initialize. Fix for the next spin: a high-side switch
(P-FET) on SD VDD under GPIO control, so firmware can power-cycle a
wedged card in flight. Until then, a wedged card means lost recording
time until the next battery swap - the already-recorded clips stay safe.
Note: the bench bulk-capacitor retrofit makes it WORSE to recover by
unplugging - the cap holds the card's rail up for minutes, so only
ejecting the card from the socket actually power-cycles it.

## 3. Serial console path glitches

The in-line ESP32 USB-serial bridge reboots on every host port open and
sprays noise bytes into the console (this is why console commands require
the ':' prefix). A future board should route UART0 to its own USB-serial
directly.

## FPVault board v1 (analog bypass) — bring-up findings

- **TVD input termination goes on the source side of the coupling cap.**
  As drawn, the 75 Ω shunt (R31) sat between C33 and the TVD pin, tying
  the pin to 0 V DC. The decoder biases its input through a weak internal
  clamp that cannot hold against 75 Ω to ground, so the video swung
  around ground, the sync tips fell below the ADC's range, and the TVD
  reported no-signal on a textbook 1 Vpp waveform at the pin. Moving R31
  to the R33/C33 junction (same divider, same 1 Vpp, pin side floating)
  locked instantly: status 0x0E, 30 fps, first recording on the board.
- **UART0's receive pad has no pull-up, and the board will not boot without
  one.** PE0 is the receive pad on this board, and the schematic gives it a
  1 kΩ series resistor (R18) to an open pad and nothing else. Port E leaves
  reset with every pin disabled and no pull, so until code configures it
  nothing defines that node. Measured over FEL with PE0 driven as an input:
  floating with no pull it reads a steady 0, the internal pull-up holds it
  at 1, and an internal pull-down also reads 0 — so the pad is genuinely
  undriven, not shorted. A receive line held at 0 is an unending break
  condition.

  Upstream U-Boot does pull up its console receive pin, but it pulls up
  **PE1**, because upstream takes PE1 for receive. On this board PE1 is the
  transmit pin, so that pull-up lands on a driven output and the real
  receive pin is left floating. The board therefore only booted while
  something external held the line high — in practice a serial adapter,
  whose idle transmit pin was doing the job. It did not have to be a working
  serial bridge: a Flipper unplugged from any computer was enough, which is
  what made this look like a logic problem rather than a DC one.

  Fixed in `uboot/patches/0001-*`, which pulls up PE0 in `gpio_init()`. That
  runs from `board_init_f` in the SPL, before the console exists, so the pin
  is defined from the earliest moment any code runs. Proven by a control
  build: same tree, same config, same environment, that one instruction
  removed, and the board stops booting.

  Fix for the next spin: a 10 kΩ pull-up from the UART0 receive net to 3V3,
  so the pad is defined by the board rather than by firmware.

  Two firmware changes went in alongside and are worth keeping, but neither
  was the cause and neither fixed it: `sys_uart_init` now pulls PE0 up
  rather than asking for no pull, and `console_poll` drops bytes that arrive
  with a framing, parity, break or overrun flag. Both only matter once the
  firmware is running, and the failure was two stages earlier.
- **Reflow the QFN before doubting anything else.** The first FPVault v1 board
  spent two days "dead" - no FEL, rails and crystal fine, every IC warm,
  the SoC swapped twice - with the RESET pin floating at the QFN side
  while the net measured 3.3 V at the button. See BRINGUP troubleshooting.
- Confirmed good as designed: USB-C straight to the SoC with 5.1k CC
  pull-downs enumerates in FEL and as the FPVault card reader on the
  first try once the chip runs; the THS7374 + TS5A3153 bypass passes the
  camera to CVBS_OUT with PE4 at its pull-down default and no firmware
  involvement; the amp's 2 Vpp output through the 75/75 divider gives
  the decoder 1 Vpp.
