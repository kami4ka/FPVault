/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The app ships in English and Ukrainian, and a string that exists in one
 * table and not the other renders as nothing at all — an empty caption in
 * the language its reader does not have. TypeScript catches a missing key
 * but not a mistyped nested one, so the shapes are compared here.
 */
import { describe, expect, it } from 'vitest'
import { en } from '../src/renderer/i18n/en.js'
import { uk } from '../src/renderer/i18n/uk.js'

type Node = Record<string, unknown>

/** Every leaf path, with what kind of value sits there. */
function shape(obj: Node, prefix = ''): string[] {
  const out: string[] = []
  for (const [key, value] of Object.entries(obj)) {
    const path = prefix ? `${prefix}.${key}` : key
    if (typeof value === 'object' && value !== null) out.push(...shape(value as Node, path))
    else out.push(`${path}:${typeof value}`)
  }
  return out.sort()
}

describe('string tables', () => {
  it('has the same keys in both languages', () => {
    expect(shape(uk as Node)).toEqual(shape(en as Node))
  })

  it('leaves nothing untranslated', () => {
    const same: string[] = []
    const walk = (a: Node, b: Node, prefix = '') => {
      for (const [key, value] of Object.entries(a)) {
        const path = prefix ? `${prefix}.${key}` : key
        const other = (b as Record<string, unknown>)[key]
        if (typeof value === 'object' && value !== null) walk(value as Node, other as Node, path)
        else if (typeof value === 'string' && value === other && /[a-z]{4}/i.test(value))
          same.push(path)
      }
    }
    walk(en as Node, uk as Node)
    /* Proper nouns and language names are meant to match; prose is not. */
    expect(same).toEqual([])
  })
})
