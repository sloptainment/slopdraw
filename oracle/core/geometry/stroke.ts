// Stroke expansion: skeleton sub-paths → the filled outline of stroking them. No topology is stitched by hand: every
// segment body, join wedge and cap is emitted as its own positively-oriented closed contour and `union` merges them
// (and fixes direction). Curves are split until each piece turns < 30° and its offset cannot cusp, then offset with
// Tiller–Hanson (offset the control polygon legs, intersect).
import type { CubicNode, CubicSub, Pt, Stroke } from '../model.ts';
import { union } from './boolean.ts';
import { expandPowerStroke } from './powerstroke.ts';

const KAPPA = 0.5522847498307936;
export const MAX_TURN = Math.PI / 6;
export const MAX_DEPTH = 8; // ponytail: ≤ 256 pieces per cubic; a curve still tighter than w/2 after that gets a twisted body (slight notch), nobody strokes those in a font
export const EPS = 1e-9;

export type Cubic = [Pt, Pt, Pt, Pt];

/** Filled outline of stroking `subs` with `stroke`, overlap-free, PostScript direction (outer CCW in y-up).
 *  A Power Stroke (`stroke.power`) goes through powerstroke.ts. */
export function expandStroke(subs: CubicSub[], stroke: Stroke): CubicSub[] {
  if (stroke.power) return expandPowerStroke(subs, stroke.power, stroke);
  // ponytail: no dashes; Tiller–Hanson offsets are ~0.1 unit off on 30° pieces at font widths, fine for export
  const r = stroke.width / 2;
  if (!(r > 0)) return [];
  const out: CubicSub[] = [];
  for (const sub of subs) {
    const segs = segments(sub);
    const n = segs.length;
    if (!n) continue;
    for (const c of segs) out.push(body(c, r));
    for (let i = 0; i < (sub.closed ? n : n - 1); i++) {
      const a = segs[i], b = segs[(i + 1) % n];
      out.push(...join(a[3], tangent(a, 1), tangent(b, 0), r, stroke));
    }
    if (!sub.closed) {
      out.push(...cap(segs[0][0], neg(tangent(segs[0], 0)), r, stroke.cap));
      out.push(...cap(segs[n - 1][3], tangent(segs[n - 1], 1), r, stroke.cap));
    }
  }
  return union([out.map(orient).filter(s => s.nodes.length)]);
}

/** Non-degenerate cubic segments of a sub-path (lines as cubics with p1=p0, p2=p3). */
function segments(sub: CubicSub): Cubic[] {
  const ns = sub.nodes, count = sub.closed ? ns.length : ns.length - 1;
  const segs: Cubic[] = [];
  if (ns.length < 2) return segs;
  for (let i = 0; i < count; i++) {
    const a = ns[i], b = ns[(i + 1) % ns.length];
    const c: Cubic = [a, a.out ?? a, b.in ?? b, b];
    if (c.every(p => dist(p, a) < EPS)) continue;
    segs.push(c);
  }
  return segs;
}

export function isLine(c: Cubic): boolean { return dist(c[0], c[1]) < EPS && dist(c[2], c[3]) < EPS; }

/** One closed contour: left offset forward, end line, right offset backward, start line. */
function body(c: Cubic, r: number): CubicSub {
  if (isLine(c)) {
    const n = scale(normal(sub(c[3], c[0])), r);
    return poly([add(c[0], n), add(c[3], n), sub(c[3], n), sub(c[0], n)]);
  }
  const pieces = split(c, r, 0);
  const left = pieces.map(p => offset(p, r)), right = pieces.map(p => offset(p, -r));
  const last = pieces.length - 1, nodes: CubicNode[] = [];
  left.forEach((q, i) => nodes.push({ x: q[0].x, y: q[0].y, ...(i > 0 && { in: left[i - 1][2] }), out: q[1] }));
  nodes.push({ x: left[last][3].x, y: left[last][3].y, in: left[last][2] });
  for (let i = last; i >= 0; i--) nodes.push({ x: right[i][3].x, y: right[i][3].y, ...(i < last && { in: right[i + 1][1] }), out: right[i][2] });
  nodes.push({ x: right[0][0].x, y: right[0][0].y, in: right[0][1] });
  return { closed: true, nodes };
}

