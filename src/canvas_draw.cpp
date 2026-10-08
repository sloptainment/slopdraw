// Canvas painting: the document, references, and each tool's overlays.
#include "canvas.h"
#include <QPainter>
#include <QPainterPath>

void Canvas::drawInverted(QPainter &p, const QImage &doc, const QPainterPath &path)
{
  QImage mask(size(), QImage::Format_Alpha8);
  mask.fill(0);
  QPainter m(&mask);
  m.setTransform(s_.view.matrix());
  QPen pen(Qt::black, 1);
  pen.setCosmetic(true);
  m.setPen(pen);
  m.drawPath(path);
  m.end();
  QImage out(size(), QImage::Format_ARGB32_Premultiplied);
  out.fill(Qt::transparent);
  QTransform inv = s_.view.matrix().inverted();
  QColor bg = palette().window().color(); // shows through transparent canvas
  for (int y = 0; y < mask.height(); y++)
    for (int x = 0; x < mask.width(); x++)
    {
      int a = mask.constScanLine(y)[x];
      if (!a)
        continue;
      QPointF d = inv.map(QPointF(x + 0.5, y + 0.5));
      QPoint dp(std::floor(d.x()), std::floor(d.y()));
      QColor c = doc.rect().contains(dp) ? doc.pixelColor(dp) : bg;
      double ca = c.alphaF();
      int r = c.red() * ca + bg.red() * (1 - ca),
          g = c.green() * ca + bg.green() * (1 - ca),
          b = c.blue() * ca + bg.blue() * (1 - ca);
      bool mid = std::abs(r - 128) < 40 && std::abs(g - 128) < 40 &&
                 std::abs(b - 128) < 40; // inverse too close to see: go darker
      QColor o = mid ? QColor(0, 0, 0) : QColor(255 - r, 255 - g, 255 - b);
      out.setPixel(x, y, qPremultiply(qRgba(o.red(), o.green(), o.blue(), a)));
    }
  p.save();
  p.resetTransform();
  p.drawImage(0, 0, out);
  p.restore();
}

void Canvas::paintEvent(QPaintEvent *)
{
  updateGroupRect();
  if (boxLayer_ != s_.doc.current())
  {
    box_ = QRect();
    floating_ = false;
    floatM_ = {};
    if (tool_ != ReferenceTool)
      frame_.reset();
  } // a layer switch clears the Box Select rectangle; setCurrent already sealed any
    // floating piece
  flushPreview();
  if (penLayer_ != s_.doc.current())
  {
    pen_.cancel();
    penLayer_ = s_.doc.current();
  } // a layer switch drops the unfinished path
  QString h = hint(); // every tool or state change calls update(), so paint is
                      // where a change of hint is noticed
  if (h != hint_)
  {
    hint_ = h;
    if (onHint)
      onHint(h);
  }
  if (onPaint)
    onPaint();
  QPainter p(this);
  if (!hasDoc())
  {
    p.fillRect(rect(), QColor(90, 90, 90));
    p.setPen(Qt::white);
    p.drawText(rect(), Qt::AlignCenter, "File > New or Open");
    return;
  }
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setTransform(s_.view.matrix());
  QImage doc = s_.doc.render();
  p.drawImage(0, 0, doc);
  drawReferenceOverlays(p);
  if (lasso_.size() > 1)
  {
    QPainterPath lasso;
    lasso.addPolygon(lasso_);
    drawInverted(p, doc, lasso); // the layer is untouched until pen-up
  }
  drawPenPath(p, doc);
  drawNodeHandles(p);
  drawSelectOverlays(p);
  drawBrushOutline(p);
}

void Canvas::drawReferenceOverlays(QPainter &p)
{
  if (cropping_ >= 0)
  { // crop mode: the hidden part as a faint ghost under the references (#548, #550)
    const Reference &r = s_.doc.references()[cropping_];
    p.save();
    p.setTransform(r.transform * s_.view.matrix());
    p.setOpacity(0.22 * r.opacity);
    p.drawImage(0, 0, r.image);
    p.restore();
  }
  if (showRefs_)
    drawReferences(
      p, s_.doc.references(),
      s_.view.matrix()); // above the document image, beyond its edge too (#539)
  if (showRefs_)
    for (int i : refPicks_)
    { // the picked references' outlines, dashed
      const Reference &r = s_.doc.references()[i];
      p.save();
      p.resetTransform();
      p.setPen(QPen(Qt::black, 1, Qt::DashLine));
      p.drawPolygon((r.transform * s_.view.matrix()).map(QPolygonF(r.crop)));
      p.restore();
    }
  if (cropping_ >= 0)
    drawCropFrame(p);
  if (tool_ == ReferenceTool)
  { // the Transform box on the picks, and the box being dragged (#542)
    if (!refPicks_.empty() && cropping_ < 0)
      drawTransformBox(p, frame());
    if (boxing_ && cursor_)
    {
      QPen pen(Qt::blue, 1, Qt::DashLine);
      pen.setCosmetic(true);
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawRect(QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized());
    }
  }
}

