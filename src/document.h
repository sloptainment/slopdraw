#pragma once
#include <QImage>
#include <QSize>
#include <memory>
#include <vector>
#include "brush.h"

class Document {
public:
    // undoBudget caps the bytes of pixels the undo history keeps (#27); the oldest steps drop first, never the newest.
    explicit Document(QSize size, qsizetype undoBudget = qsizetype(1) << 30);
    QImage render() const;
    // Replays samples through the brush onto the Raster Layer, one mypaint_brush_stroke_to per sample.
    // continuing = true carries on the current stroke (live pen input); otherwise a new stroke starts.
    // One stroke (a non-continuing call plus the continuing ones after it) is one undo step.
    void stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples, bool continuing = false);
    // Each returns false when there is nothing to undo / redo.
    bool undo();
    bool redo();

private:
    // Before/after pixels of the rect a stroke changed.
    struct Step {
        QRect rect;
        QImage before, after;
    };
    // Turns the stroke in progress (if any) into a Step of history_, and drops the oldest past the budget.
    void seal();

    QImage layer_;
    qsizetype budget_;
    std::vector<Step> history_;  // history_[0, applied_) are applied; the rest can be redone
    size_t applied_ = 0;
    // The layer as it was when the stroke in progress began; null when none is in progress.
    // Shares pixels with layer_ until the first dab detaches layer_, so starting a stroke copies once.
    QImage strokeStart_;
};
