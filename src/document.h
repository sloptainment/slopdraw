#pragma once
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QSize>
#include <memory>
#include <vector>
#include "brush.h"

class Document {
public:
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
    int layerCount() const { return int(layers_.size()); }
    int current() const { return current_; }
    bool visible(int layer) const { return layers_[layer].visible; }
    double opacity(int layer) const { return layers_[layer].opacity; }
    // "Background" for layer 0, "Layer N" for the Nth added layer; setName is one undo step (a same-name call is none).
    QString name(int layer) const { return layers_[layer].name; }
    void setName(int layer, const QString& name);
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
    enum Kind { Pixels, Add, Delete, Move, Visible, Opacity, Name };
    struct Step {
        Kind kind = Pixels;
        int layer = 0;
        QRect rect;
        QImage before, after;
        int to = 0;
        double a = 0, b = 0;
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
    };
    std::vector<Layer> layers_;  // bottom to top, never empty
    int current_ = 0;
    qsizetype budget_;
    std::vector<Step> history_;  // history_[0, applied_) are applied; the rest can be redone
    size_t applied_ = 0;
    // The current layer as it was when the stroke in progress began; null when none is in progress.
    // Shares pixels with that layer until the first dab detaches it, so starting a stroke copies once.
    QImage strokeStart_;
};
