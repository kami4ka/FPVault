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
    join: 'Join',
    export: 'Export MP4',
    exportHint: 'Encode the session to H.264 for sharing, about a tenth the size',
    joinHint: 'Merge every clip in this session into one lossless file',
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
  firmwareScreen: {
    releases: 'Releases',
    check: 'Check now',
    newest: 'newest',
    prerelease: 'pre-release',
    none: 'No firmware releases found',
    installed: 'Installed',
    install: (tag: string) => `Install ${tag}`,
    cannotFlash: 'Connect the board in card-reader mode to install firmware',
    whyUnknown:
      'The board does not report its firmware version over USB. It prints it to the serial console at boot, which this app cannot read. Installing the same release twice is harmless, so there is no need to be sure.',
    safetyNote:
      'Nothing is written to the board until the whole image has arrived and the board has checked it. A cable pulled mid-transfer changes nothing; the second or so of writing is the only moment that matters, and the app says when that is happening.',
    capableDfu: {
      title: 'This board can be updated over USB',
      body: 'It exposes the DFU interface, so it runs v0.9.2 or newer. The update takes a few seconds and the board restarts itself.'
    },
    capableLegacy: {
      title: 'This board predates the USB update interface',
      body: 'It runs v0.9.1 or earlier, which has no DFU interface at all. One recovery flash over FEL brings it onto the normal update path for good.'
    },
    capableFel: {
      title: 'This board is in recovery mode',
      body: 'The boot ROM is waiting. Firmware can be written from here even if the flash is blank or damaged.'
    },
    capableNone: {
      title: 'No board connected',
      body: 'Plug the board in to install firmware. Releases can still be browsed without one.'
    }
  },
  player: {
    close: 'Close',
    play: 'Play',
    pause: 'Pause',
    scrub: 'Scrub through the clip',
    dropout: 'dropped frame — the camera signal was lost here',
    frameOf: (i: number, n: number) => `frame ${i} of ${n}`
  },
  jobPhase: {
    queued: 'queued',
    running: 'working',
    done: 'done',
    failed: 'failed',
    cancelled: 'cancelled'
  },
  common: {
    rescan: 'Rescan',
    cancel: 'Cancel',
    unknown: 'unknown'
  }
}
