// Power Stroke: variable-width stroke expansion with the semantics of Inkscape's powerstroke LPE (lpe-powerstroke.cpp), so
// the live effect written by project.ts renders the same in Inkscape. The width is interpolated in PATH TIME t (segment
// index + fraction, per sub-path) — Inkscape's `offset_points` convention, not arc length — through a 2-D curve in
// (t, offset) space: Linear, CubicBezierJohan (horizontal handles, β = 0.2) or CentripetalCatmullRom. Open sub-paths get
// Inkscape's synthetic end points (t = 0 and t = n at the first / last knot's width, or 0 for the zero-width cap); closed
// ones wrap the neighbours around. The skeleton is split like stroke.ts (turn < 30°, no cusps, and ≤ ⅛ segment so the
// width is linear within a piece), each piece offset by a Hermite fit of the true variable offset, then joins
// (base.join), caps (zero / butt / square / round / peak) and the union do the rest.
import type { CubicNode, CubicSub, PowerStroke, Pt, Stroke } from '../model.ts';
import { union } from './boolean.ts';
import { type Cubic, EPS, MAX_TURN, MAX_DEPTH, isLine, turn, safe, tangent, join, cap, poly, orient, add, sub, neg, scale, dist, normal } from './stroke.ts';
import { curvatureAt } from './curvature.ts';

export const JOHAN_BETA = 0.2;
const MAX_DT = 1 / 8;

/** Knots in the order the expansion uses them: by t unless `sort` is off (Inkscape's sort_points). */
export const orderedKnots = (p: PowerStroke) => p.sort === false ? p.knots : p.knots.slice().sort((a, b) => a.t - b.t);

/** The (t, offset) control points Inkscape interpolates for a sub-path of n segments: knots as half-widths, plus the
 *  synthetic ends (open) or the wrapped neighbours (closed). */
function controlPoints(p: PowerStroke, n: number, closed: boolean): Pt[] {
  const pts = orderedKnots(p).map(k => ({ x: k.t, y: k.width / 2 }));
  if (!pts.length) return pts;
  const first = pts[0]!, last = pts[pts.length - 1]!;
  if (closed) return [{ x: last.x - n, y: last.y }, ...pts, { x: first.x + n, y: first.y }];
  return [{ x: 0, y: p.startCap === 'zero' ? 0 : first.y }, ...pts, { x: n, y: p.endCap === 'zero' ? 0 : last.y }];
}

/** Inkscape's "CentripetalCatmullRom" as it actually renders (measured on a straight skeleton, Inkscape 1.4): uniform
 *  Catmull-Rom, handles at p ± (next − prev) / 6, neighbours clamped at the ends. */
function catmull(p0: Pt, p1: Pt, p2: Pt, p3: Pt): Cubic {
  return [p1, add(p1, scale(sub(p2, p0), 1 / 6)), sub(p2, scale(sub(p3, p1), 1 / 6)), p2];
}

/** Point and unit tangent on a sub-path at path time t (segment index + fraction), clamped to the domain. */
export function pointAt(s: CubicSub, t: number): { p: Pt; tan: Pt } {
  const n = segCount(s), i = Math.min(n - 1, Math.max(0, Math.floor(t))), u = Math.min(1, Math.max(0, t - i));
  const a = s.nodes[i]!, b = s.nodes[(i + 1) % s.nodes.length]!, c: Cubic = [a, a.out ?? a, b.in ?? b, b], v = 1 - u;
  if (isLine(c)) return { p: { x: a.x + (b.x - a.x) * u, y: a.y + (b.y - a.y) * u }, tan: tangent(c, 0) }; // lines run linearly in t (SVG `L`, as Inkscape times them)
  const d = { x: 3 * (v * v * (c[1].x - c[0].x) + 2 * v * u * (c[2].x - c[1].x) + u * u * (c[3].x - c[2].x)), y: 3 * (v * v * (c[1].y - c[0].y) + 2 * v * u * (c[2].y - c[1].y) + u * u * (c[3].y - c[2].y)) };
  return { p: at(c, u), tan: dist(d, { x: 0, y: 0 }) > EPS ? scale(d, 1 / dist(d, { x: 0, y: 0 })) : tangent(c, u < 0.5 ? 0 : 1) };
}

