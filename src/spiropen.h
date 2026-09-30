#pragma once
#include <cmath>
#include <vector>
#include "document.h"

// The Spiro Pen's path in progress (#28, /r/6): clicks place nodes of the current type, finish turns them into one path on the
// current Vector Layer. The Canvas decides what counts as a click and which key finishes.
struct SpiroPen {
    SpiroType type = SpiroType::G2;  // what Inkscape's Spiro mode makes for a smooth node
    std::vector<SpiroNode> nodes;

    void click(QPointF p) { nodes.push_back({p.x(), p.y(), type}); }
    // True when p is within tol of the first node: clicking there closes the path.
    bool onFirst(QPointF p, double tol) const { return !nodes.empty() && std::hypot(p.x() - nodes[0].x, p.y() - nodes[0].y) <= tol; }
    void backspace() { if (!nodes.empty()) nodes.pop_back(); }
    void cancel() { nodes.clear(); }
    // One undo step. On false (not a Vector Layer, or no curve) the nodes stay, so the user can carry on or cancel.
    bool finish(Document& doc, bool closed, const QColor& color, double width) {
        if (!doc.addPath(nodes, closed, color, width)) return false;
        nodes.clear();
        return true;
    }
};
