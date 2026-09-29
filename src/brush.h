#pragma once
#include <libmypaint/mypaint-brush.h>
#include <memory>
#include <QColor>
#include <QImage>
#include <QString>
#include <vector>

// A libmypaint brush loaded from a .myb file; null if the file is unreadable or not valid brush JSON.
std::shared_ptr<MyPaintBrush> loadBrush(const QString& mybPath);

// A .myb file in a brush directory: name is the file name without ".myb".
struct BrushEntry {
    QString name, path;
};

// The .myb files directly in dir, sorted by name.
std::vector<BrushEntry> listBrushes(const QString& dir);

// Sets the brush's base color (libmypaint color_h/color_s/color_v) for the strokes painted after.
void setBrushColor(MyPaintBrush* brush, const QColor& color);

// One pen reading in document coordinates; dt is seconds since the previous sample.
struct PenSample {
    float x, y, pressure, xtilt, ytilt;
    double dt;
};

// Replays samples through brush onto an RGBA8888 image (the Raster Layer), one mypaint_brush_stroke_to per sample.
// continuing = true carries on the brush's current stroke instead of starting a new one.
void paintStroke(MyPaintBrush* brush, QImage& layer, const std::vector<PenSample>& samples, bool continuing = false, bool eraser = false);

// Sets the brush's base radius in document pixels (libmypaint stores its natural log) and opacity (0..1) for the strokes painted after.
void setBrushRadius(MyPaintBrush* brush, float radius);
void setBrushOpacity(MyPaintBrush* brush, float opacity);
