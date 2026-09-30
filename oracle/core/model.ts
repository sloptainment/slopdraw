// slopfont document model. Pure data, no methods, no DOM. See CONTEXT.md for the vocabulary.
// Coordinates are font units, y UP, floats. Export rounds to integers.

/** SVG-style affine matrix [a b c d e f]: x' = a x + c y + e ; y' = b x + d y + f */
export type Mat = [number, number, number, number, number, number];
export const IDENTITY: Mat = [1, 0, 0, 1, 0, 0];

export interface Pt { x: number; y: number }

export interface Font {
  info: FontInfo;
  masters: Master[];         // ≥ 1; masters[0] is the DEFAULT master whose body is each Glyph's own children/sketch/images/advance
  glyphs: Glyph[];           // glyph order = export order; ".notdef" is synthesised if absent
  scratch: Scratchpad;       // font-wide freeform canvas; never exported
  kerning: Kerning;          // the default master's kerning
  masterKerning?: Record<string, Kerning>; // per non-default master, by name
  features: Feature[];
  measures?: Measure[];      // pinned Measures: drawn over every Glyph and the Scratchpad
  featuresRaw?: string;      // .fea text the interchange readers could not express as Feature rules; passthrough only (appended on UFO export)
}

/** A Master: a named design position with axis values (e.g. { name: 'Bold', axes: { wght: 700 } }) and optional vertical-metric overrides. */
export interface Master { name: string; axes: Record<string, number>; info?: Partial<Pick<FontInfo, 'ascender' | 'descender' | 'capHeight' | 'xHeight' | 'lineGap'>> }
/** One Master's drawing of a Glyph: ink, Components, sketch layers, images, advance. The default master's Body IS the Glyph's own fields (see bodyOf). */
export interface Body { children: Node[]; components: Component[]; sketch: SketchLayer[]; images: RefImage[]; advance: number }

export interface FontInfo {
  family: string;
  style: string;             // "Regular", "Bold Italic", ...
  upm: number;               // default 1000
  ascender: number;
  descender: number;         // negative
  lineGap: number;           // default 0
  capHeight: number;
  xHeight: number;
  version: string;           // "1.000"
  copyright: string;
  license: string;
  licenseUrl: string;
  vendorId: string;          // 4 chars
  designer: string;
  psName?: string;          // PostScript name (name id 6, CFF name, UFO postscriptFontName, sfdir FontName); empty = derived Family-Style (opentype/glyphs.ts psName)
}

export interface Glyph {
  name: string;              // AGL-style; unicodes derived from name by default
  unicodes: number[];
  advance: number;
  children: Node[];          // the ink
  components: Component[];   // explicit OpenType composites of the DEFAULT master's body; never auto-created. Other masters': masters[name].components
  sketch: SketchLayer[];     // never exported
  images: RefImage[];        // never exported
  masters?: Record<string, Body>; // bodies (ink + components + advance) for masters[1..] by name; missing = not yet drawn in that master
  guides?: Guide[];          // user Guides, shared by every master's body (design aids, never exported)
  measures?: Measure[];      // user Measures, shared by every master's body (re-read per master, never exported)
}

/** The Scratchpad: a free canvas with no metrics — ink, sketch layers, images, and user-placed Guides. Trace, build, then move objects into Glyphs. */
export interface Scratchpad { children: Node[]; sketch: SketchLayer[]; images: RefImage[]; guides: Guide[]; measures: Measure[] }
/** A user-placed straight guide line (Inkscape-style) on the Scratchpad or a Glyph. `pos` is y for horizontal, x for vertical, in font units. */
export interface Guide { id: string; axis: 'h' | 'v'; pos: number; label?: string }
/** A Measure: a straight measuring segment in font units, laid over a Glyph or the Scratchpad. Every crossing with the
 *  flattened ink is ticked and the spans between the ticks are labelled. A pinned one (Font.measures) is drawn in every
 *  Glyph, re-reading that Glyph's ink. Never exported. */
export interface Measure { id: string; a: Pt; b: Pt }

export interface Component { glyph: string; transform: Mat }
export interface SketchLayer { name: string; visible: boolean; children: Node[] }
export interface RefImage { id: string; href: string; transform: Mat; opacity: number; locked?: boolean } // href = zip entry path

export type Node = Group | Path | Shape | Clone;

