#pragma once
#include <cmath>
#include <optional>
#include <set>
#include <vector>
#include <QLineF>
#include <QTransform>
#include "document.h"

// The Tool Options picker's rows (#53), in display order.
inline constexpr struct { const char* name; SpiroType type; } kNodeTypes[] = {
    {"Corner", SpiroType::Corner}, {"G4", SpiroType::G4}, {"G2", SpiroType::G2}, {"Left", SpiroType::Left}, {"Right", SpiroType::Right}};

// The Spiro Pen's path in progress (#28, /r/6): clicks place nodes of the current type, finish turns them into one path on the
// current Vector Layer. The Canvas decides what counts as a click and which key finishes.
struct SpiroPen {
    SpiroType type = SpiroType::G2;  // what Inkscape's Spiro mode makes for a smooth node
    bool bezier = false;  // the Tool Options toggle (#308): Bezier clicks place corners, finish makes a Bezier path
    std::vector<SpiroNode> nodes;  // in Bezier, all Corners: Spiro draws them as the same straight segments, so the preview is shared

    std::vector<QPointF> outs;  // Bezier: each node's out handle, parallel to nodes; the in handle mirrors it (#309)
    bool closing = false;  // Bezier: a press on the first node is down, so drag pulls node 0's handle and release closes (#368)

    void click(QPointF p, bool corner = false) { nodes.push_back({p.x(), p.y(), corner || bezier ? SpiroType::Corner : type}); outs.push_back(p); }  // Shift+click places a Corner (#274)
    // True when p is within tol of the first node: clicking there closes the path.
    bool onFirst(QPointF p, double tol) const { return !nodes.empty() && std::hypot(p.x() - nodes[0].x, p.y() - nodes[0].y) <= tol; }
    void backspace() { if (!nodes.empty()) nodes.pop_back(), outs.pop_back(); }
    void cancel() { nodes.clear(); outs.clear(); closing = false; }
    // Bezier: the last node's out handle follows the pointer (press-drag, #309); while closing, the first node's (#368).
    void drag(QPointF p) { if (bezier && !outs.empty()) (closing ? outs.front() : outs.back()) = p; }
    // The nodes as Bezier nodes: dragged ones Symmetric, the rest Cusps with handles on their points.
    std::vector<BezierNode> bezierNodes() const {
        std::vector<BezierNode> b;
        for (size_t i = 0; i < nodes.size(); i++) {
            QPointF p(nodes[i].x, nodes[i].y);
            b.push_back({p, 2 * p - outs[i], outs[i], outs[i] == p ? BezierType::Cusp : BezierType::Symmetric});
        }
        return b;
    }
    // One undo step. On false (not a Vector Layer, or no curve) the nodes stay, so the user can carry on or cancel.
    bool finish(Document& doc, bool closed, const QColor& color, double width, Document::PaintMode mode = Document::PaintMode::Stroke, const QColor& fillColor = {}) {
        if (bezier) {
            if (nodes.size() < 2 || !doc.vector(doc.current())) return false;
            Document::VPath v{{}, closed, color, width, {{0, width}}, Document::PathKind::Bezier, mode, fillColor.isValid() ? fillColor : color};
            if (!closed) v.knots.push_back({double(nodes.size() - 1), width});  // one cubic per segment (#306)
            v.bezier = bezierNodes();
            auto paths = doc.paths(doc.current());
            paths.push_back(v);
            if (!doc.setPaths(doc.current(), paths)) return false;
        } else if (!doc.addPath(nodes, closed, color, width, mode, fillColor)) return false;
        cancel();
        return true;
    }
};

// The nodes with those at the selected indexes moved by d (Node tool drag, #140).
inline std::vector<SpiroNode> moveNodes(std::vector<SpiroNode> nodes, const std::set<int>& selected, QPointF d) {
    for (int i : selected)
        if (i >= 0 && i < int(nodes.size())) { nodes[i].x += d.x(); nodes[i].y += d.y(); }
    return nodes;
}

// A Width Knot drag moves its point by a quarter of the pointer's motion since the press, for fine width/t control (#374).
inline constexpr double kKnotDragScale = 0.25;
inline QPointF knotDragTarget(QPointF knotStart, QPointF press, QPointF pointer) { return knotStart + (pointer - press) * kKnotDragScale; }

