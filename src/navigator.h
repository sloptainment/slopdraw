#pragma once
#include <QImage>
#include <QWidget>

class Canvas;

// Navigation dock (#286): the composite render scaled to fit, outside the view
// dimmed, the view a white haloed outline turned and flipped as the view is (#322).
class Navigator : public QWidget
{
public:
  explicit Navigator(Canvas *c) : canvas_(c) { setMinimumSize(160, 120); }
  void refresh()
  {
    thumb_ = {};
    update();
  } // the drawing changed; the rectangle alone needs only update()

protected:
  void resizeEvent(QResizeEvent *) override { thumb_ = {}; }
  void paintEvent(QPaintEvent *) override;

  // Dragging inside the view rectangle pans the canvas by the same document
  // distance (#287).
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *) override { dragging_ = false; }

private:
  QTransform
  fit() const; // document pixels -> thumbnail pixels: scaled to fit, centred
  Canvas *canvas_;
  bool dragging_ = false;
  QPointF last_;
  QImage
    thumb_; // the render at thumbnail size; null until the next paint rebuilds it
};