interface Base {
  id: string;                // unique within the Font (Clones reference by id)
  label?: string;            // user-visible name; stored as inkscape:label so Inkscape shows the same
  opacity?: number;          // 0..1, default 1. Anything below 1 is construction ink: shown in the editor, NEVER exported
  transform?: Mat;           // undefined = identity
  cutter?: boolean;          // Cutter: subtracted from the siblings beneath it at export
  locked?: boolean;          // Layers & Objects 🔒: rendered, not selectable on the canvas (sodipodi:insensitive, so Inkscape locks it too)
  fill?: boolean;            // default true; false = stroke-only
  stroke?: Stroke;           // live Stroke, expanded at export
}

export interface Group extends Base { kind: 'group'; children: Node[] }
export interface Clone extends Base { kind: 'clone'; ref: string }   // ref = id of a Group/Path/Shape

export interface Stroke {
  width: number;
  cap: 'butt' | 'round' | 'square';
  join: 'miter' | 'round' | 'bevel';
  miterLimit: number;        // default 4
  /** Power Stroke (Inkscape's powerstroke LPE, opt-in): the width varies along the skeleton; `cap` is replaced by the
   *  power caps, `join`/`miterLimit` still apply. Saved as a live LPE so Inkscape shows the same outline. */
  power?: PowerStroke;
}
/** A width knot: `t` = segment index + fraction along the sub-path's CUBIC rendering (Inkscape's `offset_points` time,
 *  applied to every sub-path of the Path); `width` is the FULL width — Inkscape stores the half-width (offset), the
 *  conversion is width = 2 · offset · scale_width (core/io/project.ts). */
export interface PowerKnot { t: number; width: number }
export type PowerCap = 'zero' | 'butt' | 'square' | 'round' | 'peak';
export interface PowerStroke {
  knots: PowerKnot[];
  /** Inkscape's Linear / CubicBezierJohan (β = 0.2, ease in-out between knots) / CentripetalCatmullRom. */
  interpolator: 'linear' | 'cubic' | 'catmull';
  startCap: PowerCap;
  endCap: PowerCap;
  sort?: boolean;            // default true (Inkscape sort_points): knots are used in t order
}

// ---- Paths: one Mode per Path, several sub-paths each with its own direction ----

export type Path = CubicPath | SpiroPath | NurbsPath;
export type Mode = Path['mode'];

export interface CubicPath extends Base { kind: 'path'; mode: 'cubic'; subpaths: CubicSub[] }
export interface SpiroPath extends Base { kind: 'path'; mode: 'spiro'; subpaths: SpiroSub[] }
export interface NurbsPath extends Base { kind: 'path'; mode: 'nurbs'; subpaths: NurbsSub[] }

/** On-curve node with optional handles (absolute coordinates). Missing handle = straight to neighbour. */
/** `smooth`: handles stay collinear. `tangent`: the node joins a straight segment and a curve; the curve handle is locked to the line's direction and only its length may change (FontForge's tangent point). Both unset = corner. */
export interface CubicNode extends Pt { in?: Pt; out?: Pt; smooth?: boolean; symmetric?: boolean /* implies smooth: dragging one handle mirrors the other, length and all */; tangent?: boolean; auto?: boolean /* Inkscape auto-smooth: handles recomputed from the neighbours whenever they move */ }
export interface CubicSub { closed: boolean; nodes: CubicNode[] }

/** TrueType-style: on-curve and off-curve points in order; two consecutive off-curves imply an on-curve midpoint.
 *  Not a Mode you draw in — this is what the glyf table wants (cu2qu) and what a pre-2026 project's quadratic Paths are read as. */
export interface QuadPoint extends Pt { on: boolean }
export interface QuadSub { closed: boolean; points: QuadPoint[] }

/** libspiro control point types. */
export interface SpiroPoint extends Pt { type: 'corner' | 'g4' | 'g2' | 'left' | 'right' }
export interface SpiroSub { closed: boolean; points: SpiroPoint[] }

export interface NurbsPoint extends Pt { w: number }
export interface NurbsSub { closed: boolean; degree: number; points: NurbsPoint[]; knots?: number[] } // knots undefined = uniform clamped

// ---- Shapes: real SVG primitives, parametric until converted to a Path ----

