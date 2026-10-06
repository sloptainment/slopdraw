#pragma once
// The Transform box (Select, Reference tool): its frame, handles and the matrices its drags make, and paths through them.
#include <cmath>
#include <optional>
#include <set>
#include <vector>
#include <QTransform>
#include "document.h"

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
