#include "colorpicker.h"
#include <algorithm>
#include <cmath>
#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>

void ColorPicker::setColor(const QColor &c)
{
  if (c.hsvHueF() >= 0)
    h_ = c.hsvHueF(); // grays have no hue: keep the last one
  s_ = c.hsvSaturationF();
  v_ = c.valueF();
  update();
}

void ColorPicker::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QConicalGradient g(center(), 0);
  for (int i = 0; i <= 6; i++)
    g.setColorAt(
      i / 6.0,
      QColor::fromHsvF(
        i % 6 / 6.0, 1,
        1)); // conical angles run counter-clockwise, like the hue math below
  p.setPen(QPen(QBrush(g), ring()));
  p.drawEllipse(center(), outer() - ring() / 2, outer() - ring() / 2);
  QImage sq(kN, kN, QImage::Format_RGB32);
  for (int y = 0; y < kN; y++)
    for (int x = 0; x < kN; x++)
      sq.setPixelColor(
        x, y, QColor::fromHsvF(h_, x / double(kN - 1), 1 - y / double(kN - 1)));
  p.drawImage(square(), sq);
  QPointF hue =
    center() + (outer() - ring() / 2) *
                 QPointF(std::cos(h_ * 2 * M_PI), -std::sin(h_ * 2 * M_PI));
  QPointF sv = square().topLeft() +
               QPointF(s_ * square().width(), (1 - v_) * square().height());
  for (QPointF m : {hue, sv})
  { // white ring on black, visible on any color
    p.setPen(QPen(Qt::black, 3));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(m, 5, 5);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(m, 5, 5);
  }
}

void ColorPicker::mousePressEvent(QMouseEvent *e)
{
  QPointF d = e->position() - center();
  double r = std::hypot(d.x(), d.y());
  mode_ = r >= outer() - ring() && r <= outer() ? Ring
          : square().contains(e->position())    ? Square
                                                : None;
  mouseMoveEvent(e);
}

void ColorPicker::mouseMoveEvent(QMouseEvent *e)
{
  QPointF d = e->position() - center();
  if (mode_ == Ring)
    h_ = std::fmod(std::atan2(-d.y(), d.x()) / (2 * M_PI) + 1, 1.0);
  else if (mode_ == Square)
  {
    s_ = std::clamp(
      (e->position().x() - square().left()) / square().width(), 0.0, 1.0);
    v_ = 1 - std::clamp(
               (e->position().y() - square().top()) / square().height(), 0.0, 1.0);
  }
  else
    return;
  update();
  if (onChange)
    onChange(QColor::fromHsvF(h_, s_, v_));
}

QRectF ColorPicker::square() const
{
  double half = (outer() - ring() - 4) / std::sqrt(2.0);
  return {center().x() - half, center().y() - half, 2 * half, 2 * half};
}
