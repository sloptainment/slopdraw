#pragma once
#include <QPointF>
#include <QTransform>
#include <cmath>

// Document -> view mapping for pan, zoom, rotate and mirror. Ops act in view space.
class View
{
public:
  void pan(QPointF d) { m_ *= QTransform::fromTranslate(d.x(), d.y()); }
  void zoomAt(QPointF a, double f) { about(a, QTransform().scale(f, f)); }
  void rotateAt(QPointF a, double deg) { about(a, QTransform().rotate(deg)); }
  void mirrorAt(QPointF a) { about(a, QTransform().scale(-1, 1)); }
  QPointF toView(QPointF doc) const { return m_.map(doc); }
  QPointF toDoc(QPointF view) const { return m_.inverted().map(view); }
  double screenRadius(double docRadius) const
  {
    return docRadius * std::hypot(m_.m11(), m_.m12());
  } // zoom only; rotate and mirror keep length
  const QTransform &matrix() const { return m_; }
  double zoom() const { return std::hypot(m_.m11(), m_.m12()); }
  // Degrees in (-180, 180]; a mirror flips the sign, and rotateAt still adds.
  double rotation() const
  {
    double deg = std::atan2(m_.m12(), m_.m11()) * 180 / M_PI +
                 (m_.determinant() < 0 ? 180 : 0);
    return deg > 180 ? deg - 360 : deg;
  }
  void setZoom(QPointF a, double z) { zoomAt(a, z / zoom()); }
  void setRotation(QPointF a, double deg) { rotateAt(a, deg - rotation()); }

private:
  void about(QPointF a, const QTransform &op)
  {
    m_ *= QTransform::fromTranslate(-a.x(), -a.y()) * op *
          QTransform::fromTranslate(a.x(), a.y());
  }
  QTransform m_;
};
