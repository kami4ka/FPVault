/* SPDX-License-Identifier: GPL-3.0-or-later */

const MB = 1024 * 1024
const GB = 1024 * MB

export function bytes(n: number): string {
  if (n >= GB) return `${(n / GB).toFixed(1)} GB`
  if (n >= MB) return `${(n / MB).toFixed(0)} MB`
  return `${(n / 1024).toFixed(0)} KB`
}

export function duration(sec: number): string {
  const s = Math.round(sec)
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const r = s % 60
  const pad = (n: number) => String(n).padStart(2, '0')
  return h ? `${h}:${pad(m)}:${pad(r)}` : `${m}:${pad(r)}`
}

export function clockTime(iso: string | null): string {
  if (!iso) return '—'
  const d = new Date(iso)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

/** For a datetime-local input, which wants local time with no zone suffix. */
export function toLocalInput(d: Date): string {
  const pad = (n: number) => String(n).padStart(2, '0')
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}` +
    `T${pad(d.getHours())}:${pad(d.getMinutes())}`
  )
}
