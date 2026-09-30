// Curvature toolset: comb, harmonize, tunnify, add extrema / inflections. Pure cubic math over CubicSub.
// Mirrors FontForge 20230101: splineutil.c SplineCurvature + Spline2DFindPointsOfInflection, splineutil2.c
// SplinePointHarmonize (harmonizeNode) + SplineAddInflections. harmonizeHandles is the Curvatura/"Grey Harmony" variant
// (node fixed, handle lengths rescaled); tunnify is the classic Tunni-point balance, not FontForge's area-preserving SplineBalance.
import type { CubicNode, CubicSub, Pt } from '../model.ts';
import { cubicAt } from './bounds.ts';

type Cubic = [Pt, Pt, Pt, Pt];
const EPS = 1e-9;

const sub = (a: Pt, b: Pt): Pt => ({ x: a.x - b.x, y: a.y - b.y });
const add = (a: Pt, b: Pt): Pt => ({ x: a.x + b.x, y: a.y + b.y });
const mul = (a: Pt, k: number): Pt => ({ x: a.x * k, y: a.y * k });
const lerp = (a: Pt, b: Pt, t: number): Pt => ({ x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t });
const cross = (a: Pt, b: Pt) => a.x * b.y - a.y * b.x;
const len = (a: Pt) => Math.hypot(a.x, a.y);
const near = (a: Pt, b: Pt, eps: number) => Math.hypot(a.x - b.x, a.y - b.y) <= eps;

/** First and second derivatives of a cubic at t. */
function derivs(p0: Pt, p1: Pt, p2: Pt, p3: Pt, t: number): [Pt, Pt] {
  const u = 1 - t;
  const d = (k: 'x' | 'y') => 3 * (u * u * (p1[k] - p0[k]) + 2 * u * t * (p2[k] - p1[k]) + t * t * (p3[k] - p2[k]));
  const dd = (k: 'x' | 'y') => 6 * (u * (p2[k] - 2 * p1[k] + p0[k]) + t * (p3[k] - 2 * p2[k] + p1[k]));
  return [{ x: d('x'), y: d('y') }, { x: dd('x'), y: dd('y') }];
}

/** Signed curvature κ = (x'y'' − y'x'') / (x'² + y'²)^{3/2}; positive = turning left. 0 where the derivative vanishes. */
export function curvatureAt(p0: Pt, p1: Pt, p2: Pt, p3: Pt, t: number): number {
  const [d1, d2] = derivs(p0, p1, p2, p3, t);
  const speed = len(d1);
  if (speed < EPS) return 0;
  return cross(d1, d2) / (speed * speed * speed);
}

/** Parameters t in (0,1) where the curvature changes sign (x'y'' − y'x'' = 0, a quadratic in t). Straight lines: none. */
export function inflections(p0: Pt, p1: Pt, p2: Pt, p3: Pt): number[] {
  // power basis: P(t) = a t³ + b t² + c t + d
  const ax = -p0.x + 3 * p1.x - 3 * p2.x + p3.x, ay = -p0.y + 3 * p1.y - 3 * p2.y + p3.y;
  const bx = 3 * p0.x - 6 * p1.x + 3 * p2.x, by = 3 * p0.y - 6 * p1.y + 3 * p2.y;
  const cx = 3 * (p1.x - p0.x), cy = 3 * (p1.y - p0.y);
  const A = 3 * (ay * bx - ax * by), B = 3 * (cx * ay - cy * ax), C = cx * by - cy * bx;
  return quadRoots(A, B, C).filter(t => t > EPS && t < 1 - EPS).sort((a, b) => a - b);
}

function quadRoots(a: number, b: number, c: number): number[] {
  const scale = Math.max(Math.abs(a), Math.abs(b), Math.abs(c));
  if (scale < EPS) return [];
  if (Math.abs(a) < EPS * scale) return Math.abs(b) < EPS * scale ? [] : [-c / b];
  const disc = b * b - 4 * a * c;
  if (disc < 0) return [];
  const s = Math.sqrt(disc);
  return [(-b + s) / (2 * a), (-b - s) / (2 * a)];
}