/** The interpolated (t, offset) curve as cubic spans, x increasing span to span. */
export function widthSpans(p: PowerStroke, n: number, closed: boolean): Cubic[] {
  const pts = controlPoints(p, n, closed), out: Cubic[] = [];
  for (let i = 0; i + 1 < pts.length; i++) {
    const a = pts[i]!, b = pts[i + 1]!;
    if (p.interpolator === 'linear') out.push([a, a, b, b]);
    else if (p.interpolator === 'cubic') { const h = { x: JOHAN_BETA * (b.x - a.x), y: 0 }; out.push([a, add(a, h), sub(b, h), b]); }
    else out.push(catmull(pts[Math.max(0, i - 1)]!, a, b, pts[Math.min(pts.length - 1, i + 2)]!));
  }
  return out;
}

const at = (c: Cubic, u: number): Pt => { const v = 1 - u, a = v * v * v, b = 3 * v * v * u, d = 3 * v * u * u, e = u * u * u; return { x: a * c[0].x + b * c[1].x + d * c[2].x + e * c[3].x, y: a * c[0].y + b * c[1].y + d * c[2].y + e * c[3].y }; };

/** Half width (offset) at path time t. Inkscape draws skeleton(x(u)) ± y(u)·normal along the (t, offset) curve's own
 *  parameter u and fills nonzero, so where x(u) folds back (Catmull-Rom overshoot) every branch contributes: the largest
 *  |y| over all crossings x(u) = t. Degenerate (zero-extent) spans — a knot sitting exactly on a synthetic end — add
 *  nothing; the width is constant beyond the ends. */
export function offsetAt(spans: Cubic[], t: number): number {
  const sp = spans.filter(s => s[3].x > s[0].x + 1e-12);
  if (!sp.length) return Math.abs(spans[0]?.[0].y ?? 0);
  if (t <= sp[0]![0].x) return Math.abs(sp[0]![0].y);
  if (t >= sp[sp.length - 1]![3].x) return Math.abs(sp[sp.length - 1]![3].y);
  let best = -1;
  for (const s of sp) {
    if (isLine(s)) { if (t >= s[0].x && t <= s[3].x) best = Math.max(best, Math.abs(s[0].y + (s[3].y - s[0].y) * (t - s[0].x) / (s[3].x - s[0].x))); continue; }
    const N = 32;
    let prev = s[0].x;
    for (let i = 1; i <= N; i++) { // every crossing of x(u) = t in a 32-step scan, refined by bisection
      const u = i / N, x = at(s, u).x;
      if ((prev < t) !== (x < t) || x === t) {
        let lo = (i - 1) / N, hi = u;
        const rising = prev < x;
        for (let k = 0; k < 30; k++) { const m = (lo + hi) / 2; if ((at(s, m).x < t) === rising) lo = m; else hi = m; }
        best = Math.max(best, Math.abs(at(s, (lo + hi) / 2).y));
      }
      prev = x;
    }
  }
  return best >= 0 ? best : Math.abs(sp[sp.length - 1]![3].y);
}
/** Full width at path time t of a sub-path with n segments (UI markers, tests). */
export const widthAt = (p: PowerStroke, t: number, n: number, closed: boolean): number => 2 * offsetAt(widthSpans(p, n, closed), t);
/** Number of segments of a sub-path (the t domain is [0, n]). */
export const segCount = (s: CubicSub): number => s.closed ? s.nodes.length : Math.max(0, s.nodes.length - 1);

/** Path time `t` after a node is inserted at segment `i`, parameter `u`: that segment becomes two, so times inside it are
 *  rescaled onto [i, i+2) and everything past it shifts by one. A knot retimed this way stays on the same skeleton point
 *  (client/ops.ts calls it on every insertion). */
export function retime(t: number, i: number, u: number): number {
  if (t <= i) return t;
  if (t >= i + 1) return t + 1;
  if (!(u > 0 && u < 1)) return t + 1; // split at a node: the whole segment became the second half
  const f = t - i;
  return f <= u ? i + f / u : i + 1 + (f - u) / (1 - u);
}