// The node with its out (or in) handle dragged to `to` (#313): a Smooth node turns the other handle opposite, keeping its length;
// a Symmetric one mirrors it; a Cusp leaves it.
inline BezierNode moveHandle(BezierNode n, bool out, QPointF to) {
    QPointF& h = out ? n.out : n.in;
    QPointF& o = out ? n.in : n.out;
    h = to;
    QPointF d = n.p - to;
    double len = std::hypot(d.x(), d.y());
    if (n.type == BezierType::Symmetric) o = n.p + d;
    else if (n.type == BezierType::Smooth && len > 0) o = n.p + d * (QLineF(n.p, o).length() / len);
    return n;
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

// The Bezier nodes with those at the selected indexes set to type t (Node/Knot edit picker, #314): the out handle (the in one if out
// sits on the point) stays, and moveHandle turns the other to suit, collinear for Smooth, mirrored for Symmetric. A node with both
// handles on its point (an undragged pen click) gets them pulled out along previous -> next, a third of the way to each (#368).
inline std::vector<BezierNode> retypeBezier(std::vector<BezierNode> nodes, const std::set<int>& selected, BezierType t, bool closed = false) {
    const int c = int(nodes.size());
    for (int i : selected)
        if (i >= 0 && i < c) {
            auto& n = nodes[i];
            n.type = t;
            if (t != BezierType::Cusp && n.in == n.p && n.out == n.p && c > 1) {
                QPointF prev = i > 0 || closed ? nodes[(i + c - 1) % c].p : n.p, next = i < c - 1 || closed ? nodes[(i + 1) % c].p : n.p;
                QPointF d = next - prev;
                double len = std::hypot(d.x(), d.y());
                if (len > 0) n.out = n.p + d / len * (QLineF(n.p, next).length() / 3), n.in = n.p - d / len * (QLineF(prev, n.p).length() / 3);
                if (n.out == n.p) n.out = 2 * n.p - n.in;  // an open end: mirror the one handle it got
            }
            bool out = n.out != n.p;
            n = moveHandle(n, out, out ? n.out : n.in);
        }
    return nodes;
}

// The path with every node mapped through m (Transform box, #229); types, closed and knot positions stay as they were. Knot widths
// do too, unless scaleWidths: then each is scaled by sqrt(|det m|) (#203).
inline Document::VPath transformNodes(Document::VPath p, const QTransform& m, bool scaleWidths = false) {
    for (auto& n : p.nodes) m.map(n.x, n.y, &n.x, &n.y);
    for (auto& n : p.bezier) n.p = m.map(n.p), n.in = m.map(n.in), n.out = m.map(n.out);
    if (scaleWidths)
        for (auto& k : p.knots) k.width *= std::sqrt(std::abs(m.determinant()));
    return p;
}

// The path at width w, every Width Knot scaled by w / its old width so the shape keeps its proportions (#256); knot t stays.
inline Document::VPath rewidthPath(Document::VPath p, double w) {
    if (p.width > 0)
        for (auto& k : p.knots) k.width *= w / p.width;
    p.width = w;
    return p;
}

// The paths with those at the picked indexes mapped through m (Transform box, #230).
inline std::vector<Document::VPath> transformPaths(std::vector<Document::VPath> paths, const std::set<int>& picked, const QTransform& m, bool scaleWidths = false) {
    for (int i : picked)
        if (i >= 0 && i < int(paths.size())) paths[i] = transformNodes(paths[i], m, scaleWidths);
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

// The Transform box as it stands: rect turned by angle degrees about its centre (#202). Unrotated until a rotate drag.
struct BoxFrame {
    QRectF rect;
    double angle = 0;
    // The turn about the centre that takes the box's own axes to the document's.
    QTransform toDoc() const { return QTransform().translate(rect.center().x(), rect.center().y()).rotate(angle).translate(-rect.center().x(), -rect.center().y()); }
};

// The turn about c that carries the pointer's direction at the drag's start to its direction now (#202).
inline QTransform rotateMatrix(QPointF c, QPointF start, QPointF pointer) {
    double a = std::atan2(pointer.y() - c.y(), pointer.x() - c.x()) - std::atan2(start.y() - c.y(), start.x() - c.x());
    return QTransform().translate(c.x(), c.y()).rotate(a * 180 / M_PI).translate(-c.x(), -c.y());
}

// The frame after m: centre mapped, each side scaled by what m does to its axis, angle that of the mapped x axis (#202).
inline BoxFrame frameAfter(BoxFrame f, const QTransform& m) {
    double a = f.angle * M_PI / 180;
    QPointF c = f.rect.center(), mx = m.map(c + QPointF(std::cos(a), std::sin(a))) - m.map(c), my = m.map(c + QPointF(-std::sin(a), std::cos(a))) - m.map(c);
    QSizeF size(f.rect.width() * std::hypot(mx.x(), mx.y()), f.rect.height() * std::hypot(my.x(), my.y()));
    QRectF rect(QPointF(), size);
    rect.moveCenter(m.map(c));
    return {rect, std::atan2(mx.y(), mx.x()) * 180 / M_PI};
}

// boxHandleAt for a turned box.
inline std::optional<QPoint> frameHandleAt(const BoxFrame& f, QPointF p, double tol) { return boxHandleAt(f.rect, f.toDoc().inverted().map(p), tol); }

// scaleMatrix for a turned box: the scale happens along the box's own axes.
inline QTransform frameScaleMatrix(const BoxFrame& f, QPoint h, QPointF pointer, bool keepRatio) {
    QTransform t = f.toDoc();
    return t.inverted() * scaleMatrix(f.rect, h, t.inverted().map(pointer), keepRatio) * t;
}

// The mirror about c: left-right (x flipped) when horizontal, else top-bottom (#203).
inline QTransform flipMatrix(QPointF c, bool horizontal) {
    return QTransform().translate(c.x(), c.y()).scale(horizontal ? -1 : 1, horizontal ? 1 : -1).translate(-c.x(), -c.y());
}