void Canvas::drawPenPath(QPainter &p, const QImage &doc)
{
  if (tool_ == SpiroPenTool && !pen_.nodes.empty())
  { // the path so far, plus a node under the pointer that a click would place
    auto nodes = pen_.nodes;
    if (cursor_ && !penDrag_)
      nodes.push_back(
        {s_.view.toDoc(*cursor_).x(), s_.view.toDoc(*cursor_).y(),
         pen_.bezier ? SpiroType::Corner : pen_.type});
    std::optional<QPainterPath> path;
    if (pen_.bezier)
    { // a Bezier path in progress: the dragged handles shape it (#309)
      Document::VPath v{
        {},        pen_.closing,
        Qt::black, 1,
        {},        Document::PathKind::Bezier}; // closed while a press on the
                                                // first node is down (#368)
      v.bezier = pen_.bezierNodes();
      if (nodes.size() > v.bezier.size())
        v.bezier.push_back(
          {{nodes.back().x, nodes.back().y},
           {nodes.back().x, nodes.back().y},
           {nodes.back().x, nodes.back().y},
           BezierType::Cusp});
      path = Document::curve(v);
    }
    else
      path = spiroToPath(nodes, false);
    if (path)
      drawInverted(p, doc, *path);
    QPen pen(Qt::blue, 1);
    pen.setCosmetic(true);
    p.setPen(pen);
    double r = 4 / s_.view.screenRadius(1);
    bool closing =
      cursor_ && pen_.nodes.size() > 2 &&
      pen_.onFirst(s_.view.toDoc(*cursor_), 6 / s_.view.screenRadius(1));
    for (size_t i = 0; i < pen_.nodes.size(); i++)
    { // a square per placed node; the first is filled when a click would close
      // (#180)
      p.setBrush(i == 0 && closing ? Qt::blue : Qt::white);
      p.drawRect(QRectF(pen_.nodes[i].x - r, pen_.nodes[i].y - r, 2 * r, 2 * r));
    }
  }
}

void Canvas::drawNodeHandles(QPainter &p)
{
  auto shown = others_; // the selected path and the others Shift+click added
  if (selected_ >= 0)
    shown[selected_] = nodeSel_;
  if (tool_ == NodeKnotEditTool)
    for (auto &[sp, sel] : shown)
    { // a square per Spiro Node of each, filled when selected, then a circle per
      // Width Knot
      QPen pen(Qt::blue, 1);
      pen.setCosmetic(true);
      p.setPen(pen);
      double r = 4 / s_.view.screenRadius(1);
      auto nodes = s_.doc.nodes(s_.doc.current(), sp);
      auto &path = s_.doc.paths(s_.doc.current())[sp];
      for (int i : sel) // a selected Bezier node's handles: a line out to a circle,
                        // skipped where a handle sits on its point (#313)
        if (i < int(path.bezier.size()))
          for (QPointF h : {path.bezier[i].in, path.bezier[i].out})
            if (h != path.bezier[i].p)
              p.setBrush(Qt::white), p.drawLine(path.bezier[i].p, h),
                p.drawEllipse(h, r, r);
      for (int i = 0; i < int(nodes.size()); i++)
      {
        p.setBrush(sel.count(i) ? Qt::blue : Qt::white);
        p.drawRect(QRectF(nodes[i].x - r, nodes[i].y - r, 2 * r, 2 * r));
      }
    }
  if (tool_ == NodeKnotEditTool)
    for (auto &[sp, sel] : shown)
    { // a handle per Width Knot of each
      QPen pen(Qt::blue, 1);
      pen.setCosmetic(true);
      p.setPen(pen);
      p.setBrush(Qt::white);
      double r = 4 / s_.view.screenRadius(1);
      for (QPointF h : s_.doc.knotHandles(s_.doc.current(), sp))
        p.drawEllipse(h, r, r);
    }
}

void Canvas::drawCropRect(QPainter &p)
{
  double px = 1 / s_.view.screenRadius(1);
  QRectF b(cropRect_);
  QPen pen(Qt::blue, 1);
  pen.setCosmetic(true);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawRect(b);
  p.setBrush(Qt::blue);
  for (double fx : {0.0, 0.5, 1.0})
    for (double fy : {0.0, 0.5, 1.0})
      if (fx != 0.5 || fy != 0.5)
        p.drawRect(QRectF(
          b.left() + fx * b.width() - 4 * px, b.top() + fy * b.height() - 4 * px,
          8 * px, 8 * px));
}

