// Boolean ops on cubic sub-paths via paper.js, with nonzero-winding semantics over the input directions (like skia-pathops' union).
// In the browser paper is a classic UMD script (global `paper`); in Node it is imported. Either way, headless.
//
// paper's self-resolve (`item.unite(null)`) mis-handles contours that share edges without crossing (T-junctions, collinear
// overlaps) and knows only OR semantics across paths, so nonzero winding is computed here: contours are split into simple
// loops, clustered by touching bounds, and folded one at a time into cumulative winding regions using paper's (robust)
// pairwise intersect/subtract/unite.
import type { CubicNode, CubicSub } from '../model.ts';
import { reverseSub } from './cleanup.ts';

const paper: paper.PaperScope = (globalThis as any).paper ?? (await import('paper')).default;
if (!paper.project) {
  paper.setup(new paper.Size(1, 1));
  paper.view?.remove(); // headless: the view only makes paper listen on document mousemove and read layout there, once per move
}

const EPS = 1e-6;
const OPTS = { insert: false };

/** Union (remove overlap) of everything, nonzero winding over the input directions. Open sub-paths are treated as closed. */
export function union(subs: CubicSub[][]): CubicSub[] {
  return fromPaper(fold(subs.flat()));
}

/** Intersection (the overlap) of every operand; each operand is folded to its own nonzero-winding region first. */
export function intersect(subs: CubicSub[][]): CubicSub[] {
  let acc = fold(subs[0] ?? []);
  for (const s of subs.slice(1)) {
    const b = acc && fold(s);
    if (!acc || !b) return [];
    acc = acc.intersect(b, OPTS);
  }
  return fromPaper(acc);
}

/** One drawn object's contours, oriented so the object adds ink: a bag whose signed area comes out negative is reversed
 *  whole. Directions *within* an object are already what the editor renders under nonzero winding and must survive — the
 *  counter of an A is a hole against its overlapping siblings, not nested inside any one of them — and reversing every
 *  contour of an object negates its winding numbers, leaving its fill identical. Contours with fewer than two nodes or no
 *  area are dropped. Only the node order changes, so drawn node types ride along; paper's reorient would rebuild the segments.
 *  Needed because separate objects, painted independently in the editor, land in one nonzero-filled contour set on export. */
export function orientNested(subs: CubicSub[]): CubicSub[] {
  const keep: CubicSub[] = [];
  let total = 0;
  for (const s of subs) {
    if (s.nodes.length < 2) continue;
    const area = toPaperPath(s).area; // paper's shoelace over our own coordinates: > 0 is CCW in y-up, as in fromPaper
    if (Math.abs(area) < EPS) continue;
    total += area;
    keep.push(s);
  }
  return total < 0 ? keep.map(reverseSub) : keep;
}

export function subtract(a: CubicSub[], b: CubicSub[]): CubicSub[] {
  const A = fold(a), B = fold(b);
  if (!A || !B) return fromPaper(A);
  return fromPaper(A.subtract(B, OPTS));
}

/** Nonzero fill of a contour set as a clean paper item (null if empty). */
function fold(subs: CubicSub[]): paper.PathItem | null {
  // 1. split self-intersecting contours into simple loops (a re-splicing of the same oriented curve, so windings add up unchanged)
  const loops: paper.Path[] = [];
  for (const sub of subs.flatMap(splitAtRepeats)) {
    if (sub.nodes.length < 2) continue;
    const p = toPaperPath(sub);
    const r: paper.PathItem = p.getCrossings(p).length ? (p as any).resolveCrossings() : p; // resolveCrossings is missing from paper.d.ts
    for (const l of (r.children ?? [r]) as paper.Path[]) if (Math.abs(l.area) > EPS) loops.push(l);
  }
  // 2. a loop only changes the winding of points inside it, so loops whose bounds never touch cannot see each other:
  //    fold each cluster alone. A Scratchpad of separate drawings then costs the sum of its parts, not every loop
  //    against the whole page (paper's collision pass is quadratic in the accumulated region).
  const parts = clusters(loops).map(windingRegion).filter((p): p is paper.PathItem => !!p);
  if (parts.length < 2) return parts[0] ?? null;
  return new paper.CompoundPath({ children: parts.flatMap(p => (p.children ?? [p]) as paper.Path[]), insert: false });
}

/** Connected components of loops whose bounds touch (union-find over the O(n²) bounds pairs; n is small). */
function clusters(loops: paper.Path[]): paper.Path[][] {
  const bounds = loops.map(l => l.bounds.expand(EPS));
  const owner = loops.map((_, i) => i);
  const find = (i: number): number => owner[i] === i ? i : (owner[i] = find(owner[i]!));
  for (let i = 0; i < loops.length; i++) for (let j = i + 1; j < loops.length; j++) if (bounds[i]!.intersects(bounds[j]!)) owner[find(i)] = find(j);
  const by = new Map<number, paper.Path[]>();
  loops.forEach((l, i) => { const k = find(i); const c = by.get(k); if (c) c.push(l); else by.set(k, [l]); });
  return [...by.values()];
}

/** Cumulative winding regions of one cluster: G[k-1] = {w ≥ k}, L[k-1] = {w ≤ -k}. Nested regions, so no re-uniting of
 *  edge-sharing tiles (paper's weak spot). Adding a loop of sign s shifts w by s under it. */
