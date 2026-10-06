#pragma once
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QTransform>
#include <QSize>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "brush.h"
#include "powerstroke.h"
#include "spiro.h"

// A Bezier path's node: its point, the handles before (in) and after (out) it, and how editing keeps the handles (#251).
enum class BezierType { Cusp, Smooth, Symmetric };
struct BezierNode { QPointF p, in, out; BezierType type; bool operator==(const BezierNode&) const = default; };

// A reference image (#533): the original picture, image pixels to document coordinates, the shown part in image pixels, and its opacity.
struct Reference { QImage image; QTransform transform; QRectF crop; double opacity = 1; };

class Document {
public:
    enum class PathKind { Spiro, Bezier };
    enum class PaintMode { Stroke, Fill, Both };
    struct VPath {
        std::vector<SpiroNode> nodes;
        bool closed;
        QColor color{};
        double width{};
        std::vector<WidthKnot> knots{};
        // color is the stroke color; fillColor paints the inside when mode is Fill or Both (#272).
        PathKind kind = PathKind::Spiro;
        PaintMode mode = PaintMode::Stroke;
        QColor fillColor = Qt::black;
        std::vector<BezierNode> bezier{};  // a Bezier path's nodes; nodes stays empty
    };
    // undoBudget caps the bytes of pixels the undo history keeps (#27); the oldest steps drop first, never the newest.
    explicit Document(QSize size, qsizetype undoBudget = qsizetype(1) << 30);
    // The visible layers composited bottom to top, source-over, each scaled by its opacity.
    QImage render() const;
    // Replays samples through the brush onto the current Raster Layer, one mypaint_brush_stroke_to per sample.
    // continuing = true carries on the current stroke (live pen input); otherwise a new stroke starts.
    // One stroke (a non-continuing call plus the continuing ones after it) is one undo step.
    void stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples, bool continuing = false, bool eraser = false);
    // Fills the closed polygon through points on the current Raster Layer: antialiased, non-zero winding, solid color.
    // eraser = true cuts the shape out of the alpha instead (to white under alpha lock). One call is one undo step.
    void fill(const QColor& color, const std::vector<QPointF>& points, bool eraser = false);
    // Bucket Fill (#604): fills the region of the current Raster Layer 4-connected to `at` whose pixels differ from the clicked one by at most threshold in every RGBA channel,
    // through fill's compositing (alpha lock, eraser, color's alpha). expand grows (or, negative, shrinks) the region by whole pixels, antialiased softens its edge,
    // sampleMerged matches against the visible composite instead of the layer (#605, #606).
    // Returns false, changing nothing, for a Vector Layer, a group or a point outside the document. One undo step.
    bool bucketFill(const QPoint& at, const QColor& color, int threshold, int expand = 0, bool antialiased = true, bool sampleMerged = true, bool eraser = false);
    // Clears a rectangle of the current Raster Layer to transparent, the background (layer 0) included. One undo step.
    // Returns false, changing nothing, for a Vector Layer.
    bool clearRect(const QRect& rect);
    // Select on a raster selection (#205). floatRect lifts rect's pixels from the current Raster Layer, clears rect, and draws the lifted pixels through m
    // (smooth): the live drag, no undo step. Every call starts again from the pixels as they were before the first one, so resampling never compounds.
    // transformRect is floatRect plus the one undo step; cancelFloat puts the layer back. Both calls refuse a Vector Layer (false).
    bool floatRect(const QRect& rect, const QTransform& m);
    bool transformRect(const QRect& rect, const QTransform& m);
    void cancelFloat();
    // Paste (#504): floatImage draws img through m over the current Raster Layer, live like floatRect; newLayer first adds a Raster Layer
    // on top for it (no step yet). pasteImage is floatImage plus one undo step, the new layer included; cancelFloat drops the float and that layer.
    // Both refuse a Vector Layer (false) unless newLayer. Pixels past the document edge are cropped.
    bool floatImage(const QImage& img, const QTransform& m, bool newLayer = false);
    bool pasteImage(const QImage& img, const QTransform& m, bool newLayer = false);
    // Select on a group (#461): floatGroup draws every raster item of the group's block (whole layers, smooth) and maps every vector item's paths through m,
    // live with no undo step; each call starts again from the items as they were before the first, so resampling never compounds. scaleWidths scales Width Knots too (#203).
    // transformGroup is floatGroup plus the one undo step; cancelFloat restores. Both refuse (false) a non-group or an empty one.
    bool floatGroup(int group, const QTransform& m, bool scaleWidths = false);
    bool transformGroup(int group, const QTransform& m, bool scaleWidths = false);
    // What a group draws: its raster items' non-transparent pixels united with its vector items' path outlines; null if nothing.
    QRectF groupBounds(int group) const;
    // The visible (composite) color at a document pixel; an invalid QColor when p is off the canvas.
    QColor colorAt(QPoint p) const;
    // Adds a transparent Raster Layer on top and makes it current; returns its index (0 is the white background).
    int addLayer();
    // Adds a transparent Vector Layer on top and makes it current; returns its index. Same undo as addLayer.
    int addVectorLayer();
    // Appends a Spiro path to the current Vector Layer, drawn as a Power Stroke `width` wide (flat ends), antialiased, in color.
    // One undo step. Returns false, changing nothing, when the current layer is a Raster Layer or the nodes give no curve (see spiroToPath).
    bool addPath(const std::vector<SpiroNode>& nodes, bool closed, const QColor& color, double width, PaintMode mode = PaintMode::Stroke, const QColor& fillColor = {});
    // Replaces path `path` of Vector Layer `layer` with these Width Knots (t = cubic index + fraction, full width) and redraws it; one undo step.
    // Returns false, changing nothing, for a Raster Layer, a bad path index or an empty knot list.
    bool setKnots(int layer, int path, const std::vector<WidthKnot>& knots);
    // Replaces the Spiro Nodes (and closed flag) of path `path` of Vector Layer `layer` and redraws it; one undo step. Knots stay as stored
    // unless a non-empty `knots` replaces them in the same step.
    // Returns false, changing nothing, for a Raster Layer, a bad path index or nodes that give no curve (see spiroToPath).
    bool setNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed, const std::vector<WidthKnot>& knots = {});
    // Turns Vector Layer `layer` into a Raster Layer keeping its pixels, name, visibility and opacity, and drops its paths; one undo step
    // that gives the editable Vector Layer back. Returns false, changing nothing, for a Raster Layer or a bad index.
    bool rasterizeLayer(int layer);
    // Replaces a Vector Layer's whole path list and redraws it, as one undo step; false (no step) for a Raster Layer or bad index.
    bool setPaths(int layer, const std::vector<VPath>& paths);
    // The topmost path of Vector Layer `layer` whose Spiro curve passes within tol of p, or -1 (also for a Raster Layer).
    int pathAt(int layer, QPointF p, double tol) const;
    // The indexes, bottom first, of the paths of Vector Layer `layer` whose drawn stroke (or, closed, Spiro curve fill) touches box; empty for a Raster Layer.
    std::vector<int> pathsIn(int layer, const QRectF& box) const;
    // The Spiro Node of path `path` nearest to p within tol, as its index, or -1 (also for a bad layer or path index).
    int nodeAt(int layer, int path, QPointF p, double tol) const;
    // The path's Spiro Nodes; empty for a bad layer or path index.
    std::vector<SpiroNode> nodes(int layer, int path) const;
    // Where each Width Knot of a path is drawn: its point on the curve moved half the knot's width along the left normal (-ty, tx); empty for a bad layer or path index.
    std::vector<QPointF> knotHandles(int layer, int path) const;
    // The path's current Width Knots; empty for a bad layer or path index.
    std::vector<WidthKnot> knots(int layer, int path) const;
    // The Width Knot a pointer at p drags a knot to: t of the nearest point of the path's curve, width twice the signed distance
    // from p to that point along the left normal (-ty, tx), so crossing the path makes it negative. nullopt for a bad layer or path index.
    std::optional<WidthKnot> projectKnot(int layer, int path, QPointF p) const;
    // Adds a Width Knot at the path point nearest p, with the width the stroke has there (so a flat stretch renders unchanged); one undo step.
    // Returns false, changing nothing, for a Raster Layer or a bad path index.
    bool addKnot(int layer, int path, QPointF p);
    // Inserts a Spiro Node of `type` at the path point nearest p; the Width Knots move with their stretch of path; one undo step.
    // Returns false, changing nothing, for a Raster Layer, a bad path index, or a point on an existing node.
    bool addNode(int layer, int path, QPointF p, SpiroType type);
    // Like setKnots but only redraws: no undo step, no checks beyond the indexes. For live dragging; finish with setKnots.
    void previewKnots(int layer, int path, const std::vector<WidthKnot>& knots);
    // Like setNodes but only redraws: no undo step; a node set with no curve leaves the path as it was. For live dragging; finish with setNodes.
    void previewNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed);
    // Replaces path `path` of Vector Layer `layer` with v and redraws: no undo step, no checks beyond the indexes. For live dragging (#313); finish with setPaths.
    void previewPath(int layer, int path, const VPath& v);
    int layerCount() const { return int(layers_.size()); }
    int current() const { return current_; }
    bool visible(int layer) const { return layers_[layer].visible; }
    double opacity(int layer) const { return layers_[layer].opacity; }
    // "Background" for layer 0, "Layer N" for the Nth added layer; setName is one undo step (a same-name call is none).
    QString name(int layer) const { return layers_[layer].name; }
    void setName(int layer, const QString& name);
    bool vector(int layer) const { return layers_[layer].vector; }
    // Groups (#424): every item has a depth (0 = root); a group has no pixels and sits directly above its children, the run below it that is deeper.
    int depth(int layer) const { return layers_[layer].depth; }
    bool isGroup(int layer) const { return layers_[layer].group; }
    // A group's children composited into one transparent image, at their own opacity and visibility (not the group's).
    QImage groupImage(int group) const;
    // Adds an empty group directly above the current item, at its depth, and makes it current; returns its index. One undo step.
    int addGroup();
    // Wraps item `layer` (with its block) in a new group at its place and returns the group's index, which becomes current; -1, no step, for the Background or a bad index. One undo step.
    int wrap(int layer) { return wrap(std::vector<int>{layer}); }
    // Wraps several items (each with its block) in one new group placed where the topmost was, the items inside in their stacking order (#472); the group becomes current. -1, no step, for the Background, a bad index or a broken tree.
    int wrap(const std::vector<int>& items);
    // Steps item `layer` with its block one place up or down (#425): past a plain item, out of its group at the group's top or bottom edge, into an expanded group next to it
    // (up: as its bottom item, down: as its top item), past a collapsed group whole. Refuses (false, no step) at the ends and below the Background. The current item keeps its identity.
    bool stepItem(int layer, bool up);
    // Steps several items one place each, as stepItem does, keeping the gaps between them; one undo step. If any one cannot step, none does (false, no step).
    bool stepItems(const std::vector<int>& items, bool up);
    // Moves item `layer` with its block so its block starts at gap `gap` (0..layerCount, counted in the list as it is now: before the item at that index) at `depth`; one undo step.
    // Refuses (false, no step) a gap inside its own block, below the Background, a depth that leaves a group without its place, or a move that changes nothing.
    bool moveItem(int layer, int gap, int depth) { return moveItems({layer}, gap, depth); }
    // Moves several items (each with its block) so they sit together, adjacent in stacking order, at gap `gap` and `depth`; one undo step. Refuses a gap inside any of their blocks.
    bool moveItems(const std::vector<int>& items, int gap, int depth);
    // Whether a group hides its children's rows in the Layers dock. Not undoable, not saved.
    bool collapsed(int layer) const { return layers_[layer].collapsed; }
    void setCollapsed(int layer, bool on) { layers_[layer].collapsed = on; }
    // Alpha Lock (#427): a Raster Layer flag, one undo step (a same-value call is none). A Vector Layer refuses it: returns false, no step.
    bool alphaLock(int layer) const { return layers_[layer].alphaLock; }
    bool setAlphaLock(int layer, bool on);
    // Inherit Alpha (#429): an item flag on Raster and Vector Layers, one undo step (a same-value call is none). render() cuts a flagged item by everything visible below it; the Background ignores it.
    bool inheritAlpha(int layer) const { return layers_[layer].inheritAlpha; }
    void setInheritAlpha(int layer, bool on);
    // A Vector Layer's paths, bottom first; empty for a Raster Layer. outline is the filled shape a path draws (its expanded Power Stroke).
    const std::vector<VPath>& paths(int layer) const { return layers_[layer].paths; }
    static QPainterPath outline(const VPath& v);
    // The curve the path's fill covers (open paths close by a straight line when filled).
    static QPainterPath curve(const VPath& v);
    // The same path as a Bezier path (#316): one node per skeleton cubic join, Smooth where its handles are collinear, else Cusp. Knots kept.
    static VPath toBezier(const VPath& v);
    // For loading: makes a layer (already restored) a Vector Layer with these paths and redraws it. Drops the undo history.
    void restorePaths(int layer, const std::vector<VPath>& paths);
    const QImage& image(int layer) const { return layers_[layer].image; }
    // For loading a file into a fresh Document: replaces a layer's contents outright and drops the undo history.
    void restoreLayer(int layer, const QImage& image, const QString& name, bool visible, double opacity, bool alphaLock = false, bool inheritAlpha = false, int depth = 0, bool group = false);
    void setCurrent(int layer);
    void setVisible(int layer, bool visible);
    void setOpacity(int layer, double opacity);
    // Both leave the background (layer 0) alone and return false when they do nothing. moveLayer puts layer `from` at index `to`.
    // Deleting a group deletes its children too, as one undo step.
    // Every layer op (add, delete, move, visibility, opacity) is one undo step, in the same history as strokes.
    bool deleteLayer(int layer);
    // The same settings on several layers (#444), each call one undo step and false, no step, when nothing changed. Groups and the Background are skipped (#474 takes groups);
    // setAlphaLock also skips Vector Layers, rasterizeLayers Raster Layers.
    bool setVisible(const std::vector<int>& layers, bool visible);
    bool setOpacity(const std::vector<int>& layers, double opacity);
    bool setAlphaLock(const std::vector<int>& layers, bool on);
    bool setInheritAlpha(const std::vector<int>& layers, bool on);
    bool rasterizeLayers(const std::vector<int>& layers);
    bool deleteLayers(const std::vector<int>& layers);
    bool moveLayer(int from, int to);
    // References, last on top; never layers, never in render(). add and set are one undo step each; preview is none (live drags), finish with set.
    const std::vector<Reference>& references() const { return refs_; }
    void addReference(const Reference& r);
    void setReferences(const std::vector<Reference>& refs);
    void previewReferences(const std::vector<Reference>& refs) { refs_ = refs; }
    // Each returns false when there is nothing to undo / redo.
    bool undo();
    bool redo();
    // The id of the undo step the history stands on, 0 for none: every new step gets a fresh, increasing id (#240's dirty marker).
    // Seals a stroke in progress first, as undo does, so call it between strokes, not during one.
    int stepId();

