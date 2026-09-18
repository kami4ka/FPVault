/* SPDX-License-Identifier: GPL-3.0-or-later */
export const en = {
  nav: {
    device: 'Device',
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
  common: {
    rescan: 'Rescan',
    unknown: 'unknown'
  }
}
