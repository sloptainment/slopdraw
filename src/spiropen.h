#pragma once
#include <cmath>
#include <set>
#include <vector>
#include "document.h"

// The Tool Options picker's rows (#53), in display order.
inline constexpr struct { const char* name; SpiroType type; } kNodeTypes[] = {
    {"Corner", SpiroType::Corner}, {"G4", SpiroType::G4}, {"G2", SpiroType::G2}, {"Left", SpiroType::Left}, {"Right", SpiroType::Right}};

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

// The nodes with those at the selected indexes moved by d (Node tool drag, #140).
inline std::vector<SpiroNode> moveNodes(std::vector<SpiroNode> nodes, const std::set<int>& selected, QPointF d) {
    for (int i : selected)
        if (i >= 0 && i < int(nodes.size())) { nodes[i].x += d.x(); nodes[i].y += d.y(); }
    return nodes;
}