private:
    // Pixels: before/after pixels of the rect a stroke changed. The layer ops keep what undo needs: Add nothing (its layer is blank
    // once later steps are undone), Delete the layer's image in before plus its visible/opacity in a/b, Move the target index in to,
    // Visible, Opacity, AlphaLock and InheritAlpha the old and new value in a and b (Delete keeps both flags and the depth in to, lock as bit 0, inherit as bit 1, depth from bit 2; Tree the whole item list before and after in items and newItems, the current item in to and layer), Name the old and new in na and nb.
    // A Vector Layer's paths are kept as nodes and its image is redrawn from them; Add carries vector, Delete the paths, PathAdd the new path, Knots the old and new Width Knots of one path, Nodes the old and new path in paths[0] and paths[1] (index in to), Rasterize the dropped paths, Paths the old and new list in paths and newPaths.
    struct Layer {
        bool visible = true;
        double opacity = 1;
        QImage image;
        QString name;
        bool vector = false;
        std::vector<VPath> paths{};
        bool alphaLock = false;
        bool inheritAlpha = false;
        int depth = 0;
        bool group = false;  // no pixels: image is null
        bool collapsed = false;
    };
    enum Kind { Pixels, Add, Delete, Move, Visible, Opacity, Name, AlphaLock, InheritAlpha, PathAdd, Knots, Nodes, Rasterize, Paths, References, Tree };
    struct Step {
        Kind kind = Pixels;
        int id = 0;
        int layer = 0;
        QRect rect;
        QImage before, after;
        int to = 0;
        double a = 0, b = 0;
        bool vector = false;
        std::vector<VPath> paths, newPaths;
        std::vector<Layer> items, newItems;  // Tree
        std::vector<Reference> refs, newRefs;  // References: the old and new list
        std::vector<WidthKnot> ka, kb;  // Knots: path index in to, the old and new knots
        QString na, nb;  // Add: the new layer's name (nb unused); Delete: the deleted name in na; Name: old and new
    };
    // Discards the redo branch, appends s as applied, and drops the oldest steps past the budget.
    void push(Step s);
    // The first index of the block that ends at item i (i itself for a layer).
    int blockStart(int i) const;
    // Puts items [lo, hi] at index `at` of the list without them, depth shifted so the top item's is `depth`, as one Tree step; false, no step, if that changes nothing or breaks the tree.
    bool relocate(int lo, int hi, int at, int depth) { return relocate({{lo, hi}}, at, depth); }
    // The same for several blocks [lo, hi], ascending, gathered in order; with `group` a new group at `depth` is put above them and they sit one deeper. moved_ keeps where each item came from.
    bool relocate(const std::vector<std::pair<int, int>>& blocks, int at, int depth, bool group = false);
    // The blocks the items stand for, ascending, none inside another; empty if an index is bad or is the Background.
    std::vector<std::pair<int, int>> blocksOf(const std::vector<int>& items) const;
    // One Tree step in which `change` edits each of the layers that are not the Background or a group; it returns whether it changed that layer. False, no step, if none did.
    bool editLayers(const std::vector<int>& layers, const std::function<bool(Layer&)>& change);
    // Applies s (undo: reverse = true) to layers_.
    void apply(const Step& s, bool reverse);
    void applyMove(int from, int to);
    // Turns the stroke in progress (if any) into a Step of history_, and drops the oldest past the budget.
    void seal();

    // The items [lo, hi), all at depth d or deeper, composited into one transparent image: each level on its own, groups at their opacity.
    QImage compose(int lo, int hi, int d) const;
    // A list is a tree when, reading down from the top item (depth 0), depth only grows by one, right after a group (#424).
    static bool wellFormed(const std::vector<Layer>& v);
    // Redraws a Vector Layer's image from its paths.
    static void rasterize(Layer& l);
    std::vector<int> moved_;  // after a relocate: for each new index, the old one (-1 for a new group)
    std::vector<Layer> layers_;  // bottom to top, never empty
    int current_ = 0;
    std::vector<Reference> refs_;
    qsizetype budget_;
    std::vector<Step> history_;  // history_[0, applied_) are applied; the rest can be redone
    size_t applied_ = 0;
    int lastId_ = 0;  // the newest id handed out
    int baseId_ = 0;  // the id of the newest step dropped past the budget: what applied_ == 0 stands on
    // The current layer as it was when the stroke in progress began; null when none is in progress.
    // Shares pixels with that layer until the first dab detaches it, so starting a stroke copies once.
    QImage strokeStart_;
    std::vector<Layer> groupStart_;  // while a group floats: the items as they were, for seal to push as one Tree step
    bool pasting_ = false;  // a floatImage is in progress (strokeStart_ alone can't tell it from an unsealed stroke)
    int floatFrom_ = -1;  // while a paste floats in a layer it added: the layer that was current before; seal turns that layer into one Add step
};
