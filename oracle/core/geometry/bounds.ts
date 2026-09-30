// Tight bounding boxes of cubic contours (via curve extrema, not control points).
import type { CubicSub, ExportGlyph, Pt } from '../model.ts';

export interface BBox { xMin: number; yMin: number; xMax: number; yMax: number }

export function contourBounds(subs: CubicSub[]): BBox | null {
  let b: BBox | null = null;
  const add = (x: number, y: number) => {
    if (!b) b = { xMin: x, yMin: y, xMax: x, yMax: y };
    else { b.xMin = Math.min(b.xMin, x); b.yMin = Math.min(b.yMin, y); b.xMax = Math.max(b.xMax, x); b.yMax = Math.max(b.yMax, y); }
  };
  for (const sub of subs) {
    const n = sub.nodes;
    for (let i = 0; i < n.length; i++) {
      add(n[i].x, n[i].y);
      const next = n[i + 1] ?? (sub.closed ? n[0] : null);
      if (!next || (!n[i].out && !next.in)) continue;
      for (const p of cubicExtrema(n[i], n[i].out ?? n[i], next.in ?? next, next)) add(p.x, p.y);
    }
  }
  return b;
}

/** Components are not resolved here (no Font in scope); the OT writer unions component bounds itself if it needs them. */
export function glyphBounds(g: ExportGlyph): BBox | null {
  return contourBounds(g.contours.map(c => ({ closed: true, nodes: c.nodes })));
}

/** Interior extremum points of a cubic (t in (0,1)), both axes. */
export function cubicExtrema(p0: Pt, p1: Pt, p2: Pt, p3: Pt): Pt[] {
  const out: Pt[] = [];
  for (const axis of ['x', 'y'] as const) {
    // derivative: a t² + b t + c
    const a = -p0[axis] + 3 * p1[axis] - 3 * p2[axis] + p3[axis];
    const b = 2 * (p0[axis] - 2 * p1[axis] + p2[axis]);
    const c = p1[axis] - p0[axis];
    const roots: number[] = [];
    if (Math.abs(a) < 1e-12) { if (Math.abs(b) > 1e-12) roots.push(-c / b); }
    else {
      const disc = b * b - 4 * a * c;
      if (disc >= 0) { const s = Math.sqrt(disc); roots.push((-b + s) / (2 * a), (-b - s) / (2 * a)); }
    }
    for (const t of roots) if (t > 0 && t < 1) out.push(cubicAt(p0, p1, p2, p3, t));
  }
  return out;
}

export function cubicAt(p0: Pt, p1: Pt, p2: Pt, p3: Pt, t: number): Pt {
  const u = 1 - t, a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
  return { x: a * p0.x + b * p1.x + c * p2.x + d * p3.x, y: a * p0.y + b * p1.y + c * p2.y + d * p3.y };
}

