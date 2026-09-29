#pragma once
#include <QImage>
#include <QSize>
#include <memory>
#include <vector>
#include "brush.h"

class Document {
public:
    explicit Document(QSize size);
    QImage render() const;
    // Replays samples through the brush onto the Raster Layer, one mypaint_brush_stroke_to per sample.
    // continuing = true carries on the current stroke (live pen input); otherwise a new stroke starts.
    void stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples, bool continuing = false);

private:
    QImage layer_;
};
