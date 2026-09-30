#pragma once

// The tools, in the order Canvas::setTool and the Tools dock number them.
enum Tool { BrushTool, LassoFillTool, SpiroPenTool, NodeKnotEditTool };

// The tool after a layer switch (#170, #161): the current one if it fits the new layer's kind (Brush and Lasso Fill fit a
// Raster Layer, Spiro Pen and Node/Knot edit a Vector Layer), else the last tool used on that kind.
inline int toolForLayer(bool vector, int current, int lastRaster = BrushTool, int lastVector = SpiroPenTool) {
    if ((current >= SpiroPenTool) == vector) return current;
    return vector ? lastVector : lastRaster;
}
