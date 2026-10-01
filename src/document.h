#pragma once
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QSize>
#include <memory>
#include <optional>
#include <vector>
#include "brush.h"
#include "powerstroke.h"
#include "spiro.h"

class Document {
public:
    struct VPath {
        std::vector<SpiroNode> nodes;
        bool closed;
        QColor color;
        double width;
        std::vector<WidthKnot> knots;
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
    // eraser = true cuts the shape out instead (to white). One call is one undo step.
    void fill(const QColor& color, const std::vector<QPointF>& points, bool eraser = false);
    // The visible (composite) color at a document pixel; an invalid QColor when p is off the canvas.
    QColor colorAt(QPoint p) const;
    // Adds a transparent Raster Layer on top and makes it current; returns its index (0 is the white background).
    int addLayer();
    // Adds a transparent Vector Layer on top and makes it current; returns its index. Same undo as addLayer.
    int addVectorLayer();
    // Appends a Spiro path to the current Vector Layer, drawn as a Power Stroke `width` wide (flat ends), antialiased, in color.
    // One undo step. Returns false, changing nothing, when the current layer is a Raster Layer or the nodes give no curve (see spiroToPath).
    bool addPath(const std::vector<SpiroNode>& nodes, bool closed, const QColor& color, double width);
    // Replaces path `path` of Vector Layer `layer` with these Width Knots (t = cubic index + fraction, full width) and redraws it; one undo step.
    // Returns false, changing nothing, for a Raster Layer, a bad path index or an empty knot list.
    bool setKnots(int layer, int path, const std::vector<WidthKnot>& knots);
    // Replaces the Spiro Nodes (and closed flag) of path `path` of Vector Layer `layer` and redraws it; one undo step. Knots stay as stored.
    // Returns false, changing nothing, for a Raster Layer, a bad path index or nodes that give no curve (see spiroToPath).
    bool setNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed);
    // The topmost path of Vector Layer `layer` whose Spiro curve passes within tol of p, or -1 (also for a Raster Layer).
    int pathAt(int layer, QPointF p, double tol) const;
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
    // Like setKnots but only redraws: no undo step, no checks beyond the indexes. For live dragging; finish with setKnots.
    void previewKnots(int layer, int path, const std::vector<WidthKnot>& knots);
    // Like setNodes but only redraws: no undo step; a node set with no curve leaves the path as it was. For live dragging; finish with setNodes.
    void previewNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed);
    int layerCount() const { return int(layers_.size()); }
    int current() const { return current_; }
    bool visible(int layer) const { return layers_[layer].visible; }
    double opacity(int layer) const { return layers_[layer].opacity; }
    // "Background" for layer 0, "Layer N" for the Nth added layer; setName is one undo step (a same-name call is none).
    QString name(int layer) const { return layers_[layer].name; }
    void setName(int layer, const QString& name);
    bool vector(int layer) const { return layers_[layer].vector; }
    // A Vector Layer's paths, bottom first; empty for a Raster Layer. outline is the filled shape a path draws (its expanded Power Stroke).
    const std::vector<VPath>& paths(int layer) const { return layers_[layer].paths; }
    static QPainterPath outline(const VPath& v);
    // For loading: makes a layer (already restored) a Vector Layer with these paths and redraws it. Drops the undo history.
    void restorePaths(int layer, const std::vector<VPath>& paths);
    const QImage& image(int layer) const { return layers_[layer].image; }
    // For loading a file into a fresh Document: replaces a layer's contents outright and drops the undo history.
    void restoreLayer(int layer, const QImage& image, const QString& name, bool visible, double opacity);
    void setCurrent(int layer);
    void setVisible(int layer, bool visible);
    void setOpacity(int layer, double opacity);
    // Both leave the background (layer 0) alone and return false when they do nothing. moveLayer puts layer `from` at index `to`.
    // Every layer op (add, delete, move, visibility, opacity) is one undo step, in the same history as strokes.
    bool deleteLayer(int layer);
    bool moveLayer(int from, int to);
    // Each returns false when there is nothing to undo / redo.
    bool undo();
    bool redo();

private:
    // Pixels: before/after pixels of the rect a stroke changed. The layer ops keep what undo needs: Add nothing (its layer is blank
    // once later steps are undone), Delete the layer's image in before plus its visible/opacity in a/b, Move the target index in to,
    // Visible and Opacity the old and new value in a and b, Name the old and new in na and nb.
    // A Vector Layer's paths are kept as nodes and its image is redrawn from them; Add carries vector, Delete the paths, PathAdd the new path, Knots the old and new Width Knots of one path, Nodes the old and new path in paths[0] and paths[1] (index in to).
    enum Kind { Pixels, Add, Delete, Move, Visible, Opacity, Name, PathAdd, Knots, Nodes };
    struct Step {
        Kind kind = Pixels;
        int layer = 0;
        QRect rect;
        QImage before, after;
        int to = 0;
        double a = 0, b = 0;
        bool vector = false;
        std::vector<VPath> paths;
        std::vector<WidthKnot> ka, kb;  // Knots: path index in to, the old and new knots
        QString na, nb;  // Add: the new layer's name (nb unused); Delete: the deleted name in na; Name: old and new
    };
    // Discards the redo branch, appends s as applied, and drops the oldest steps past the budget.
    void push(Step s);
    // Applies s (undo: reverse = true) to layers_.
    void apply(const Step& s, bool reverse);
    void applyMove(int from, int to);
    // Turns the stroke in progress (if any) into a Step of history_, and drops the oldest past the budget.
    void seal();

    struct Layer {
        bool visible = true;
        double opacity = 1;
        QImage image;
        QString name;
        bool vector = false;
        std::vector<VPath> paths;
    };
    // Redraws a Vector Layer's image from its paths.
    static void rasterize(Layer& l);
    std::vector<Layer> layers_;  // bottom to top, never empty
    int current_ = 0;
    qsizetype budget_;
    std::vector<Step> history_;  // history_[0, applied_) are applied; the rest can be redone
    size_t applied_ = 0;
    // The current layer as it was when the stroke in progress began; null when none is in progress.
    // Shares pixels with that layer until the first dab detaches it, so starting a stroke copies once.
    QImage strokeStart_;
};
