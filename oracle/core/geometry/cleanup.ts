// Node-level fixes on cubic sub-paths: duplicate / zero-length node removal, point types, reversal.
// removeDuplicateNodes is after FontForge's RemoveZeroLengthSplines (splineutil2.c):
// a segment is zero-length when its chord is within tolerance AND its whole control polygon is (so a tight curved loop
// survives); the two nodes merge into one that keeps the first node's `in` and the last node's `out`.
import type { CubicNode, CubicSub, Pt } from '../model.ts';

const dist = (a: Pt, b: Pt) => Math.hypot(a.x - b.x, a.y - b.y);
const polyLen = (a: CubicNode, b: CubicNode) => dist(a, a.out ?? a) + dist(a.out ?? a, b.in ?? b) + dist(b.in ?? b, b);
const copy = (n: CubicNode): CubicNode => ({ ...n, ...(n.in && { in: { ...n.in } }), ...(n.out && { out: { ...n.out } }) });

/** Merge consecutive nodes closer than `tolerance` (closed sub-paths also check last→first). The merged node sits at the
 *  first node, keeps the first's `in` and the last's `out`, drops handles that collapse onto it, and is smooth only when
 *  both were. Sub-paths left with < 2 nodes are dropped. Pure: returns new sub-paths and the number of nodes removed. */
export function removeDuplicateNodes(subs: CubicSub[], tolerance: number): { subs: CubicSub[]; removed: number } {
  let removed = 0;
  const out: CubicSub[] = [];
  for (const sub of subs) {
    const nodes = sub.nodes.map(copy);
    const merged = new Set<CubicNode>();
    for (let i = 0; i < nodes.length && nodes.length > 1;) {
      const j = i + 1 < nodes.length ? i + 1 : sub.closed ? 0 : -1;
      if (j < 0 || j === i) break;
      const a = nodes[i]!, b = nodes[j]!;
      if (dist(a, b) > tolerance || polyLen(a, b) > tolerance) { i++; continue; }
      if (b.out) a.out = b.out; else delete a.out;
      a.smooth = !!(a.smooth && b.smooth); if (!a.smooth) delete a.smooth;
      merged.add(a); nodes.splice(j, 1); removed++;
      if (j < i) i--; // removed the first node (seam merge): a shifted down by one
    }
    for (const n of merged) { if (n.in && dist(n.in, n) <= tolerance) delete n.in; if (n.out && dist(n.out, n) <= tolerance) delete n.out; }
    if (nodes.length >= 2) out.push({ closed: sub.closed, nodes });
    else removed += nodes.length;
  }
  return { subs: out, removed };
}

/** Point types from geometry (a boolean drops them, FontForge infers them the same way on import): collinear handles on
 *  both sides → smooth; a handle collinear with the straight segment on the other side → tangent; anything else → corner.
 *  Only for contours a boolean produced — on a drawn contour it would demote the node types the user set. */
export function markPointTypes(nodes: CubicNode[]): void {
  const n = nodes.length;
  if (n < 2) return;
  for (let i = 0; i < n; i++) {
    const p = nodes[i], prev = nodes[(i + n - 1) % n], next = nodes[(i + 1) % n];
    const a = p.in ?? prev, b = p.out ?? next;
    const ax = p.x - a.x, ay = p.y - a.y, bx = b.x - p.x, by = b.y - p.y, la = Math.hypot(ax, ay), lb = Math.hypot(bx, by);
    delete p.smooth; delete p.symmetric; delete p.tangent;
    if (!la || !lb || ax * bx + ay * by <= 0 || Math.abs(ax * by - ay * bx) > 1e-3 * la * lb) continue;
    if (p.in && p.out) p.smooth = true; else if (p.in || p.out) p.tangent = true;
  }
}

/** The same contour running the other way: handles swap sides, node types (smooth / tangent / symmetric / auto) ride along. */
export function reverseSub(sub: CubicSub): CubicSub {
  const nodes = sub.nodes.slice().reverse().map(p => {
    const q: CubicNode = { ...p };
    delete q.in; delete q.out;
    if (p.out) q.in = p.out;
    if (p.in) q.out = p.in;
    return q;
  });
  return { ...sub, nodes };
}