function windingRegion(loops: paper.Path[]): paper.PathItem | null {
  let G: paper.PathItem[] = [], L: paper.PathItem[] = [];
  for (const loop of loops) {
    const s = loop.area > 0 ? 1 : -1;
    const [up, down] = s > 0 ? [G, L] : [L, G];
    const nextUp: paper.PathItem[] = [], nextDown: paper.PathItem[] = [];
    for (let k = 0; k <= up.length; k++) { // {|w| ≥ k+1}' = {|w| ≥ k+1} ∪ ({|w| ≥ k} ∩ c), where {|w| ≥ 0} = not on the other side
      const lifted = k > 0 ? up[k - 1].intersect(loop, OPTS) : down[0] ? loop.subtract(down[0], OPTS) : loop;
      const cur = up[k] ? up[k].unite(lifted, OPTS) : lifted;
      if (!cur.isEmpty()) nextUp.push(cur);
    }
    for (let k = 0; k < down.length; k++) { // {|w| ≥ k+1}' = {|w| ≥ k+1} − (c − {|w| ≥ k+2})
      const removed = down[k + 1] ? loop.subtract(down[k + 1], OPTS) : loop;
      const cur = down[k].subtract(removed, OPTS);
      if (!cur.isEmpty()) nextDown.push(cur);
    }
    if (s > 0) { G = nextUp; L = nextDown; } else { L = nextUp; G = nextDown; }
  }
  // ponytail: G[0] and L[0] abut only where a + and a − loop share an edge exactly (mirrored halves); paper's unite copes with the simple cases.
  return G[0] && L[0] ? G[0].unite(L[0], OPTS) : G[0] ?? L[0] ?? null;
}

/** Split a contour into loops at repeated vertices (self-touching / doubled edges), which paper's resolveCrossings leaves as spurs.
 * Re-splicing at a shared vertex keeps the winding sum unchanged. */
function splitAtRepeats(sub: CubicSub): CubicSub[] {
  const out: CubicSub[] = [];
  const stack: CubicNode[] = [];
  const key = (n: CubicNode) => `${Math.round(n.x * 1e6)},${Math.round(n.y * 1e6)}`;
  const at = new Map<string, number>();
  for (const n of sub.nodes) {
    const k = key(n), i = at.get(k);
    if (i === undefined) { at.set(k, stack.length); stack.push({ ...n }); continue; }
    const loop = stack.splice(i);
    for (const l of loop) at.delete(key(l));
    const first = loop[0];
    loop[0] = { x: first.x, y: first.y, ...(first.out && { out: first.out }), ...(n.in && { in: n.in }) };
    if (loop.length >= 2) out.push({ closed: true, nodes: loop });
    at.set(k, stack.length);
    stack.push({ x: n.x, y: n.y, ...(first.in && { in: first.in }), ...(n.out && { out: n.out }) });
  }
  if (stack.length >= 2) out.push({ closed: true, nodes: stack });
  return out;
}

function toPaperPath(sub: CubicSub): paper.Path {
  const p = new paper.Path(OPTS);
  for (const n of sub.nodes) {
    const hin = n.in ? new paper.Point(n.in.x - n.x, n.in.y - n.y) : undefined;
    const hout = n.out ? new paper.Point(n.out.x - n.x, n.out.y - n.y) : undefined;
    p.add(new paper.Segment(new paper.Point(n.x, n.y), hin, hout));
  }
  p.closed = true;
  return p;
}

/** Back to CubicSub, fixing direction to PostScript convention (outer CCW in y-up = positive signed area, holes negative),
 * dropping degenerate contours and collinear handle-less artefact nodes. */
function fromPaper(item: paper.PathItem | null): CubicSub[] {
  if (!item) return [];
  const paths = item instanceof paper.CompoundPath ? (item.children as paper.Path[]) : [item as paper.Path];
  // paper's y-down "clockwise" == positive signed area == CCW in y-up, so reorient(nonZero, clockwise=true) is exactly our convention.
  const cp = new paper.CompoundPath({ children: paths.map(p => p.clone(OPTS)), insert: false });
  cp.reorient(true, true);
  const out: CubicSub[] = [];
  for (const p of cp.children as paper.Path[]) {
    if (Math.abs(p.area) < EPS) continue;
    const nodes: CubicNode[] = [];
    for (const s of p.segments) {
      const n: CubicNode = { x: s.point.x, y: s.point.y };
      if (s.handleIn.length > EPS) n.in = { x: s.point.x + s.handleIn.x, y: s.point.y + s.handleIn.y };
      if (s.handleOut.length > EPS) n.out = { x: s.point.x + s.handleOut.x, y: s.point.y + s.handleOut.y };
      nodes.push(n);
    }
    const cleaned = dropCollinear(nodes);
    if (cleaned.length >= 2) out.push({ closed: true, nodes: cleaned });
  }
  return out;
}

/** Remove a node with no handles on either side that lies on the straight line between its neighbours (or duplicates its predecessor). */
function dropCollinear(nodes: CubicNode[]): CubicNode[] {
  const res = nodes.slice();
  for (let i = 0; i < res.length && res.length > 2;) {
    const prev = res[(i + res.length - 1) % res.length], n = res[i], next = res[(i + 1) % res.length];
    if (n.in || n.out || prev.out || next.in) { i++; continue; }
    const ax = n.x - prev.x, ay = n.y - prev.y, bx = next.x - n.x, by = next.y - n.y;
    const cross = ax * by - ay * bx, dot = ax * bx + ay * by, len = Math.hypot(ax, ay) + Math.hypot(bx, by);
    if ((Math.abs(cross) < EPS * len && dot >= 0) || Math.hypot(ax, ay) < EPS) res.splice(i, 1);
    else i++;
  }
  return res;
}
