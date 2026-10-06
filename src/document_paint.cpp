// Painting on the current layer: brush strokes, Lasso Fill, Bucket Fill.
#include "document.h"
#include <algorithm>
#include <QPainter>
#include <QPainterPath>

void Document::stroke(
  const std::shared_ptr<MyPaintBrush> &brush,
  const std::vector<PenSample> &samples,
  bool continuing,
  bool eraser)
{
  if (!continuing || strokeStart_.isNull())
  {
    seal();
    history_.resize(applied_); // a new edit discards the redo branch
    strokeStart_ = layers_[current_].image;
  }
  paintStroke(
    brush.get(), layers_[current_].image, samples, continuing, eraser,
    layers_[current_].alphaLock);
}

void Document::fill(
  const QColor &color, const std::vector<QPointF> &points, bool eraser)
{
  seal();
  history_.resize(applied_); // a new edit discards the redo branch
  strokeStart_ = layers_[current_].image;
  QPainterPath path;
  path.setFillRule(Qt::WindingFill);
  path.addPolygon(QPolygonF(QVector<QPointF>(points.begin(), points.end())));
  path.closeSubpath();
  QPainter p(&layers_[current_].image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  if (layers_[current_].alphaLock)
    p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
  else if (eraser)
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  p.fillPath(path, eraser ? QColor(255, 255, 255, color.alpha()) : color);
  p.end();
  seal(); // its own step, so a live stroke after it starts fresh
}

// The coverage grown (expand > 0) or shrunk by |expand| whole pixels, 4-neighbour,
// one pixel per pass: dilate sets any neighbour of the region to full coverage,
// erode drops region pixels that touch outside it (#605).
static std::vector<float>
expandCover(std::vector<float> cover, int w, int h, int expand)
{
  auto at_ = [w](int x, int y) { return size_t(y) * w + x; };
  for (int i = 0; i < std::abs(expand); ++i)
  {
    std::vector<float> next = cover;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (QPoint d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)})
        {
          int nx = x + d.x(), ny = y + d.y();
          if (nx < 0 || ny < 0 || nx >= w || ny >= h)
            continue; // the document edge neither grows nor erodes
          if (expand > 0 && cover[at_(nx, ny)] > 0)
            next[at_(x, y)] = std::max(next[at_(x, y)], 1.f);
          if (expand < 0 && cover[at_(x, y)] > 0 && cover[at_(nx, ny)] == 0)
            next[at_(x, y)] = 0;
        }
    cover = next;
  }
  return cover;
}

// The region flooded 4-neighbour from at over pixels within threshold of its
// colour (largest channel difference), as per-pixel coverage: 0 outside, 1 inside,
// or with antialiased a ramp down towards threshold (#605).
static std::vector<float>
floodCover(const QImage &src, QPoint at, int threshold, bool antialiased)
{
  const QRgb target = src.pixel(at);
  auto diff = [&](int x, int y)
  {
    QRgb c = src.pixel(x, y);
    return std::max(
      {std::abs(qRed(c) - qRed(target)), std::abs(qGreen(c) - qGreen(target)),
       std::abs(qBlue(c) - qBlue(target)), std::abs(qAlpha(c) - qAlpha(target))});
  };
  const int w = src.width(), h = src.height();
  auto at_ = [w](int x, int y) { return size_t(y) * w + x; };
  std::vector<float> cover(size_t(w) * h); // 0 outside the region
  std::vector<bool> seen(cover.size());
  auto visit = [&](QPoint q)
  {
    seen[at_(q.x(), q.y())] = true;
    cover[at_(q.x(), q.y())] =
      antialiased
        ? 1.f - 0.5f * diff(q.x(), q.y()) / std::max(threshold, 1)
        : 1.f; // ponytail: GIMP's ramp halved so halfway to Threshold is partial
  };
  std::vector<QPoint> todo = {at};
  visit(at);
  while (!todo.empty())
  {
    QPoint p = todo.back();
    todo.pop_back();
    for (QPoint q :
         {p + QPoint(1, 0), p + QPoint(-1, 0), p + QPoint(0, 1), p + QPoint(0, -1)})
      if (
        src.rect().contains(q) && !seen[at_(q.x(), q.y())] &&
        diff(q.x(), q.y()) <= threshold)
      {
        visit(q);
        todo.push_back(q);
      }
  }
  return cover;
}

bool Document::bucketFill(
  const QPoint &at,
  const QColor &color,
  int threshold,
  int expand,
  bool antialiased,
  bool sampleMerged,
  bool eraser)
{
  Layer &l = layers_[current_];
  if (l.vector || l.group || !l.image.rect().contains(at))
    return false;
  seal();
  history_.resize(applied_); // a new edit discards the redo branch
  strokeStart_ = l.image;
  const QImage src =
    (sampleMerged ? render() : l.image)
      .convertToFormat(
        QImage::Format_ARGB32); // render() skips hidden layers and references
  const int w = src.width(), h = src.height();
  auto at_ = [&](int x, int y) { return size_t(y) * w + x; };
  std::vector<float> cover =
    expandCover(floodCover(src, at, threshold, antialiased), w, h, expand);
  QImage mask(src.size(), QImage::Format_ARGB32_Premultiplied);
  mask.fill(Qt::transparent);
  QColor paint = eraser ? QColor(255, 255, 255, color.alpha()) : color;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (cover[at_(x, y)] > 0)
      {
        QColor c = paint;
        c.setAlphaF(paint.alphaF() * cover[at_(x, y)]);
        mask.setPixel(x, y, qPremultiply(c.rgba()));
      }
  QPainter p(&l.image);
  if (l.alphaLock)
    p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
  else if (eraser)
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  p.drawImage(0, 0, mask);
  p.end();
  seal(); // its own step, so a live stroke after it starts fresh
  return true;
}