export type Shape = Rect | Ellipse | Polygon;
export interface Rect extends Base { kind: 'rect'; x: number; y: number; width: number; height: number; rx: number; ry: number }
/** An ellipse, or with `start`/`end` Inkscape's arc of it: angles in radians, counter-clockwise in y-up font space, the sweep
 *  runs from start to end (both absent, or equal, = the whole ellipse). `arc` is how the sweep closes — 'pie' (Inkscape's
 *  slice: to the centre and back; the default), 'chord' (one straight closing line), 'arc' (left open: a fill draws the implicit chord). */
export interface Ellipse extends Base { kind: 'ellipse'; cx: number; cy: number; rx: number; ry: number; start?: number; end?: number; arc?: 'arc' | 'pie' | 'chord' }
export interface Polygon extends Base { kind: 'polygon'; points: Pt[] }

// ---- Spacing ----

export interface Kerning {
  /** Kerning Classes. Key = class name without '@'. */
  classes1: Record<string, string[]>;   // left side
  classes2: Record<string, string[]>;   // right side
  /** Key = `${left}|${right}` where each side is a glyph name or '@class'. Glyph–glyph pairs are Kerning Exceptions. */
  pairs: Record<string, number>;
}

// ---- Features (GSUB only) ----

/** `name`: the human-friendly label beside the tag — FontForge's lookup name on an sfdir import, otherwise whatever the
 *  user typed. Absent means "no name of its own": the UI shows the registered description for the tag instead. */
export interface Feature { tag: string; name?: string; rules: Rule[] }
export type Rule =
  | { type: 'single'; from: string; to: string }
  | { type: 'alternate'; from: string; to: string[] }
  | { type: 'ligature'; from: string[]; to: string };

// ---- Export-side shapes (what the OT writer consumes) ----

/** A closed cubic contour after the export pipeline: Clones resolved, Strokes expanded, Shapes/Modes flattened, Cutters applied, overlaps removed. */
export interface Contour { nodes: CubicNode[] }
export interface ExportGlyph { name: string; unicodes: number[]; advance: number; contours: Contour[]; components: Component[] }

export function emptyFont(family = 'Untitled'): Font {
  return {
    info: { family, style: 'Regular', upm: 1000, ascender: 800, descender: -200, lineGap: 0, capHeight: 700, xHeight: 500,
      version: '1.000', copyright: '', license: '', licenseUrl: '', vendorId: 'SLOP', designer: '' },
    masters: [{ name: 'Regular', axes: { wght: 400 } }],
    glyphs: [], scratch: { children: [], sketch: [], images: [], guides: [], measures: [] }, kerning: { classes1: {}, classes2: {}, pairs: {} }, features: [],
  };
}

// ---- Masters ----

export const defaultMaster = (font: Font): string => font.masters[0]!.name;

/** The Glyph's Body in a Master: the default master's is a live view onto the Glyph's own fields (same arrays, advance
 *  read/written through), other masters' are `glyph.masters[name]`; undefined = not drawn in that master yet. */
export function bodyOf(glyph: Glyph, master: string, font: Font): Body | undefined {
  if (master !== defaultMaster(font)) return glyph.masters?.[master];
  return {
    get children() { return glyph.children; }, set children(v) { glyph.children = v; },
    get components() { return glyph.components; }, set components(v) { glyph.components = v; },
    get sketch() { return glyph.sketch; }, set sketch(v) { glyph.sketch = v; },
    get images() { return glyph.images; }, set images(v) { glyph.images = v; },
    get advance() { return glyph.advance; }, set advance(v) { glyph.advance = v; },
  };
}

/** bodyOf, creating an empty Body (advance copied from the default) for a non-default master the Glyph has no drawing in. */
export function ensureBody(glyph: Glyph, master: string, font: Font): Body {
  const b = bodyOf(glyph, master, font);
  if (b) return b;
  if (!font.masters.some(m => m.name === master)) throw new Error(`unknown master ${master}`);
  glyph.masters ??= {};
  return glyph.masters[master] = { children: [], components: [], sketch: [], images: [], advance: glyph.advance };
}

/** A Master's Kerning; a non-default master's is created (empty) on first access so edits stick. */
export function kerningOf(font: Font, master: string): Kerning {
  if (master === defaultMaster(font)) return font.kerning;
  font.masterKerning ??= {};
  return font.masterKerning[master] ??= { classes1: {}, classes2: {}, pairs: {} };
}

/** Font info with the Master's vertical-metric overrides applied. */
export function infoOf(font: Font, master: string): FontInfo {
  return { ...font.info, ...font.masters.find(m => m.name === master)?.info };
}
