#pragma once

// The tools, in the order Canvas::setTool and the Tools dock number them.
enum Tool { BrushTool, LassoFillTool, SpiroPenTool, NodeKnotEditTool, SelectTool, BoxSelectTool, ReferenceTool, BucketFillTool };

// Brush, Lasso Fill, Box Select and Bucket Fill fit a Raster Layer, Spiro Pen and Node/Knot edit a Vector Layer, Select and Reference either (#221, #204, #539).
// A group takes Select and Reference alone (#426).
inline bool toolFitsKind(bool vector, int tool, bool group = false) { return group ? tool == SelectTool || tool == ReferenceTool : tool == SelectTool || tool == ReferenceTool || (tool >= SpiroPenTool && tool != BoxSelectTool && tool != BucketFillTool) == vector; }

// The tool after a layer switch (#170, #161): the current one if it fits the new layer's kind, else the last tool used on that kind.
inline int toolForLayer(bool vector, int current, int lastRaster = BrushTool, int lastVector = SpiroPenTool, bool group = false) {
    if (toolFitsKind(vector, current, group)) return current;
    if (group) return SelectTool;
    return vector ? lastVector : lastRaster;
}
