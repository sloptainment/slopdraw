#pragma once
#include <QPointF>
#include <QTransform>
#include <cmath>

// Document -> view mapping for pan, zoom, rotate and mirror. Ops act in view space.
class View {
public:
    void pan(QPointF d) { m_ *= QTransform::fromTranslate(d.x(), d.y()); }
    void zoomAt(QPointF a, double f) { about(a, QTransform().scale(f, f)); }
    void rotateAt(QPointF a, double deg) { about(a, QTransform().rotate(deg)); }
    void mirrorAt(QPointF a) { about(a, QTransform().scale(-1, 1)); }
    QPointF toView(QPointF doc) const { return m_.map(doc); }
    QPointF toDoc(QPointF view) const { return m_.inverted().map(view); }
    double screenRadius(double docRadius) const { return docRadius * std::hypot(m_.m11(), m_.m12()); }  // zoom only; rotate and mirror keep length
    const QTransform& matrix() const { return m_; }

private:
    void about(QPointF a, const QTransform& op) {
        m_ *= QTransform::fromTranslate(-a.x(), -a.y()) * op * QTransform::fromTranslate(a.x(), a.y());
    }
    QTransform m_;
};