/** Cubic control points of segment i of a sub, or null when it does not exist or is straight. */
function segment(s: CubicSub, i: number): Cubic | null {
  const a = s.nodes[i], b = s.nodes[i + 1] ?? (s.closed && s.nodes.length > 1 ? s.nodes[0] : undefined);
  if (!a || !b || (!a.out && !b.in)) return null;
  return [a, a.out ?? a, b.in ?? b, b];
}
const segCount = (s: CubicSub) => s.closed ? (s.nodes.length > 1 ? s.nodes.length : 0) : s.nodes.length - 1;
const neighbours = (s: CubicSub, i: number): [CubicNode | undefined, CubicNode | undefined] => [
  s.nodes[i - 1] ?? (s.closed ? s.nodes[s.nodes.length - 1] : undefined),
  s.nodes[i + 1] ?? (s.closed ? s.nodes[0] : undefined),
];

// ---- Comb ----

export interface Tooth { a: Pt; b: Pt }

/**
 * Curvature comb. Teeth stand on the convex side (away from the centre of curvature, the Rhino/Illustrator convention:
 * a circle's comb is a larger concentric circle), length = clamp(|κ|·scale, 0, maxLen). `envelope` holds one polyline of
 * tooth tips per curved segment. Straight segments get nothing.
 */
export function comb(subs: CubicSub[], opts: { samples?: number; scale: number; maxLen?: number }): { teeth: Tooth[]; envelope: Pt[][] } {
  const samples = opts.samples ?? 24, maxLen = opts.maxLen ?? Infinity;
  const teeth: Tooth[] = [], envelope: Pt[][] = [];
  for (const s of subs) for (let i = 0; i < segCount(s); i++) {
    const c = segment(s, i);
    if (!c) continue;
    const tips: Pt[] = [];
    // ponytail: uniform t, not arc length — teeth bunch where the curve is slow. Reparametrise if it bothers anyone.
    for (let k = 0; k <= samples; k++) {
      const t = k / samples, a = cubicAt(...c, t), [d1] = derivs(...c, t), kap = curvatureAt(...c, t), speed = len(d1);
      const l = Math.min(Math.abs(kap) * opts.scale, maxLen);
      const n = speed < EPS ? { x: 0, y: 0 } : mul({ x: -d1.y, y: d1.x }, -Math.sign(kap) * l / speed);
      const b = add(a, n);
      teeth.push({ a, b });
      tips.push(b);
    }
    envelope.push(tips);
  }
  return { teeth, envelope };
}

// ---- Harmonize ----

type NodeRef = { sub: number; node: number };

function eachNode(subs: CubicSub[], refs: NodeRef[] | undefined, fn: (s: CubicSub, i: number) => boolean): { subs: CubicSub[]; changed: number } {
  const out = structuredClone(subs);
  let changed = 0;
  const list = refs ?? out.flatMap((s, si) => s.nodes.map((_, ni) => ({ sub: si, node: ni })));
  for (const r of list) if (out[r.sub]?.nodes[r.node] && fn(out[r.sub], r.node)) changed++;
  return { subs: out, changed };
}

/** New in/out handles for G2 at n with n fixed and handle directions fixed; null when not applicable. */
function g2Handles(prev: CubicNode, n: CubicNode, next: CubicNode): { in: Pt; out: Pt } | null {
  if (!n.smooth || !n.in || !n.out) return null;
  const vi = sub(n, n.in), vo = sub(n.out, n), li = len(vi), lo = len(vo);
  if (li < EPS || lo < EPS) return null;
  // signed distance of the far handle from the tangent line at n; κ at the node = (2/3)·h/ℓ²
  const hi = cross(vi, sub(prev.out ?? prev, n)) / li, ho = cross(vo, sub(next.in ?? next, n)) / lo;
  if (hi * ho <= 0) return null; // opposite turning (inflection at the node) or a flat side: nothing to equalise
  const k = ((2 / 3) * hi / (li * li) + (2 / 3) * ho / (lo * lo)) / 2;
  const ni = Math.sqrt((2 / 3) * hi / k), no = Math.sqrt((2 / 3) * ho / k);
  return { in: sub(n, mul(vi, ni / li)), out: add(n, mul(vo, no / lo)) };
}

/**
 * Harmonize (handles variant): for each smooth node between two curved segments, rescale the two adjacent handle lengths so
 * curvature is equal on both sides. On-curve points and handle directions never move. Iterates because neighbouring nodes
 * share handles.
 */
