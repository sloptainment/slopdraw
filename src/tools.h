#pragma once

// The tools, in the order Canvas::setTool and the Tools dock number them.
enum Tool { BrushTool, LassoFillTool, SpiroPenTool, NodeKnotEditTool, SelectTool, BoxSelectTool, ReferenceTool };

// Brush, Lasso Fill and Box Select fit a Raster Layer, Spiro Pen and Node/Knot edit a Vector Layer, Select and Reference either (#221, #204, #539).
inline bool toolFitsKind(bool vector, int tool) { return tool == SelectTool || tool == ReferenceTool || (tool >= SpiroPenTool && tool != BoxSelectTool) == vector; }

// The tool after a layer switch (#170, #161): the current one if it fits the new layer's kind, else the last tool used on that kind.
inline int toolForLayer(bool vector, int current, int lastRaster = BrushTool, int lastVector = SpiroPenTool) {
    if (toolFitsKind(vector, current)) return current;
    return vector ? lastVector : lastRaster;
}