function split(c: Cubic, r: number, depth: number): Cubic[] {
  if (depth >= MAX_DEPTH || (turn(c) < MAX_TURN && safe(c, r))) return [c];
  const [a, b] = halves(c);
  return [...split(a, r, depth + 1), ...split(b, r, depth + 1)];
}

export function halves(c: Cubic): [Cubic, Cubic] {
  const [p0, p1, p2, p3] = c;
  const p01 = mid(p0, p1), p12 = mid(p1, p2), p23 = mid(p2, p3), p012 = mid(p01, p12), p123 = mid(p12, p23), m = mid(p012, p123);
  return [[p0, p01, p012, m], [m, p123, p23, p3]];
}

/** Total turning of the control polygon. */
export function turn(c: Cubic): number {
  const legs = [sub(c[1], c[0]), sub(c[2], c[1]), sub(c[3], c[2])].filter(v => len(v) > EPS);
  let t = 0;
  for (let i = 1; i < legs.length; i++) t += Math.abs(Math.atan2(cross(legs[i - 1], legs[i]), dot(legs[i - 1], legs[i])));
  return t;
}

/** Offset radius r is below the radius of curvature at five samples (else the inner offset cusps). */
export function safe(c: Cubic, r: number): boolean {
  const [p0, p1, p2, p3] = c;
  for (const t of [0, 0.25, 0.5, 0.75, 1]) {
    const u = 1 - t;
    const dx = 3 * (u * u * (p1.x - p0.x) + 2 * u * t * (p2.x - p1.x) + t * t * (p3.x - p2.x));
    const dy = 3 * (u * u * (p1.y - p0.y) + 2 * u * t * (p2.y - p1.y) + t * t * (p3.y - p2.y));
    const ddx = 6 * (u * (p2.x - 2 * p1.x + p0.x) + t * (p3.x - 2 * p2.x + p1.x));
    const ddy = 6 * (u * (p2.y - 2 * p1.y + p0.y) + t * (p3.y - 2 * p2.y + p1.y));
    const v = Math.hypot(dx, dy);
    if (v < EPS) continue;
    if (Math.abs(dx * ddy - dy * ddx) / (v * v * v) * r > 1) return false;
  }
  return true;
}

/** Tiller–Hanson: offset each control-polygon leg by d along its left normal and intersect neighbouring legs. */
export function offset(c: Cubic, d: number): Cubic {
  const [p0, p1, p2, p3] = c;
  let d0 = sub(p1, p0), d1 = sub(p2, p1), d2 = sub(p3, p2);
  if (len(d1) < EPS) d1 = sub(p3, p0);
  if (len(d0) < EPS) d0 = d1;
  if (len(d2) < EPS) d2 = d1;
  const n0 = scale(normal(d0), d), n1 = scale(normal(d1), d), n2 = scale(normal(d2), d);
  const a0 = add(p0, n0), a1 = add(p1, n1), a2 = add(p2, n2);
  return [a0, intersect(a0, d0, a1, d1) ?? a1, intersect(a1, d1, a2, d2) ?? add(p2, n1), add(p3, n2)];
}

export function intersect(a: Pt, u: Pt, b: Pt, v: Pt): Pt | undefined {
  const den = cross(u, v);
  if (Math.abs(den) < EPS * len(u) * len(v)) return undefined;
  return add(a, scale(u, cross(sub(b, a), v) / den));
}

/** Unit tangent at t = 0 or 1, skipping coincident control points. */
export function tangent(c: Cubic, t: 0 | 1): Pt {
  const order = t === 0 ? [c[1], c[2], c[3]].map(p => sub(p, c[0])) : [c[2], c[1], c[0]].map(p => sub(c[3], p));
  return unit(order.find(v => len(v) > EPS) ?? { x: 1, y: 0 });
}