void Canvas::drawSelectOverlays(QPainter &p)
{
  if (tool_ == CropTool && !cropRect_.isEmpty())
    drawCropRect(p);
  if (tool_ == SelectTool)
  { // Select: the picked paths' outlines, and the box being dragged
    QPen pen(Qt::blue, 1);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    for (int i : picked())
      p.drawPath(Document::outline(s_.doc.paths(s_.doc.current())[i]));
    if (!picked().empty() || rasterBox() || groupBox())
      drawTransformBox(p, frame());
    if (boxing_ && cursor_)
    {
      pen.setStyle(Qt::DashLine);
      p.setPen(pen);
      p.drawRect(QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized());
    }
  }
  QRectF marked =
    tool_ == BoxSelectTool && boxing_ && cursor_
      ? QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized()
      : QRectF(box_); // Box Select: the rectangle, or the one being dragged
  if (!marked.isEmpty())
  {
    QPen pen(Qt::blue, 1, Qt::DashLine);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(marked);
  }
}

void Canvas::drawBrushOutline(QPainter &p)
{
  if (cursor_ && brush_ && tool_ == BrushTool)
  { // brush outline: dark with a light line beside it, readable on any color
    p.resetTransform();
    p.setRenderHint(QPainter::Antialiasing);
    double r = s_.view.screenRadius(radius());
    p.setBrush(Qt::NoBrush);
    p.setPen(Qt::white);
    p.drawEllipse(*cursor_, r + 1, r + 1);
    p.setPen(Qt::black);
    p.drawEllipse(*cursor_, r, r);
  }
}

void Canvas::drawCropFrame(QPainter &p)
{
  const Reference &r = s_.doc.references()[cropping_];
  QTransform m = r.transform * s_.view.matrix();
  double px = 1 / std::sqrt(std::abs(m.determinant())); // image px per screen px
  QRectF c = r.crop;
  p.save();
  p.setTransform(m);
  p.setBrush(Qt::NoBrush);
  auto pen = [&p](QColor color, double w, Qt::PenStyle style)
  {
    QPen q(color, w, style);
    q.setCosmetic(true);
    q.setCapStyle(Qt::SquareCap);
    p.setPen(q);
  };
  pen(QColor(0xcc, 0xcc, 0xcc), 1, Qt::DotLine);
  p.drawRect(QRectF(r.image.rect()));
  pen(Qt::white, 1, Qt::SolidLine);
  p.drawRect(c);
  pen(Qt::black, 1, Qt::DashLine);
  p.drawRect(c);
  pen(Qt::white, 4, Qt::SolidLine);
  double L = 16 * px, B = 10 * px;
  for (double sx : {1.0, -1.0})
    for (double sy : {1.0, -1.0})
    {
      QPointF k(sx > 0 ? c.left() : c.right(), sy > 0 ? c.top() : c.bottom());
      p.drawPolyline(
        QPolygonF({k + QPointF(sx * L, 0), k, k + QPointF(0, sy * L)}));
    }
  QPointF mid = c.center();
  p.drawLine(QPointF(mid.x() - B, c.top()), QPointF(mid.x() + B, c.top()));
  p.drawLine(QPointF(mid.x() - B, c.bottom()), QPointF(mid.x() + B, c.bottom()));
  p.drawLine(QPointF(c.left(), mid.y() - B), QPointF(c.left(), mid.y() + B));
  p.drawLine(QPointF(c.right(), mid.y() - B), QPointF(c.right(), mid.y() + B));
  p.restore();
}

void Canvas::drawTransformBox(QPainter &p, const BoxFrame &f)
{
  double px = 1 / s_.view.screenRadius(1);
  QRectF b = f.rect;
  p.save();
  p.setTransform(f.toDoc(), true);
  QPen pen(Qt::blue, 1);
  pen.setCosmetic(true);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawRect(b);
  QPointF top(b.center().x(), b.top()), rot = top - QPointF(0, 24 * px);
  p.drawLine(top, rot);
  p.drawEllipse(rot, 5 * px, 5 * px);
  p.setBrush(Qt::white);
  for (double fx : {0.0, 0.5, 1.0})
    for (double fy : {0.0, 0.5, 1.0})
      if (fx != 0.5 || fy != 0.5)
        p.drawRect(QRectF(
          b.left() + fx * b.width() - 4 * px, b.top() + fy * b.height() - 4 * px,
          8 * px, 8 * px));
  p.restore();
}
