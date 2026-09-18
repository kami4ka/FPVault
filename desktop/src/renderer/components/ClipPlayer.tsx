/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Playback without a video element.
 *
 * Chromium has no AVI demuxer, so a <video> pointed at one of these clips
 * simply fails. It does not need one: every frame the firmware writes is a
 * complete JPEG, so the player asks for frame N, decodes it with
 * createImageBitmap and blits it to a canvas on a timer.
 *
 * Three things fall out of that. Scrubbing is frame-accurate rather than
 * keyframe-accurate. Crash-cut clips play even before they are repaired.
 * And a dropout frame — a zero-length chunk the firmware writes so wall
 * clock stays honest — can be shown as a held image with a marker, instead
 * of being silently skipped.
 */
import { useCallback, useEffect, useRef, useState } from 'react'
import type { ClipMedia } from '@shared/types'
import { duration } from './format.js'
import type { Strings } from '../i18n/index.js'

/** Frames fetched ahead of the playhead, enough to absorb IPC latency. */
const PREFETCH = 24

export function ClipPlayer({
  clipId,
  name,
  s,
  onClose
}: {
  clipId: string
  name: string
  s: Strings
  onClose: () => void
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [media, setMedia] = useState<ClipMedia | null>(null)
  const [index, setIndex] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [dropout, setDropout] = useState(false)
  /* Bumped whenever a frame lands, so the draw effect re-runs: decoding is
   * asynchronous and the frame we want is usually not in the cache yet when
   * the index changes. */
  const [arrived, setArrived] = useState(0)

  /* Decoded frames, keyed by index. Bounded so a long clip cannot grow it
   * without limit while scrubbing. */
  const cache = useRef(new Map<number, ImageBitmap>())
  const inflight = useRef(new Set<number>())
  /* Indices known to be dropouts, so they are not re-fetched forever. */
  const empties = useRef(new Set<number>())
  const indexRef = useRef(0)
  indexRef.current = index

  useEffect(() => {
    let alive = true
    void window.fpvault.clip.media(clipId).then((m) => alive && setMedia(m))
    return () => {
      alive = false
      for (const bmp of cache.current.values()) bmp.close()
      cache.current.clear()
      inflight.current.clear()
    }
  }, [clipId])

  /** Fetch and decode one frame, unless it is already present or pending. */
  const fetchFrame = useCallback(
    async (i: number) => {
      if (cache.current.has(i) || inflight.current.has(i) || empties.current.has(i)) return
      inflight.current.add(i)
      try {
        const bytes = await window.fpvault.clip.frame(clipId, i)
        if (bytes.length === 0) {
          /* A zero-length chunk is a recorded dropout, not a missing frame:
           * the firmware writes one per lost frame so wall clock stays true.
           * Hold the previous image and say so. */
          empties.current.add(i)
          if (i === indexRef.current) setDropout(true)
          return
        }
        /* Copy into a plain ArrayBuffer: the IPC value may be backed by a
         * SharedArrayBuffer, which Blob will not accept. */
        const copy = new Uint8Array(bytes.length)
        copy.set(bytes)
        const bmp = await createImageBitmap(new Blob([copy.buffer], { type: 'image/jpeg' }))
        cache.current.set(i, bmp)
        if (i === indexRef.current) setArrived((n) => n + 1)

        /* Evict frames well behind the playhead. */
        if (cache.current.size > PREFETCH * 3) {
          for (const key of [...cache.current.keys()]) {
            if (key < indexRef.current - PREFETCH) {
              cache.current.get(key)?.close()
              cache.current.delete(key)
            }
          }
        }
      } catch {
        /* A frame that will not decode is shown as a held image; the
         * diagnostics panel is where broken frames are reported. */
      } finally {
        inflight.current.delete(i)
      }
    },
    [clipId]
  )

  /* Draw whatever we have for the current index, and read ahead. */
  useEffect(() => {
    if (!media) return
    const canvas = canvasRef.current
    if (!canvas) return

    const bmp = cache.current.get(index)
    if (bmp) {
      const ctx = canvas.getContext('2d')
      if (ctx) {
        canvas.width = bmp.width
        canvas.height = bmp.height
        ctx.drawImage(bmp, 0, 0)
      }
      setDropout(false)
    } else if (empties.current.has(index)) {
      setDropout(true)
    } else {
      void fetchFrame(index)
    }

    for (let i = index; i < Math.min(media.frames, index + PREFETCH); i++) void fetchFrame(i)
  }, [index, media, fetchFrame, arrived])

  /* Play at the clip's own rate, which is the board's capture rate. */
  useEffect(() => {
    if (!playing || !media) return
    const period = media.fps ? 1000 / media.fps : 33
    const timer = setInterval(() => {
      setIndex((i) => {
        if (i + 1 >= media.frames) {
          setPlaying(false)
          return i
        }
        return i + 1
      })
    }, period)
    return () => clearInterval(timer)
  }, [playing, media])

  const step = (n: number) => {
    setPlaying(false)
    setIndex((i) => Math.max(0, Math.min((media?.frames ?? 1) - 1, i + n)))
  }

  return (
    <div className="fixed inset-0 z-50 flex flex-col bg-black/85 p-6" onClick={onClose}>
      <div
        className="mx-auto flex max-h-full w-full max-w-3xl flex-col overflow-hidden rounded-[var(--radius-card)] bg-[var(--color-surface)]"
        onClick={(e) => e.stopPropagation()}
      >
        <header className="flex items-center gap-3 border-b border-[var(--color-line)] px-4 py-2">
          <span className="font-[family-name:var(--font-mono)] text-sm font-semibold">{name}</span>
          <span className="text-xs text-[var(--color-muted)]">
            {media ? `${media.width}x${media.height} · ${media.fps.toFixed(2)} fps` : ''}
          </span>
          <span className="flex-1" />
          <button
            type="button"
            onClick={onClose}
            className="rounded px-2 py-1 text-xs text-[var(--color-muted)] hover:text-[var(--color-ink)]"
          >
            {s.player.close}
          </button>
        </header>

        <div className="relative flex min-h-0 flex-1 items-center justify-center bg-black">
          <canvas ref={canvasRef} className="max-h-[60vh] max-w-full object-contain" />
          {dropout && (
            <span className="absolute inset-x-0 bottom-3 mx-auto w-fit rounded bg-[var(--color-warn)] px-2 py-1 text-xs font-semibold text-white">
              {s.player.dropout}
            </span>
          )}
        </div>

        <footer className="space-y-2 border-t border-[var(--color-line)] px-4 py-3">
          <input
            type="range"
            min={0}
            max={Math.max(0, (media?.frames ?? 1) - 1)}
            value={index}
            onChange={(e) => {
              setPlaying(false)
              setIndex(Number(e.target.value))
            }}
            className="w-full accent-[var(--color-brand)]"
            aria-label={s.player.scrub}
          />
          <div className="flex items-center gap-2 text-xs">
            <button
              type="button"
              onClick={() => setPlaying((v) => !v)}
              className="rounded-[var(--radius-card)] bg-[var(--color-brand)] px-3 py-1.5 font-semibold text-white"
            >
              {playing ? s.player.pause : s.player.play}
            </button>
            <button type="button" onClick={() => step(-1)} className="px-2 py-1">
              −1
            </button>
            <button type="button" onClick={() => step(1)} className="px-2 py-1">
              +1
            </button>
            <span className="flex-1" />
            <span className="font-[family-name:var(--font-mono)] text-[var(--color-muted)]">
              {s.player.frameOf(index + 1, media?.frames ?? 0)} ·{' '}
              {duration(media?.fps ? index / media.fps : 0)}
            </span>
          </div>
        </footer>
      </div>
    </div>
  )
}
