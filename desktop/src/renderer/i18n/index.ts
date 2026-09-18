/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The repo ships every document in English and Ukrainian; the app matches.
 * Strings live here from the first commit so nothing has to be retrofitted
 * later — the guidance captions and error text especially, since those are
 * the ones read under stress.
 */
import { en } from './en.js'
import { uk } from './uk.js'

export type Strings = typeof en
export type Lang = 'en' | 'uk'

const TABLES: Record<Lang, Strings> = { en, uk }

export function detectLang(): Lang {
  const stored = localStorage.getItem('fpvault.lang')
  if (stored === 'en' || stored === 'uk') return stored
  return navigator.language.toLowerCase().startsWith('uk') ? 'uk' : 'en'
}

export function setLang(lang: Lang): void {
  localStorage.setItem('fpvault.lang', lang)
}

export function strings(lang: Lang): Strings {
  return TABLES[lang]
}
