/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The FPVault board, drawn from docs/img/board-v1.jpg.
 *
 * Hand-authored SVG in the same style as docs/img/pipeline.svg: flat fills,
 * 10px corner radius, no gradients and no shadows, with dashed strokes
 * reserved for things that are annotations rather than parts. Every part
 * that guidance needs to point at carries a stable id so a step can
 * highlight it by name.
 *
 * Layout matches the real board: USB-C on the top edge left of centre, SW2
 * top right, SW1 on the left edge two thirds down, the F1C200s bottom
 * centre, the SPI-NOR beside the USB connector, and four mounting holes.
 */

export type PartId =
  | 'usbc'
  | 'sw1'
  | 'sw2'
  | 'soc'
  | 'nor'
  | 'led'
  | 'card'
  | 'cvbsIn'
  | 'cvbsOut'
  | 'power'

export function BoardArt({
  highlight,
  hand,
  tone
}: {
  highlight: PartId[]
  hand: 'press' | null
  tone?: 'success'
}) {
  const on = (id: PartId) => (highlight.includes(id) ? 'hi' : '')

  return (
    <svg
      viewBox="0 0 400 400"
      className={`board-art h-full w-full${tone === 'success' ? ' ok' : ''}`}
      role="img"
      aria-hidden
    >
      {/* PCB */}
      <rect x="30" y="30" width="340" height="340" rx="18" className="pcb" />

      {/* mounting holes */}
      {[
        [75, 75],
        [325, 75],
        [75, 325],
        [325, 325]
      ].map(([cx, cy]) => (
        <circle key={`${cx}-${cy}`} cx={cx} cy={cy} r="15" className="hole" />
      ))}

      {/* USB-C, straddling the top edge */}
      <g id="usbc" className={`part ${on('usbc')}`}>
        <rect x="128" y="18" width="76" height="52" rx="8" className="metal" />
        <rect x="140" y="34" width="52" height="14" rx="7" className="slot" />
        <text x="166" y="86" className="ref">USB1</text>
      </g>

      {/* SW2 — the FEL button, top right */}
      <g id="sw2" className={`part ${on('sw2')}`}>
        <rect x="246" y="52" width="46" height="40" rx="7" className="switch" />
        <circle cx="269" cy="72" r="13" className="plunger" />
        <text x="269" y="108" className="ref">SW2</text>
      </g>

      {/* SW1 — reset, left edge lower third */}
      <g id="sw1" className={`part ${on('sw1')}`}>
        <rect x="74" y="248" width="44" height="40" rx="7" className="switch" />
        <circle cx="96" cy="268" r="12" className="plunger" />
        <text x="96" y="304" className="ref">SW1</text>
      </g>

      {/* SPI-NOR, beside the connector */}
      <g id="nor" className={`part ${on('nor')}`}>
        <rect x="186" y="58" width="48" height="44" rx="5" className="chip" />
        <text x="210" y="116" className="ref">U3</text>
      </g>

      {/* F1C200s, bottom centre */}
      <g id="soc" className={`part ${on('soc')}`}>
        <rect x="152" y="252" width="104" height="104" rx="6" className="chip" />
        <text x="204" y="310" className="socLabel">F1C200s</text>
      </g>

      {/* crystal */}
      <rect x="186" y="170" width="34" height="26" rx="4" className="passive" />

      {/* status LED */}
      <g id="led" className={`part ${on('led')}`}>
        <circle cx="268" cy="196" r="9" className="led" />
        <text x="268" y="222" className="ref">LED1</text>
      </g>

      {/* microSD socket, drawn on the underside edge as a hint */}
      <g id="card" className={`part ${on('card')}`}>
        <rect x="118" y="360" width="86" height="26" rx="5" className="socket" />
        <text x="161" y="379" className="socketLabel">microSD</text>
      </g>

      {/* power pads, left edge */}
      <g id="power" className={`part ${on('power')}`}>
        <circle cx="48" cy="150" r="7" className="pad" />
        <text x="62" y="154" className="padLabel" textAnchor="start">5V_IN</text>
      </g>

      {/* video pads, right edge */}
      <g id="cvbsOut" className={`part ${on('cvbsOut')}`}>
        <circle cx="352" cy="150" r="7" className="pad" />
        <text x="338" y="154" className="padLabel" textAnchor="end">CVBS_OUT</text>
      </g>
      <g id="cvbsIn" className={`part ${on('cvbsIn')}`}>
        <circle cx="352" cy="210" r="7" className="pad" />
        <text x="338" y="214" className="padLabel" textAnchor="end">CVBS_IN</text>
      </g>

      {/* a finger pressing SW2, shown only while a step says to hold it */}
      {hand === 'press' && (
        <g className="hand">
          <circle cx="269" cy="72" r="24" className="press-ring" />
          <circle cx="269" cy="72" r="34" className="press-ring delay" />
        </g>
      )}
    </svg>
  )
}
