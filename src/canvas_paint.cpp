// Canvas input for the Brush, Lasso Fill and Bucket Fill, the Stabilizer, brush
// resize and color pick.
#include "canvas.h"
#include <QTimer>

void Canvas::resizeTo(QPointF pos)
{
  if (!brush_)
    return;
  setRadius(dragRadius(radius(), pos.x() - last_.x()));
  last_ = pos;
  if (onRadiusChange)
    onRadiusChange();
}

void Canvas::pickAt(QPointF pos)
{
  QPointF d = s_.view.toDoc(pos);
  QColor c = pickColorAt(s_.doc, d, showRefs_);
  if (!c.isValid() || !brush_ || tool_ == NodeKnotEditTool)
    return; // Node/Knot edit: a Ctrl+click no knot or node gesture took does
            // nothing
  setColor(c);
  if (onColorPick)
    onColorPick(c);
}

void Canvas::lassoAt(QPointF pos, bool eraser)
{
  if (!lassoing_)
    lassoErase_ = eraser, lassoSmooth_.q.clear();
  lassoing_ = true;
  QPointF d = s_.view.toDoc(pos);
  // ponytail: smoothed per pen event, no timer like the Brush's; the outline waits
  // while the pen holds still
  lassoSmooth_.push({float(d.x()), float(d.y()), 1, 0, 0, 0.0});
  auto m = lassoSmooth_.sample();
  lasso_.push_back({m.x, m.y});
  update();
}

void Canvas::bucketAt(QPointF pos)
{
  QPointF d = s_.view.toDoc(pos);
  QColor c = color_;
  c.setAlphaF(bucketOpacity_);
  s_.doc.bucketFill(
    {int(std::floor(d.x())), int(std::floor(d.y()))}, c, bucketThreshold_,
    bucketExpand_, bucketAA_, bucketMerged_, eraseBucket_);
  update();
}

void Canvas::endLasso()
{
  if (!lassoing_)
    return;
  lassoing_ = false;
  if (lassoFinish_)
    for (auto &m : lassoSmooth_.finish())
      lasso_.push_back({m.x, m.y}); // the shape closes at the pen-up point
  QColor c = color_;
  c.setAlphaF(lassoOpacity_);
  if (lasso_.size() > 2)
    s_.doc.fill(c, {lasso_.begin(), lasso_.end()}, lassoErase_);
  lasso_.clear();
  update();
}

void Canvas::paintAt(
  QPointF pos, float pressure, float xtilt, float ytilt, ulong ms, bool eraser)
{
  if (!brush_)
    return;
  QPointF d = s_.view.toDoc(pos);
  if (smooth_.n > 0)
  {
    // Smoothing: each pen event paints the mean; smoothTick repeats raw_ every 10
    // ms while the pen holds still, so the line catches up (/r/14).
    raw_ = {float(d.x()), float(d.y()), pressure,
            xtilt,        ytilt,        painting_ ? (ms - lastMs_) / 1000.0 : 0.0};
    smoothErase_ = eraser;
    lastMs_ = ms;
    if (painting_)
    {
      // Every pen event is smoothed and painted; the timer only fires once the pen
      // holds still for 10 ms.
      smooth_.push(raw_);
      s_.doc.stroke(brush_, {smooth_.sample()}, true, eraser);
      smoothTimer_->start();
      return update();
    }
    smooth_.q.clear();
    smooth_.push(raw_);
    s_.doc.stroke(brush_, {smooth_.sample()}, false, eraser); // no dead zone (#246)
    painting_ = true;
    if (!smoothTimer_)
    {
      smoothTimer_ = new QTimer(this);
      smoothTimer_->setInterval(10);
      QObject::connect(smoothTimer_, &QTimer::timeout, [this] { smoothTick(); });
    }
    smoothTimer_->start();
    return update();
  }
  s_.doc.stroke(
    brush_,
    {{float(d.x()), float(d.y()), pressure, xtilt, ytilt,
      painting_ ? (ms - lastMs_) / 1000.0 : 0.0}},
    painting_, eraser);
  painting_ = true;
  lastMs_ = ms;
  update();
}

void Canvas::endSmooth()
{
  if (!smoothTimer_ || !smoothTimer_->isActive())
    return;
  smoothTimer_->stop();
  if (!finishLine_)
    return;
  auto tail = smooth_.finish();
  if (!tail.empty())
    s_.doc.stroke(brush_, tail, true, smoothErase_);
  update();
}

void Canvas::smoothTick()
{
  raw_.dt = 0.01;
  smooth_.push(raw_);
  s_.doc.stroke(brush_, {smooth_.sample()}, true, smoothErase_);
  update();
}
