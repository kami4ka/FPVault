# Hardware errata (current board revision)

English | [Українська](HARDWARE-ERRATA.uk.md)

Observations from bring-up that firmware cannot fix; targets for the next
board spin.

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

## 3. Serial console path glitches

The in-line ESP32 USB-serial bridge reboots on every host port open and
sprays noise bytes into the console (this is why console commands require
the ':' prefix). A future board should route UART0 to its own USB-serial
directly.