type Piece = { c: Cubic; t0: number; t1: number };

function segments(s: CubicSub): Piece[] {
  const ns = s.nodes, out: Piece[] = [];
  for (let i = 0; i < segCount(s); i++) {
    const a = ns[i]!, b = ns[(i + 1) % ns.length]!, c: Cubic = [a, a.out ?? a, b.in ?? b, b];
    if (!c.every(p => dist(p, a) < EPS)) out.push({ c, t0: i, t1: i + 1 });
  }
  return out;
}

/** de Casteljau at parameter u, with the t range split proportionally; lines stay in line form (p1 = p0, p2 = p3) so their
 *  offsets come out handle-free — paper's union mishandles cubic edges whose handles merely lie on the chord. */
function cutAt(p: Piece, u: number): [Piece, Piece] {
  const [p0, p1, p2, p3] = p.c, l = (a: Pt, b: Pt): Pt => ({ x: a.x + (b.x - a.x) * u, y: a.y + (b.y - a.y) * u }), tm = p.t0 + (p.t1 - p.t0) * u;
  if (isLine(p.c)) { const m = l(p0, p3); return [{ c: [p0, p0, m, m], t0: p.t0, t1: tm }, { c: [m, m, p3, p3], t0: tm, t1: p.t1 }]; }
  const p01 = l(p0, p1), p12 = l(p1, p2), p23 = l(p2, p3), a = l(p01, p12), b = l(p12, p23), m = l(a, b);
  return [{ c: [p0, p01, a, m], t0: p.t0, t1: tm }, { c: [m, b, p23, p3], t0: tm, t1: p.t1 }];
}
/** Split a segment at the width knots inside it, then until each piece turns < 30°, cannot cusp at its widest, and spans ≤ ⅛ segment. */
function pieces(seg: Piece, cuts: number[], w: (t: number) => number): Piece[] {
  let parts = [seg];
  for (const t of cuts) { const i = parts.findIndex(p => p.t0 < t && t < p.t1); if (i >= 0) parts.splice(i, 1, ...cutAt(parts[i]!, (t - parts[i]!.t0) / (parts[i]!.t1 - parts[i]!.t0))); }
  const out: Piece[] = [];
  const rec = (p: Piece, depth: number) => {
    const r = Math.max(w(p.t0), w(p.t1));
    if (depth >= MAX_DEPTH || (turn(p.c) < MAX_TURN && safe(p.c, r) && p.t1 - p.t0 <= MAX_DT)) { out.push(p); return; }
    const [a, b] = cutAt(p, 0.5);
    rec(a, depth + 1); rec(b, depth + 1);
  };
  parts.forEach(p => rec(p, 0));
  return out;
}

/** Offset at a distance varying from d0 (start) to d1 (end), with the distance's slope in the piece's own parameter
 *  (dd0, dd1): the cubic Hermite that matches the true variable offset O = C + d·N at both ends in position AND
 *  derivative, O' = |C'|(1 − dκ)·T + d'·N — the offset tangent is NOT the skeleton tangent where the width changes, and
 *  pretending it is put an S-bend in every piece (a sawtooth comb that got worse the finer the pieces were cut).
 *  Neighbouring pieces share C, C', κ, d and d', so they meet tangentially. Lines stay handle-free lines. */
function offsetVar(c: Cubic, d0: number, d1: number, dd0: number, dd1: number): Cubic {
  const [p0, p1, p2, p3] = c;
  if (isLine(c)) { const n = normal(sub(p3, p0)), a = add(p0, scale(n, d0)), b = add(p3, scale(n, d1)); return [a, a, b, b]; }
  const end = (u: 0 | 1, d: number, dd: number): [Pt, Pt] => {
    const at = u ? p3 : p0, t = tangent(c, u), n = normal(t);
    const speed = 3 * dist(at, u ? p2 : p1) || 3 * dist(p0, p3); // |C'(u)|, the degenerate handle falling back to the chord
    const v = add(scale(t, speed * (1 - d * curvatureAt(p0, p1, p2, p3, u))), scale(n, dd));
    return [add(at, scale(n, d)), scale(v, 1 / 3)];
  };
  const [a0, v0] = end(0, d0, dd0), [a3, v1] = end(1, d1, dd1);
  return [a0, add(a0, v0), sub(a3, v1), a3];
}

