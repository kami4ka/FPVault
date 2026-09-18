/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A clip's poster frame.
 *
 * No thumbnail cache on disk and no ffmpeg: a poster is just one '00dc'
 * payload, which is already a JPEG the browser can display. Reading one
 * measures at well under a millisecond, so generating them on demand is
 * cheaper than storing them.
 */
import { useEffect, useState } from 'react'

/** Frame 0 often catches the camera's gain still settling. */
function posterIndex(total: number): number {
  return total > 60 ? 45 : Math.max(0, Math.floor(total / 3))
}

export function ClipThumb({
  clipId,
  frames,
  className
}: {
  clipId: string
  frames: number
  className?: string
}) {
  const [url, setUrl] = useState<string | null>(null)

  useEffect(() => {
    let alive = true
    let objectUrl: string | null = null

    void window.fpvault.clip.frame(clipId, posterIndex(frames)).then((bytes) => {
      if (!alive || bytes.length === 0) return
      const copy = new Uint8Array(bytes.length)
      copy.set(bytes)
      objectUrl = URL.createObjectURL(new Blob([copy.buffer], { type: 'image/jpeg' }))
      setUrl(objectUrl)
    })

    return () => {
      alive = false
      if (objectUrl) URL.revokeObjectURL(objectUrl)
    }
  }, [clipId, frames])

  return (
    <span
      className={`block shrink-0 overflow-hidden rounded bg-[var(--color-line)] ${className ?? ''}`}
      aria-hidden
    >
      {url && <img src={url} alt="" className="h-full w-full object-cover" />}
    </span>
  )
}