export function join(p: Pt, tIn: Pt, tOut: Pt, r: number, stroke: Stroke): CubicSub[] {
  const cr = cross(tIn, tOut), dt = dot(tIn, tOut);
  if (Math.abs(cr) < 1e-9 && dt > 0) return []; // smooth
  if (stroke.join === 'round') return [circle(p, r)];
  const s = cr > 0 ? -1 : 1; // left turn (y-up) → outer side is the right
  const nIn = scale(normal(tIn), s * r), nOut = scale(normal(tOut), s * r);
  const a = add(p, nIn), b = add(p, nOut);
  const theta = Math.atan2(Math.abs(cr), dt); // turning angle; miter length / width = 1 / cos(θ/2)
  if (stroke.join === 'miter' && 1 / Math.cos(theta / 2) <= stroke.miterLimit) {
    return [poly([p, a, add(p, scale(add(nIn, nOut), 1 / (1 + dt))), b])];
  }
  return [poly([p, a, b])];
}

/** Cap at endpoint p with outward unit tangent t. */
export function cap(p: Pt, t: Pt, r: number, kind: Stroke['cap']): CubicSub[] {
  if (kind === 'butt') return [];
  if (kind === 'round') return [circle(p, r)];
  const n = scale(normal(t), r), f = scale(t, r);
  return [poly([add(p, n), add(add(p, n), f), add(sub(p, n), f), sub(p, n)])];
}

function circle(c: Pt, r: number): CubicSub {
  const k = r * KAPPA;
  return { closed: true, nodes: [
    { x: c.x + r, y: c.y, in: { x: c.x + r, y: c.y - k }, out: { x: c.x + r, y: c.y + k } },
    { x: c.x, y: c.y + r, in: { x: c.x + k, y: c.y + r }, out: { x: c.x - k, y: c.y + r } },
    { x: c.x - r, y: c.y, in: { x: c.x - r, y: c.y + k }, out: { x: c.x - r, y: c.y - k } },
    { x: c.x, y: c.y - r, in: { x: c.x - k, y: c.y - r }, out: { x: c.x + k, y: c.y - r } },
  ] };
}

export function poly(pts: Pt[]): CubicSub { return { closed: true, nodes: pts.map(p => ({ x: p.x, y: p.y })) }; }

/** Positive signed area (so overlapping pieces add up under nonzero winding); empty if degenerate. */
export function orient(s: CubicSub): CubicSub {
  let area = 0;
  const ns = s.nodes;
  for (let i = 0; i < ns.length; i++) {
    const a = ns[i], b = ns[(i + 1) % ns.length], p1 = a.out ?? a, p2 = b.in ?? b;
    let prev: Pt = a;
    for (let k = 1; k <= 8; k++) {
      const t = k / 8, u = 1 - t;
      const q = { x: u * u * u * a.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * b.x, y: u * u * u * a.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * b.y };
      area += prev.x * q.y - q.x * prev.y;
      prev = q;
    }
  }
  if (Math.abs(area) < EPS) return { closed: true, nodes: [] };
  if (area > 0) return s;
  return { closed: true, nodes: ns.slice().reverse().map(n => ({ x: n.x, y: n.y, ...(n.out && { in: n.out }), ...(n.in && { out: n.in }) })) };
}

export const add = (a: Pt, b: Pt): Pt => ({ x: a.x + b.x, y: a.y + b.y });
export const sub = (a: Pt, b: Pt): Pt => ({ x: a.x - b.x, y: a.y - b.y });
export const neg = (a: Pt): Pt => ({ x: -a.x, y: -a.y });
export const mid = (a: Pt, b: Pt): Pt => ({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 });
export const scale = (a: Pt, k: number): Pt => ({ x: a.x * k, y: a.y * k });
export const len = (a: Pt) => Math.hypot(a.x, a.y);
export const dist = (a: Pt, b: Pt) => len(sub(a, b));
export const dot = (a: Pt, b: Pt) => a.x * b.x + a.y * b.y;
export const cross = (a: Pt, b: Pt) => a.x * b.y - a.y * b.x;
export const unit = (a: Pt): Pt => scale(a, 1 / len(a));
export const normal = (a: Pt): Pt => { const u = unit(a); return { x: -u.y, y: u.x }; }; // left normal (y-up)
