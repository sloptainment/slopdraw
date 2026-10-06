#pragma once
#include <functional>
#include <QWidget>

// Hue ring around a saturation/value square. Dragging either calls onChange with
// the new color; setColor moves it from outside.
class ColorPicker : public QWidget
{
public:
  std::function<void(const QColor &)> onChange;
  std::function<void()> onRelease; // a drag ended
  ColorPicker() { setMinimumSize(180, 180); }
  void setColor(const QColor &c);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *) override
  {
    if (mode_ != None && onRelease)
      onRelease();
    mode_ = None;
  }

private:
  static constexpr int kN =
    64; // resolution of the square's image, scaled up to fit
  enum Mode
  {
    None,
    Ring,
    Square
  } mode_ = None;
  double outer() const
  {
    return std::min(width(), height()) / 2.0 - 2;
  } // the wheel fills the widget, whatever its size
  double ring() const { return outer() * 16 / 88; }
  QPointF center() const { return {width() / 2.0, height() / 2.0}; }
  QRectF square() const; // the square inscribed in the ring's inner circle
  double h_ = 0, s_ = 0, v_ = 0;
};