export function harmonizeHandles(subs: CubicSub[], nodeIndices?: NodeRef[]): { subs: CubicSub[]; changed: number } {
  const orig = subs;
  let cur = subs;
  // ponytail: fixed-point iteration, capped; converges in a handful of passes on sane outlines, diverging shapes just stop early.
  for (let pass = 0, moved = true; moved && pass < 20; pass++) {
    moved = false;
    cur = eachNode(cur, nodeIndices, (s, i) => {
      const n = s.nodes[i], [prev, next] = neighbours(s, i);
      if (!prev || !next) return false;
      const h = g2Handles(prev, n, next);
      if (!h || (near(h.in, n.in!, EPS) && near(h.out, n.out!, EPS))) return false;
      n.in = h.in; n.out = h.out;
      return moved = true;
    }).subs;
  }
  let changed = 0;
  cur.forEach((s, si) => s.nodes.forEach((n, ni) => {
    const o = orig[si].nodes[ni];
    if ((n.in && !near(n.in, o.in!, EPS)) || (n.out && !near(n.out, o.out!, EPS))) changed++;
  }));
  return { subs: cur, changed };
}

/**
 * Harmonize (Green Harmony / FontForge SplinePointHarmonize): handles stay, the node slides along its handle line to the G2
 * position t = √p / (√p + √n), p and n being the distances of the far handles from that line.
 */
export function harmonizeNode(subs: CubicSub[], nodeIndices?: NodeRef[]): { subs: CubicSub[]; changed: number } {
  return eachNode(subs, nodeIndices, (s, i) => {
    const n = s.nodes[i], [prev, next] = neighbours(s, i);
    if (!prev || !next || !n.smooth || !n.in || !n.out || near(n.in, n.out, EPS)) return false;
    const line = sub(n.out, n.in), l = len(line);
    const p = Math.abs(cross(line, sub(prev.out ?? prev, n))) / l, q = Math.abs(cross(line, sub(next.in ?? next, n))) / l;
    if (p < EPS || q < EPS) return false;
    const t = Math.sqrt(p) / (Math.sqrt(p) + Math.sqrt(q)), m = lerp(n.in, n.out, t);
    if (near(m, n, EPS)) return false;
    n.x = m.x; n.y = m.y;
    return true;
  });
}

// ---- Tunnify ----

/**
 * Tunnify: per curved segment with both handles, move both handles to the same fraction of the way toward the Tunni point
 * (intersection of the handle lines), the fraction being the mean of the current two. Parallel handle lines, missing handles
 * or handles pointing away from the intersection are skipped.
 */
export function tunnify(subs: CubicSub[], segIndices?: { sub: number; seg: number }[]): { subs: CubicSub[]; changed: number } {
  const out = structuredClone(subs);
  let changed = 0;
  const list = segIndices ?? out.flatMap((s, si) => Array.from({ length: segCount(s) }, (_, i) => ({ sub: si, seg: i })));
  for (const r of list) {
    const s = out[r.sub], a = s?.nodes[r.seg], b = s?.nodes[r.seg + 1] ?? (s?.closed ? s.nodes[0] : undefined);
    if (!a || !b || !a.out || !b.in) continue;
    const d1 = sub(a.out, a), d2 = sub(b.in, b), ab = sub(b, a), den = cross(d1, d2);
    if (Math.abs(den) < EPS * len(d1) * len(d2)) continue; // parallel (or degenerate) handle lines: no Tunni point
    const u = cross(ab, d2) / den, v = cross(ab, d1) / den; // T = a + u·d1 = b + v·d2
    if (u <= 0 || v <= 0) continue; // a handle points away from T
    const f = (1 / u + 1 / v) / 2;
    a.out = add(a, mul(d1, f * u)); b.in = add(b, mul(d2, f * v));
    changed++;
  }
  return { subs: out, changed };
}

// ---- Add extrema ----

const END_T = 0.01;   // candidates this close (in t) to a segment end are the end
const NODE_TOL = 0.5; // ...or this close (units) to an existing node

/** Roots of the derivative along one axis (t where x' = 0 or y' = 0), all real t. */
function extremaT(c: Cubic, k: 'x' | 'y'): number[] {
  const [p0, p1, p2, p3] = c;
  return quadRoots(-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k], 2 * (p0[k] - 2 * p1[k] + p2[k]), p1[k] - p0[k]);
}

