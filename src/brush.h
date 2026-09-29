#pragma once
#include <libmypaint/mypaint-brush.h>
#include <memory>
#include <QImage>
#include <QString>
#include <vector>

// A libmypaint brush loaded from a .myb file; null if the file is unreadable or not valid brush JSON.
std::shared_ptr<MyPaintBrush> loadBrush(const QString& mybPath);

// One pen reading in document coordinates; dt is seconds since the previous sample.
struct PenSample {
    float x, y, pressure, xtilt, ytilt;
    double dt;
};

// Replays samples through brush onto an RGBA8888 image (the Raster Layer), one mypaint_brush_stroke_to per sample.
// continuing = true carries on the brush's current stroke instead of starting a new one.
void paintStroke(MyPaintBrush* brush, QImage& layer, const std::vector<PenSample>& samples, bool continuing = false);
