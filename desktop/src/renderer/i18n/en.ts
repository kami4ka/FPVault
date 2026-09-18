/* SPDX-License-Identifier: GPL-3.0-or-later */
export const en = {
  nav: {
    device: 'Device',
    import: 'Import',
    library: 'Library',
    firmware: 'Firmware',
    settings: 'Settings'
  },
  state: {
    absent: {
      title: 'No board detected',
      body: 'Plug the FPVault board into this computer with a data cable. The board picks its mode in the first two and a half seconds of power, so plug it in rather than waking it up.',
      action: 'Show me how'
    },
    reader: {
      title: 'Board connected',
      body: 'The card is mounted and ready to import.',
      action: 'Import clips'
    },
    legacy: {
      title: 'Board connected, firmware too old to update over USB',
      body: 'This board runs v0.9.1 or earlier, which has no update interface. One recovery flash brings it onto the normal update path for good.',
      action: 'Recover with FEL'
    },
    fel: {
      title: 'Board in recovery mode',
      body: 'The boot ROM is waiting. The app can write U-Boot and firmware from here.',
      action: 'Flash firmware'
    },
    cardOnly: {
      title: 'Card reader connected',
      body: 'A FPVault card is mounted without the board. Importing works; firmware updates need the board itself.',
      action: 'Import clips'
    },
    noCardYet: 'Waiting for the card to mount',
    noCard: 'No card in the board'
  },
  card: {
    sessions: (n: number) => (n === 1 ? '1 session' : `${n} sessions`),
    free: 'free',
    of: 'of',
    firmware: 'Firmware',
    firmwareUnknown: 'unknown',
    mount: 'Mounted at'
  },
  tasks: {
    import: { title: 'Import', body: 'Copy clips off the card and repair them' },
    repair: { title: 'Repair', body: 'Rebuild the index on power-cut clips' },
    glue: { title: 'Join', body: 'Merge a session into one file' },
    timestamps: { title: 'Timestamps', body: 'Give clips their real date and time' },
    update: { title: 'Update firmware', body: 'Fetch the latest release and flash it' }
  },
  health: {
    clean: 'Closed cleanly',
    crashCut: 'Cut by a power loss, repaired on import',
    damaged: 'Structurally damaged'
  },
  importScreen: {
    scanning: 'Reading the card',
    empty: 'No clips on this card',
    transfers: 'Transfers',
    start: (n: number) => (n === 1 ? 'Import 1 session' : `Import ${n} sessions`),
    reclaimNote: (n: number) =>
      `${n} ${n === 1 ? 'clip is' : 'clips are'} a 200 MB preallocation holding much less. Only the real part is read.`
  },
  libraryScreen: {
    empty: 'Nothing imported yet',
    reveal: 'Show',
    playHint: 'Play this clip',
    setStart: 'Set start time',
    changeStart: 'Change start time',
    recovered: (saved: string) => `${saved} of preallocation left behind`,
    tornDropped: 'torn final frame dropped',
    repaired: (n: number) => `${n} repaired`,
    gapUncertain: 'The gap before this clip is a minimum, not a measurement',
    noRtcNote:
      'The board has no clock, so every clip on the card is stamped 2020-01-01. Times here come from the session start you set plus each clip\'s real duration. A ~ marks a gap that is a minimum rather than a measurement.'
  }
,
  player: {
    close: 'Close',
    play: 'Play',
    pause: 'Pause',
    scrub: 'Scrub through the clip',
    dropout: 'dropped frame — the camera signal was lost here',
    frameOf: (i: number, n: number) => `frame ${i} of ${n}`
  },
  common: {
    rescan: 'Rescan',
    unknown: 'unknown'
  }
}