function splitAt(c: Cubic, t: number): [Cubic, Cubic] {
  const [p0, p1, p2, p3] = c;
  const p01 = lerp(p0, p1, t), p12 = lerp(p1, p2, t), p23 = lerp(p2, p3, t), p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t), m = lerp(p012, p123, t);
  return [[p0, p01, p012, m], [m, p123, p23, p3]];
}

function setHandle(n: CubicNode, key: 'in' | 'out', p: Pt) {
  if (near(p, n, EPS)) delete n[key]; else n[key] = p;
}

/**
 * Insert smooth on-curve nodes at horizontal/vertical extrema (and optionally inflections) of every curved segment by exact
 * de Casteljau splitting. Candidates outside t ∈ (0.01, 0.99) or within 0.5 units of an existing/just-added node are skipped,
 * which makes this idempotent.
 */
export function addExtrema(subs: CubicSub[], opts: { inflections?: boolean } = {}): { subs: CubicSub[]; added: number } {
  const out = structuredClone(subs);
  let added = 0;
  for (const s of out) {
    const nodes = s.nodes, res: CubicNode[] = [];
    for (let i = 0; i < nodes.length; i++) {
      const a = nodes[i];
      res.push(a);
      const c0 = segment(s, i);
      if (!c0) continue;
      const b = nodes[i + 1] ?? nodes[0];
      const ts = [...extremaT(c0, 'x'), ...extremaT(c0, 'y'), ...(opts.inflections ? inflections(...c0) : [])]
        .filter(t => t > END_T && t < 1 - END_T).sort((p, q) => p - q);
      let c = c0, t0 = 0, last: CubicNode = a;
      const inserted: CubicNode[] = [];
      for (const t of ts) {
        const [l, r] = splitAt(c, (t - t0) / (1 - t0));
        if (near(r[0], a, NODE_TOL) || near(r[0], b, NODE_TOL) || near(r[0], last, NODE_TOL)) continue;
        setHandle(last, 'out', l[1]);
        last = { x: r[0].x, y: r[0].y, smooth: true };
        setHandle(last, 'in', l[2]);
        inserted.push(last);
        c = r; t0 = t;
      }
      if (!inserted.length) continue;
      setHandle(last, 'out', c[1]);
      setHandle(b, 'in', c[2]);
      res.push(...inserted);
      added += inserted.length;
    }
    s.nodes = res;
  }
  return { subs: out, added };
}

// ---- Missing extrema (FontForge Find Problems → "Missing extrema") ----

export interface MissingExtremum { sub: number; seg: number; t: number; pt: Pt; axis: 'x' | 'y' }

/**
 * Curved segments whose x' or y' vanishes strictly inside with no on-curve node there — the same candidates addExtrema
 * inserts (same root finder, same t ∈ (END_T, 1−END_T) window, same NODE_TOL), so addExtrema empties this list.
 * FontForge (splinechar.c SCValidate / problems.c missingextrema) skips segments whose chord is ≤ extrema_bound, default
 * (ascent+descent)/32 — "short splines (serifs) need no extrema" — hence `minLen` (default upm 1000 / 32).
 */
export function findMissingExtrema(subs: CubicSub[], opts: { minLen?: number; endMargin?: number } = {}): MissingExtremum[] {
  const minLen = opts.minLen ?? 1000 / 32, margin = opts.endMargin ?? END_T;
  const out: MissingExtremum[] = [];
  subs.forEach((s, si) => { for (let i = 0; i < segCount(s); i++) {
    const c = segment(s, i);
    if (!c || len(sub(c[3], c[0])) <= minLen) continue;
    const found: Pt[] = [];
    for (const axis of ['x', 'y'] as const) for (const t of extremaT(c, axis)) {
      if (t <= margin || t >= 1 - margin) continue;
      const pt = cubicAt(...c, t);
      if (near(pt, c[0], NODE_TOL) || near(pt, c[3], NODE_TOL) || found.some(q => near(pt, q, NODE_TOL))) continue; // one node fixes coincident x/y extrema
      found.push(pt); out.push({ sub: si, seg: i, t, pt, axis });
    }
  } });
  return out;
}
