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
    void stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples);

private:
    QImage layer_;
};