/** One closed contour per segment: left offsets forward, right offsets backward (as stroke.ts body()). */
function body(ps: Piece[], w: (t: number) => number): CubicSub {
  // d and its slope at both ends, one-sided so a knot's genuine kink in w stays a kink instead of being smeared
  const ds = ps.map(p => { const h = (p.t1 - p.t0) * 1e-4, d0 = w(p.t0), d1 = w(p.t1);
    return { d0, d1, s0: (w(p.t0 + h) - d0) / 1e-4, s1: (d1 - w(p.t1 - h)) / 1e-4 }; });
  const left = ps.map((p, i) => offsetVar(p.c, ds[i]!.d0, ds[i]!.d1, ds[i]!.s0, ds[i]!.s1));
  const right = ps.map((p, i) => offsetVar(p.c, -ds[i]!.d0, -ds[i]!.d1, -ds[i]!.s0, -ds[i]!.s1));
  const last = ps.length - 1, nodes: CubicNode[] = [];
  const h = (at: Pt, handle: Pt | undefined, key: 'in' | 'out') => handle && dist(at, handle) > EPS ? { [key]: handle } : {}; // no zero-length handles
  left.forEach((q, i) => nodes.push({ x: q[0].x, y: q[0].y, ...(i > 0 && h(q[0], left[i - 1]![2], 'in')), ...h(q[0], q[1], 'out') }));
  nodes.push({ x: left[last]![3].x, y: left[last]![3].y, ...h(left[last]![3], left[last]![2], 'in') });
  for (let i = last; i >= 0; i--) nodes.push({ x: right[i]![3].x, y: right[i]![3].y, ...(i < last && h(right[i]![3], right[i + 1]![1], 'in')), ...h(right[i]![3], right[i]![2], 'out') });
  nodes.push({ x: right[0]![0].x, y: right[0]![0].y, ...h(right[0]![0], right[0]![1], 'in') });
  return { closed: true, nodes };
}

/** Inkscape's caps at an end point with outward unit tangent t and offset r: zero / butt add nothing, peak is a triangle of height r. */
export function powerCap(p: Pt, t: Pt, r: number, kind: PowerStroke['startCap']): CubicSub[] {
  if (!(r > EPS) || kind === 'zero' || kind === 'butt') return [];
  if (kind === 'peak') { const n = scale(normal(t), r); return [poly([add(p, n), add(p, scale(t, r)), sub(p, n)])]; }
  return cap(p, t, r, kind);
}

/** Filled outline of the power stroke of `subs`: overlap-free, PostScript direction. `base` supplies join / miterLimit. */
export function expandPowerStroke(subs: CubicSub[], power: PowerStroke, base: Stroke): CubicSub[] {
  const out: CubicSub[] = [];
  for (const s of subs) {
    const segs = segments(s), n = segCount(s);
    if (!segs.length || !power.knots.length) continue;
    const spans = widthSpans(power, n, s.closed), w = (t: number) => offsetAt(spans, t);
    const cuts = [...new Set(spans.flatMap(sp => [sp[0].x, sp[3].x]))].sort((a, b) => a - b);
    for (const seg of segs) out.push(body(pieces(seg, cuts.filter(t => seg.t0 < t && t < seg.t1), w), w));
    for (let i = 0; i < (s.closed ? segs.length : segs.length - 1); i++) {
      const a = segs[i]!, b = segs[(i + 1) % segs.length]!, r = w(a.t1);
      if (r > EPS) out.push(...join(a.c[3], tangent(a.c, 1), tangent(b.c, 0), r, base));
    }
    if (!s.closed) {
      const first = segs[0]!, last = segs[segs.length - 1]!;
      out.push(...powerCap(first.c[0], neg(tangent(first.c, 0)), w(0), power.startCap), ...powerCap(last.c[3], tangent(last.c, 1), w(n), power.endCap));
    }
  }
  return union([out.map(orient).filter(c => c.nodes.length)]);
}
