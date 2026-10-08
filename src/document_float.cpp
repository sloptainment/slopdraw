// Floating pixels: lifting, transforming and committing a raster piece, a group or
// a paste.
#include "document.h"
#include <QPainter>
#include "transform.h"

bool Document::clearRect(const QRect &rect)
{
  if (layers_[current_].vector)
    return false;
  seal();
  history_.resize(applied_); // a new edit discards the redo branch
  strokeStart_ = layers_[current_].image;
  QPainter p(&layers_[current_].image);
  p.setCompositionMode(QPainter::CompositionMode_Source);
  p.fillRect(rect, Qt::transparent);
  p.end();
  seal(); // its own step; a rect that changed nothing pushes none
  return true;
}

bool Document::resize(QSize size, QPoint offset)
{
  if (size.width() < 1 || size.height() < 1)
    return false;
  if (size == layers_[0].image.size() && offset.isNull())
    return false;
  seal();
  auto v = layers_;
  for (Layer &l : v)
  {
    if (l.group)
      continue;
    QImage img(size, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(offset, l.image);
    p.end();
    l.image = img;
    for (VPath &path : l.paths)
      path = transformNodes(path, QTransform::fromTranslate(offset.x(), offset.y()));
    if (l.vector)
      rasterize(l);
  }
  Step s;
  s.kind = Tree;
  s.items = layers_;
  s.newItems = v;
  s.to = s.layer = current_;
  push(s);
  apply(s, false);
  return true;
}

bool Document::floatRect(const QRect &rect, const QTransform &m)
{
  if (layers_[current_].vector)
    return false;
  if (strokeStart_.isNull())
  {
    seal();
    history_.resize(applied_); // a new edit discards the redo branch
    strokeStart_ = layers_[current_].image;
  }
  QImage lifted = strokeStart_.copy(rect);
  layers_[current_].image = strokeStart_;
  QPainter p(&layers_[current_].image);
  p.setCompositionMode(QPainter::CompositionMode_Source);
  p.fillRect(rect, current_ == 0 ? QColor(Qt::white) : QColor(Qt::transparent));
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setTransform(m);
  p.drawImage(rect.topLeft(), lifted);
  return true;
}

bool Document::transformRect(const QRect &rect, const QTransform &m)
{
  if (!floatRect(rect, m))
    return false;
  seal();
  return true;
}

bool Document::floatGroup(int group, const QTransform &m, bool scaleWidths)
{
  if (
    group < 1 || group >= int(layers_.size()) || !layers_[group].group ||
    blockStart(group) == group)
    return false;
  if (groupStart_.empty())
  {
    seal();
    history_.resize(applied_); // a new edit discards the redo branch
    groupStart_ = layers_;
    pasting_ = true; // not a step yet: stepId must not seal it
  }
  layers_ = groupStart_;
  for (int i = blockStart(group); i < group; i++)
  {
    Layer &l = layers_[i];
    if (l.group)
      continue;
    if (l.vector)
    {
      for (VPath &v : l.paths)
        v = transformNodes(v, m, scaleWidths);
      rasterize(l);
    }
    else
    {
      QImage out(l.image.size(), l.image.format());
      out.fill(Qt::transparent);
      QPainter p(&out);
      p.setRenderHint(QPainter::SmoothPixmapTransform);
      p.setTransform(m);
      p.drawImage(0, 0, l.image);
      l.image = out;
    }
  }
  return true;
}

bool Document::transformGroup(int group, const QTransform &m, bool scaleWidths)
{
  if (!floatGroup(group, m, scaleWidths))
    return false;
  seal();
  return true;
}

QRectF Document::groupBounds(int group) const
{
  QRectF box;
  if (group < 1 || group >= int(layers_.size()) || !layers_[group].group)
    return box;
  for (int i = blockStart(group); i < group; i++)
  {
    const Layer &l = layers_[i];
    if (l.group)
      continue;
    if (l.vector)
    {
      for (const VPath &v : l.paths)
        box |= outline(v).boundingRect();
      continue;
    }
    QRect r;
    for (int y = 0; y < l.image.height(); y++)
    {
      auto *p = reinterpret_cast<const QRgb *>(l.image.constScanLine(y));
      for (int x = 0; x < l.image.width(); x++)
        if (qAlpha(p[x]))
          r |= QRect(x, y, 1, 1);
    }
    box |= QRectF(r);
  }
  return box;
}

void Document::cancelFloat()
{
  if (!groupStart_.empty())
  {
    layers_ = groupStart_;
    groupStart_.clear();
    pasting_ = false;
    return;
  }
  if (strokeStart_.isNull())
    return;
  layers_[current_].image = strokeStart_;
  strokeStart_ = QImage();
  pasting_ = false;
  if (floatFrom_ < 0)
    return;
  layers_.erase(layers_.begin() + current_);
  current_ = floatFrom_;
  floatFrom_ = -1;
}

bool Document::floatImage(const QImage &img, const QTransform &m, bool newLayer)
{
  if (!pasting_)
  {
    if (!newLayer && (layers_[current_].vector || layers_[current_].group))
      return false;
    seal(); // a stroke still in progress is its own step
    pasting_ = true;
    history_.resize(applied_); // a new edit discards the redo branch
    if (newLayer)
    {
      floatFrom_ = current_;
      Layer l{
        true, 1, QImage(layers_[0].image.size(), QImage::Format_RGBA8888),
        "Layer " + QString::number(layers_.size())};
      l.image.fill(Qt::transparent);
      l.depth = layers_[current_].depth;
      layers_.insert(
        layers_.begin() + ++current_,
        std::move(l)); // directly above the current item, as addLayer does
    }
    strokeStart_ = layers_[current_].image;
  }
  layers_[current_].image = strokeStart_;
  QPainter p(&layers_[current_].image);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setTransform(m);
  p.drawImage(0, 0, img);
  return true;
}

bool Document::pasteImage(const QImage &img, const QTransform &m, bool newLayer)
{
  if (!floatImage(img, m, newLayer))
    return false;
  seal();
  return true;
}
