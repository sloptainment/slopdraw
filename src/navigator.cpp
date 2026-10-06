#include "navigator.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include "canvas.h"

void Navigator::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  p.fillRect(rect(), QColor(90, 90, 90));
  if (!canvas_->hasDoc())
    return;
  QTransform toThumb = fit();
  QRectF r =
    toThumb.mapRect(QRectF(QPointF(), QSizeF(canvas_->doc().image(0).size())));
  if (thumb_.isNull())
    thumb_ = canvas_->doc().render().scaled(
      r.size().toSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  p.fillRect(r, Qt::white);
  p.drawImage(r.topLeft(), thumb_);
  QTransform m = canvas_->session().view.matrix().inverted() *
                 toThumb; // canvas pixels -> thumbnail pixels
  QRectF c = canvas_->rect();
  QPolygonF view = m.map(QPolygonF(c));
  p.setRenderHint(QPainter::Antialiasing);
  QPainterPath outside;
  outside.addRect(r);
  outside.addPolygon(view); // odd-even fill: the view's inside stays clear
  p.fillPath(outside, QColor(0, 0, 0, 115));
  QLineF tick(
    m.map(c.topLeft() + QPointF(c.width() / 4, 0)),
    m.map(
      c.topLeft() +
      QPointF(
        c.width() / 4, 1))); // into the view from its top edge, a quarter in from
                             // the screen's left, so rotation and mirror show
  tick.setLength(6);
  for (auto [pen, w] : {std::pair{QColor(0, 0, 0, 160), 3.0}, {Qt::white, 1.0}})
  { // dark halo under the white line
    p.setPen(QPen(pen, w));
    p.drawPolygon(view);
    p.drawLine(tick);
  }
}

void Navigator::mousePressEvent(QMouseEvent *e)
{
  if (!canvas_->hasDoc())
    return;
  QTransform m = canvas_->session().view.matrix().inverted() * fit();
  dragging_ = m.map(QPolygonF(QRectF(canvas_->rect())))
                .containsPoint(e->position(), Qt::OddEvenFill);
  last_ = e->position();
}

void Navigator::mouseMoveEvent(QMouseEvent *e)
{
  if (!dragging_)
    return;
  QTransform toDoc = fit().inverted();
  View &v = canvas_->session().view;
  v.pan(
    v.toView(toDoc.map(last_)) -
    v.toView(toDoc.map(
      e->position()))); // the dragged doc point lands where the last one was
  last_ = e->position();
  canvas_->update();
}

QTransform Navigator::fit() const
{
  QSizeF ds = canvas_->doc().image(0).size();
  double k = std::min(width() / ds.width(), height() / ds.height());
  return QTransform::fromScale(k, k) *
         QTransform::fromTranslate(
           (width() - ds.width() * k) / 2, (height() - ds.height() * k) / 2);
}
