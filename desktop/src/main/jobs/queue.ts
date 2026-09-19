/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Job queue for anything that touches a lot of bytes.
 *
 * Concurrency is one for card work, on purpose. The board's mass-storage
 * path does its SD reads inside the USB interrupt, serialised with the bus
 * transfer (docs/ARCHITECTURE.md), so parallel reads make a card slower, not
 * faster. Import is therefore strictly sequential; the user sees one clip
 * moving at full speed instead of four crawling.
 */
import { EventEmitter } from 'node:events'
import { randomUUID } from 'node:crypto'

export type JobKind = 'import' | 'repair' | 'join' | 'export' | 'firmware'
export type JobPhase = 'queued' | 'running' | 'done' | 'failed' | 'cancelled'

export interface JobState {
  id: string
  kind: JobKind
  /** What the user sees: usually the clip's name on the card. */
  label: string
  phase: JobPhase
  /** 0..1, or null while a job has no measurable progress yet. */
  progress: number | null
  detail: string
  error?: string
}

export interface JobContext {
  /** Report progress; `detail` is shown verbatim under the bar. */
  report(progress: number | null, detail: string): void
  /** Throws if the job has been cancelled, to unwind a long loop. */
  throwIfCancelled(): void
}

class Cancelled extends Error {
  constructor() {
    super('cancelled')
  }
}

export class JobQueue extends EventEmitter {
  private jobs = new Map<string, JobState>()
  private cancelled = new Set<string>()
  private chain: Promise<void> = Promise.resolve()

  list(): JobState[] {
    return [...this.jobs.values()]
  }

  cancel(id: string): void {
    const job = this.jobs.get(id)
    if (!job) return
    this.cancelled.add(id)
    if (job.phase === 'queued') this.finish(id, 'cancelled')
  }

  /** Queue a unit of work. Resolves when it has run, not when it is queued. */
  add<T>(kind: JobKind, label: string, run: (ctx: JobContext) => Promise<T>): string {
    const id = randomUUID()
    const job: JobState = { id, kind, label, phase: 'queued', progress: null, detail: '' }
    this.jobs.set(id, job)
    this.emitChange(job)

    this.chain = this.chain.then(async () => {
      if (this.cancelled.has(id)) return
      this.update(id, { phase: 'running', progress: 0 })

      const ctx: JobContext = {
        report: (progress, detail) => this.update(id, { progress, detail }),
        throwIfCancelled: () => {
          if (this.cancelled.has(id)) throw new Cancelled()
        }
      }

      try {
        await run(ctx)
        this.finish(id, 'done')
      } catch (err) {
        if (err instanceof Cancelled) this.finish(id, 'cancelled')
        else this.finish(id, 'failed', err instanceof Error ? err.message : String(err))
      }
    })

    return id
  }

  private update(id: string, patch: Partial<JobState>) {
    const job = this.jobs.get(id)
    if (!job) return
    Object.assign(job, patch)
    this.emitChange(job)
  }

  private finish(id: string, phase: JobPhase, error?: string) {
    this.update(id, { phase, error, progress: phase === 'done' ? 1 : null })
  }

  private emitChange(job: JobState) {
    this.emit('change', { ...job })
  }
}
