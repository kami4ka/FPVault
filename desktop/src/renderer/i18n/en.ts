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
    exports: 'Exports',
    exportsHint: 'joined and converted files',
    play: 'Play',
    open: 'Open',
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
    recovery: 'Recovery mode',
    recoverWith: (tag: string) => `Recover with ${tag}`,
    check: 'Check now',
    newest: 'newest',
    prerelease: 'pre-release',
    none: 'No firmware releases found',
    installed: 'Installed',
    install: (tag: string) => `Install ${tag}`,
    cannotFlash: 'Connect the board in card-reader mode to install firmware',
    whyUnknown:
      'This board runs firmware older than v0.9.3, which is when the version was first reported over USB. Before that it existed only in the boot message on the serial console. Installing the same release twice is harmless, so there is no need to be sure.',
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
  settingsScreen: {
    library: {
      title: 'Library',
      body: 'Where imported clips, joined files and exports are kept.',
      change: 'Change folder',
      show: 'Show',
      note: 'Choosing another folder starts a fresh index there. Clips already imported stay where they are, so point this back to find them again.'
    },
    exports: {
      title: 'MP4 export',
      body: 'Used by Export MP4. The lossless join has no settings of its own: it copies frames without re-encoding them.',
      deinterlace: 'Deinterlace',
      deinterlaceHint:
        'The camera feeds the board interlaced analogue video, so this is usually right. Turn it off for a progressive camera, which it would only soften.',
      quality: 'Quality',
      high: 'High',
      highHint: 'Slowest to encode and the largest file. Closest to what was recorded.',
      balanced: 'Balanced',
      balancedHint: 'About a tenth the size of the original, at roughly 45x real time.',
      small: 'Small',
      smallHint: 'Smallest file, visibly softer on grass, trees and other fine detail.'
    },
    gap: {
      title: 'Gap between clips',
      body: 'A clip that fills up rolls straight into the next one with no gap at all, and that case is detected exactly. A clip that ended early ended because the camera signal was lost, and how long that lasted is not recorded anywhere.',
      label: 'Assume',
      unit: 'seconds',
      note: 'Six is the floor: the board needs five seconds of lost signal to close a clip and one second of stable signal to open the next. Raise it if your sessions have longer breaks. It only shifts the times shown after such a gap, never the video.'
    },
    tools: {
      title: 'Bundled tools',
      body: 'These ship inside the app; nothing needs installing. A tool missing from this build is why the feature beside it is unavailable.',
      present: 'bundled',
      missing: 'not in this build',
      needs: {
        ffmpeg: 'MP4 export',
        'sunxi-fel': 'FEL recovery'
      },
      fetchHint: 'Build the app with npm run fetch-binaries and npm run build-fel to include it.',
      pureNote:
        'Import, repair, join and playback use neither. They are plain TypeScript, so a missing tool costs one feature rather than the app.'
    },
    licences: {
      title: 'Open source licences',
      body: 'FPVault Desktop is free software, and so is everything it bundles. The source link beside each entry is the offer of corresponding source that the GPL requires.',
      source: 'Source',
      what: {
        fpvault: 'This app and the board firmware it updates.',
        ffmpeg: 'Encodes the H.264/MP4 export. Bundled as a static binary.',
        x264: 'The H.264 encoder inside that FFmpeg build.',
        'sunxi-tools': 'sunxi-fel writes flash through the boot ROM during recovery.',
        runtime: 'The application runtime and build tooling.'
      }
    },
    language: {
      title: 'Language',
      body: 'Also switchable from the bottom of the sidebar.'
    },
    about: {
      title: 'About',
      version: 'Version'
    }
  },
  guide: {
    progress: 'Step',
    plugTitle: 'Plug the board into this computer',
    plugSub: 'USB-C, any port. Use a cable you know carries data, not a charging cable.',
    onlyPowerTitle: 'USB has to be its only power',
    onlyPowerSub: 'Disconnect the flight battery and anything on the 5V_IN pad. With another supply present the board never looks for a computer.',
    windowTitle: 'The board decides in its first two and a half seconds',
    windowSub: 'At power-on it looks for a computer, and whatever it finds it stays until the next replug. It cannot be switched over afterwards.',
    replugTitle: 'Already plugged in? Unplug it, count to five, plug it back in',
    replugSub: 'There is a bulk capacitor at the card socket, so a quick replug may not actually power the card down. Give it five seconds.',
    cableTitle: 'Still nothing? Try a different cable',
    cableSub: 'Plenty of USB-C cables carry power only. A charge-only cable looks identical and never enumerates.',

    foundTitle: 'There it is',
    foundSub: 'The board is connected and the app can see it.',
    afterDoneTitle: 'Back, and running the new firmware',
    afterDoneSub: 'The card is available again.',

    felUnplugTitle: 'Unplug the board',
    felUnplugSub: 'Recovery starts from no power at all, so take the cable out first.',
    felHoldTitle: 'Press and hold SW2',
    felHoldSub: 'The small button beside the USB connector, top right. SW1 on the left edge is reset — not that one.',
    felPlugTitle: 'Keep holding SW2 and plug the USB back in',
    felPlugSub: 'This is the part that catches people out: the button has to be down before power arrives and stay down as it does.',
    felReleaseTitle: 'Recovery mode — you can let go of SW2',
    felReleaseSub: 'The boot ROM is waiting and will accept a firmware write, even if the flash is blank or damaged. SW2 only blinds the ROM while it looks for flash; the flash itself is untouched. Pick a release below.',
    felMissedTitle: 'That was a normal boot',
    felMissedSub: 'SW2 was probably released too early, or was not down when power arrived. Let us try again.',

    afterRebootTitle: 'The board is restarting into the new firmware',
    afterRebootSub: 'It wrote its flash, read it back and compared before rebooting itself.',
    afterBackTitle: 'It should come back as a card reader in about five seconds',
    afterBackSub: 'Watch the LED: a fast blink means it has enumerated and handed the card over.',
    afterReplugTitle: 'It did not come back — unplug, wait five seconds, plug in again',
    afterReplugSub: 'If it still does not appear, recovery over FEL will reflash it from scratch.'
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
    dismiss: 'Dismiss',
    unknown: 'unknown'
  }
}
