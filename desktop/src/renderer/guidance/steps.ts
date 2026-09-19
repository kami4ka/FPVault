/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The guidance sequences.
 *
 * What makes these better than a video is that they advance on what the app
 * can actually see. A step carries a predicate over the live device state,
 * so when the board really does land in FEL the animation moves on by
 * itself instead of asking the user to judge whether it worked.
 */
import type { DeviceState } from '@shared/types'
import type { PartId } from './BoardArt.js'
import type { Strings } from '../i18n/index.js'

export type SequenceId = 'notDetected' | 'enterFel' | 'afterUpdate'

export interface GuideStep {
  id: string
  /** Picked out of the strings table so both languages stay in step. */
  caption: (s: Strings) => string
  sub?: (s: Strings) => string
  highlight: PartId[]
  /** Show a finger pressing SW2. */
  press?: boolean
  /** Auto-advance after this long when no predicate is given. */
  ms: number
  /** When true of the live device state, jump straight on. */
  waitFor?: (d: DeviceState) => boolean
  /** Restart the sequence from here instead of advancing. */
  restart?: boolean
}

const g = (k: keyof Strings['guide']) => (s: Strings) => s.guide[k]

/**
 * Board not detected. Two of these five steps carry facts a user cannot
 * guess and no generic "plug it in" illustration conveys: the board decides
 * its mode in the first 2.5 s of power (src/main.c), and the bulk capacitor
 * at the SD socket holds the card's rail up for a while, so a quick replug
 * may not actually reset anything (docs/HARDWARE-ERRATA.md).
 */
const notDetected: GuideStep[] = [
  {
    id: 'plug',
    caption: g('plugTitle'),
    sub: g('plugSub'),
    highlight: ['usbc'],
    ms: 2600,
    waitFor: (d) => d.kind !== 'absent'
  },
  {
    id: 'onlyPower',
    caption: g('onlyPowerTitle'),
    sub: g('onlyPowerSub'),
    highlight: ['power'],
    ms: 2600,
    waitFor: (d) => d.kind !== 'absent'
  },
  {
    id: 'window',
    caption: g('windowTitle'),
    sub: g('windowSub'),
    highlight: ['usbc', 'soc'],
    ms: 3200,
    waitFor: (d) => d.kind !== 'absent'
  },
  {
    id: 'replug',
    caption: g('replugTitle'),
    sub: g('replugSub'),
    highlight: ['usbc'],
    ms: 3600,
    waitFor: (d) => d.kind !== 'absent'
  },
  {
    id: 'cable',
    caption: g('cableTitle'),
    sub: g('cableSub'),
    highlight: ['usbc'],
    ms: 2800,
    waitFor: (d) => d.kind !== 'absent',
    restart: true
  }
]

/**
 * Entering FEL. The part prose always gets wrong is that SW2 has to be held
 * *before and during* the plug-in, not pressed afterwards — so the press is
 * shown as a sustained hold that overlaps the connection.
 */
const enterFel: GuideStep[] = [
  {
    id: 'unplug',
    caption: g('felUnplugTitle'),
    sub: g('felUnplugSub'),
    highlight: ['usbc'],
    ms: 3000,
    waitFor: (d) => d.kind === 'absent'
  },
  {
    id: 'hold',
    caption: g('felHoldTitle'),
    sub: g('felHoldSub'),
    highlight: ['sw2'],
    press: true,
    ms: 2600
  },
  {
    id: 'plugHolding',
    caption: g('felPlugTitle'),
    sub: g('felPlugSub'),
    highlight: ['sw2', 'usbc'],
    press: true,
    ms: 6000,
    waitFor: (d) => d.kind === 'fel' || d.kind === 'reader' || d.kind === 'legacy'
  },
  {
    id: 'release',
    caption: g('felReleaseTitle'),
    sub: g('felReleaseSub'),
    highlight: ['sw2'],
    ms: 2600,
    waitFor: (d) => d.kind === 'fel'
  },
  {
    id: 'missed',
    caption: g('felMissedTitle'),
    sub: g('felMissedSub'),
    highlight: ['sw2'],
    ms: 3200,
    restart: true
  }
]

/** After a firmware update the board reboots itself. */
const afterUpdate: GuideStep[] = [
  {
    id: 'rebooting',
    caption: g('afterRebootTitle'),
    sub: g('afterRebootSub'),
    highlight: ['soc'],
    ms: 4000,
    waitFor: (d) => d.kind === 'absent'
  },
  {
    id: 'comesBack',
    caption: g('afterBackTitle'),
    sub: g('afterBackSub'),
    highlight: ['usbc', 'led'],
    ms: 6000,
    waitFor: (d) => d.kind === 'reader'
  },
  {
    id: 'replugIfNot',
    caption: g('afterReplugTitle'),
    sub: g('afterReplugSub'),
    highlight: ['usbc'],
    ms: 5000,
    waitFor: (d) => d.kind === 'reader',
    restart: true
  }
]

export const SEQUENCES: Record<SequenceId, GuideStep[]> = {
  notDetected,
  enterFel,
  afterUpdate
}
