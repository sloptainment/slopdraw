#pragma once
#include <cmath>
#include <optional>
#include <set>
#include <vector>
#include <QTransform>
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

// The nodes without those at the selected indexes (Node tool Delete, #141); nullopt if fewer than 2 would remain.
inline std::optional<std::vector<SpiroNode>> removeNodes(const std::vector<SpiroNode>& nodes, const std::set<int>& selected) {
    std::vector<SpiroNode> kept;
    for (int i = 0; i < int(nodes.size()); i++)
        if (!selected.count(i)) kept.push_back(nodes[i]);
    if (kept.size() < 2) return std::nullopt;
    return kept;
}

// The nodes with those at the selected indexes set to type t (Tool Options picker, #141).
inline std::vector<SpiroNode> retypeNodes(std::vector<SpiroNode> nodes, const std::set<int>& selected, SpiroType t) {
    for (int i : selected)
        if (i >= 0 && i < int(nodes.size())) nodes[i].type = t;
    return nodes;
}

// The path with every node mapped through m (Transform box, #229); types, closed, knots and widths stay as they were.
inline Document::VPath transformNodes(Document::VPath p, const QTransform& m) {
    for (auto& n : p.nodes) m.map(n.x, n.y, &n.x, &n.y);
    return p;
}

// The paths with those at the picked indexes mapped through m (Transform box, #230).
inline std::vector<Document::VPath> transformPaths(std::vector<Document::VPath> paths, const std::set<int>& picked, const QTransform& m) {
    for (int i : picked)
        if (i >= 0 && i < int(paths.size())) paths[i] = transformNodes(paths[i], m);
    return paths;
}

// The union of the picked paths' Power Stroke outlines (Transform box, #230); null when none are picked.
inline QRectF pickedBounds(const std::vector<Document::VPath>& paths, const std::set<int>& picked) {
    QRectF box;
    for (int i : picked)
        if (i >= 0 && i < int(paths.size())) box |= Document::outline(paths[i]).boundingRect();
    return box;
}

// The Transform box handle (hx, hy each -1, 0 or 1; not both 0) within tol of p, or none (#231).
inline std::optional<QPoint> boxHandleAt(QRectF b, QPointF p, double tol) {
    for (int hx : {-1, 0, 1})
        for (int hy : {-1, 0, 1})
            if ((hx || hy) && std::hypot(p.x() - (b.center().x() + hx * b.width() / 2), p.y() - (b.center().y() + hy * b.height() / 2)) <= tol) return QPoint(hx, hy);
    return std::nullopt;
}

// The scale from dragging box handle h to pointer: about the opposite corner or edge, one axis only for an edge handle;
// keepRatio gives a corner the larger of its two factors on both axes (#231).
inline QTransform scaleMatrix(QRectF b, QPoint h, QPointF pointer, bool keepRatio) {
    QPointF c = b.center(), grab(c.x() + h.x() * b.width() / 2, c.y() + h.y() * b.height() / 2), anchor(c.x() - h.x() * b.width() / 2, c.y() - h.y() * b.height() / 2);
    double sx = h.x() && grab.x() != anchor.x() ? (pointer.x() - anchor.x()) / (grab.x() - anchor.x()) : 1;
    double sy = h.y() && grab.y() != anchor.y() ? (pointer.y() - anchor.y()) / (grab.y() - anchor.y()) : 1;
    if (keepRatio && h.x() && h.y()) sx = sy = std::abs(sx) >= std::abs(sy) ? sx : sy;
    return QTransform().translate(anchor.x(), anchor.y()).scale(sx, sy).translate(-anchor.x(), -anchor.y());
}
