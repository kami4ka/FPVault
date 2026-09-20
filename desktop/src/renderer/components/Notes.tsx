/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Release notes, rendered.
 *
 * These are written as GitHub Markdown and were being shown as their own
 * source, which for v0.9.4 meant a table of which button to press arrived as
 * a row of pipes. Tables and fenced code are exactly the parts worth reading,
 * so GFM is enabled.
 *
 * Two things this must not do. It must not render raw HTML: the text comes
 * from a network response, and react-markdown ignores embedded HTML unless a
 * plugin is added, so the safe behaviour is simply the default one kept.
 * And it must not let a link navigate the window - the renderer has nowhere
 * to come back from - so every anchor is turned into a button that hands the
 * URL to the main process, which opens it in the real browser and refuses
 * anything that is not https.
 */
import ReactMarkdown, { type Components } from 'react-markdown'
import remarkGfm from 'remark-gfm'

const muted = 'text-[var(--color-muted)]'

const components: Components = {
  h1: ({ children }) => <p className="mt-3 text-xs font-semibold first:mt-0">{children}</p>,
  h2: ({ children }) => <p className="mt-3 text-xs font-semibold first:mt-0">{children}</p>,
  h3: ({ children }) => <p className="mt-3 text-xs font-semibold first:mt-0">{children}</p>,
  p: ({ children }) => (
    <p className={`mt-1.5 text-[11px] leading-relaxed first:mt-0 ${muted}`}>{children}</p>
  ),
  ul: ({ children }) => (
    <ul className={`mt-1.5 list-disc space-y-1 pl-4 text-[11px] leading-relaxed ${muted}`}>
      {children}
    </ul>
  ),
  ol: ({ children }) => (
    <ol className={`mt-1.5 list-decimal space-y-1 pl-4 text-[11px] leading-relaxed ${muted}`}>
      {children}
    </ol>
  ),
  strong: ({ children }) => (
    <strong className="font-semibold text-[var(--color-ink)]">{children}</strong>
  ),
  code: ({ children }) => (
    <code className="rounded bg-[var(--color-line)] px-1 py-0.5 font-[family-name:var(--font-mono)] text-[10px]">
      {children}
    </code>
  ),
  /* A fenced block arrives as <pre><code>; style the pre and let the code
   * inside it fall back to plain text rather than the inline pill above. */
  pre: ({ children }) => (
    <pre className="mt-1.5 overflow-x-auto rounded-[var(--radius-card)] border border-[var(--color-line)] bg-[var(--color-surface)] p-2 font-[family-name:var(--font-mono)] text-[10px] leading-relaxed [&_code]:bg-transparent [&_code]:p-0 [&_code]:text-[10px]">
      {children}
    </pre>
  ),
  blockquote: ({ children }) => (
    <blockquote className={`mt-1.5 border-l-2 border-[var(--color-line)] pl-2 text-[11px] ${muted}`}>
      {children}
    </blockquote>
  ),
  hr: () => <hr className="mt-3 border-[var(--color-line)]" />,
  /* Tables are the reason GFM is on, and the one thing that may exceed the
   * panel's width, so they scroll on their own rather than stretching it. */
  table: ({ children }) => (
    <div className="mt-2 overflow-x-auto">
      <table className="w-full border-collapse text-[11px]">{children}</table>
    </div>
  ),
  th: ({ children }) => (
    <th className="border border-[var(--color-line)] px-2 py-1 text-left font-semibold">
      {children}
    </th>
  ),
  td: ({ children }) => (
    <td className={`border border-[var(--color-line)] px-2 py-1 align-top ${muted}`}>{children}</td>
  ),
  a: ({ href, children }) => (
    <button
      type="button"
      onClick={() => href && void window.fpvault.app.openUrl(href)}
      className="text-[var(--color-brand)] underline-offset-2 hover:underline"
    >
      {children}
    </button>
  ),
  img: () => null
}

export function Notes({ markdown }: { markdown: string }) {
  return (
    <ReactMarkdown remarkPlugins={[remarkGfm]} components={components}>
      {markdown}
    </ReactMarkdown>
  )
}
