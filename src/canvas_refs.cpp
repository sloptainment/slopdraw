// Canvas input for the Reference tool: picks, transforms, opacity and crop mode.
#include "canvas.h"
#include <QMenu>

std::optional<double> Canvas::refOpacity() const
{
  if (tool_ != ReferenceTool || refPicks_.empty())
    return std::nullopt;
  return s_.doc.references()[*refPicks_.begin()].opacity;
}

void Canvas::setRefOpacity(double o, bool live)
{
  if (!refOpacity())
    return;
  endCrop();
  if (!opacityBefore_)
    opacityBefore_ = s_.doc.references();
  auto refs = *opacityBefore_;
  for (int i : refPicks_)
    refs[i].opacity = o;
  if (live)
    s_.doc.previewReferences(refs);
  else
    s_.doc.previewReferences(*opacityBefore_), s_.doc.setReferences(refs),
      opacityBefore_.reset();
  update();
}

void Canvas::importReference(const QImage &img)
{
  commitFloat();
  endCrop();
  s_.doc.addReference(
    {img, placeReference(img.size(), size(), s_.view.matrix()), QRectF(img.rect()),
     1});
  refPicks_ = {int(s_.doc.references().size()) - 1};
  frame_.reset();
  update();
  layersChanged();
}

bool Canvas::enterCrop(QPointF pos)
{
  int i = referenceAt(s_.doc.references(), s_.view.toDoc(pos));
  if (i < 0 || cropping_ >= 0)
    return false;
  moveDrag_ = boxing_ = false;
  refPicks_ = {i};
  cropping_ = i;
  cropBefore_ = s_.doc.references();
  update();
  return true;
}

void Canvas::refPress(QPointF pos, Qt::KeyboardModifiers mods)
{
  if (cropping_ >= 0)
  { // crop mode: an edge starts a crop drag, the picture keeps the mode, anywhere
    // else leaves it
    cropEdge_ = cropEdgeUnder(pos);
    const Reference &r = s_.doc.references()[cropping_];
    QPointF ip = r.transform.inverted().map(s_.view.toDoc(pos));
    if (!cropEdge_ && r.crop.contains(ip))
      cropGrab_ = ip, cropGrabFrom_ = r.crop; // inside the crop: a drag moves it
    if (cropEdge_ || cropGrab_ || QRectF(r.image.rect()).contains(ip))
      return;
    return endCrop();
  }
  bool shift = mods & Qt::ShiftModifier;
  QPointF d = s_.view.toDoc(pos);
  int i = referenceAt(s_.doc.references(), d);
  auto handle = refPicks_.empty()
                  ? std::nullopt
                  : frameHandleAt(frame(), d, 6 / s_.view.screenRadius(1));
  if (handle && (!handle->x() || !handle->y()))
    handle.reset(); // corners only: an edge would break the aspect (#543)
  bool rotate = !refPicks_.empty() && onRotateHandle(pos);
  if (handle || rotate)
  { // a corner scales about the opposite one, aspect kept; the circle rotates about
    // the centre
    moveFrame_ = frame();
    moveDrag_ = true;
    handle_ = handle;
    rotating_ = rotate;
    moveStart_ = d;
    dragM_ = {};
    refsBefore_ = s_.doc.references();
    return;
  }
  if (
    !shift && !refPicks_.empty() &&
    frame().rect.contains(frame().toDoc().inverted().map(d)) &&
    (i < 0 || refPicks_.count(i)))
  {
    moveFrame_ = frame();
    moveDrag_ = true;
    rotating_ = false;
    handle_.reset();
    moveStart_ = d;
    dragM_ = {};
    refsBefore_ = s_.doc.references();
    return;
  }
  frame_.reset();
  if (!shift)
    refPicks_.clear();
  if (i < 0)
    boxing_ = true, boxStart_ = d;
  else if (!shift)
    refPicks_ = {i};
  else if (!refPicks_.erase(i))
    refPicks_.insert(i);
  update();
}

void Canvas::refMenu(QPoint globalPos)
{
  endCrop();
  if (refPicks_.empty())
    return;
  QMenu menu;
  auto *h = menu.addAction("Flip Horizontal");
  auto *v = menu.addAction("Flip Vertical");
  menu.addSeparator();
  auto *fr = menu.addAction("Bring to Front");
  auto *bk = menu.addAction("Send to Back");
  auto *chosen = menu.exec(globalPos);
  if (chosen == fr || chosen == bk)
  {
    s_.doc.setReferences(
      reorderReferences(s_.doc.references(), refPicks_, chosen == fr));
    int n = refPicks_.size(), total = s_.doc.references().size();
    refPicks_.clear();
    for (int i = 0; i < n; ++i)
      refPicks_.insert(chosen == fr ? total - n + i : i);
    update();
    return;
  }
  if (chosen != h && chosen != v)
    return;
  BoxFrame f = frame();
  QTransform m = flipMatrix(f.rect.center(), chosen == h);
  s_.doc.setReferences(transformReferences(s_.doc.references(), refPicks_, m));
  if (frame_)
    frame_ = frameAfter(f, m);
  update();
}

std::optional<QPoint> Canvas::cropEdgeUnder(QPointF pos) const
{
  const Reference &r = s_.doc.references()[cropping_];
  QTransform m = r.transform * s_.view.matrix();
  return cropEdgeAt(
    r.crop, m.inverted().map(pos), 8 / std::sqrt(std::abs(m.determinant())));
}

void Canvas::cropDragTo(QPointF pos)
{
  auto refs = s_.doc.references();
  Reference &r = refs[cropping_];
  QPointF ip = (r.transform * s_.view.matrix()).inverted().map(pos);
  if (cropGrab_)
  {
    QRectF c = cropGrabFrom_.translated(ip - *cropGrab_);
    r.crop = c.translated(
      std::clamp(c.x(), 0.0, r.image.width() - c.width()) - c.x(),
      std::clamp(c.y(), 0.0, r.image.height() - c.height()) - c.y());
  }
  else
    r.crop = cropTo(r.crop, r.image.size(), *cropEdge_, ip);
  s_.doc.previewReferences(refs);
  update();
}

void Canvas::endCrop()
{
  if (cropping_ < 0)
    return;
  auto refs = s_.doc.references();
  bool changed = refs[cropping_].crop != cropBefore_[cropping_].crop;
  cropping_ = -1;
  cropEdge_.reset();
  cropGrab_.reset();
  if (changed)
    s_.doc.previewReferences(cropBefore_), s_.doc.setReferences(refs);
  unsetCursor();
  update();
  layersChanged();
}

void Canvas::deleteRefPicks()
{
  endCrop();
  if (refPicks_.empty())
    return;
  std::vector<Reference> kept;
  for (int i = 0; i < int(s_.doc.references().size()); i++)
    if (!refPicks_.count(i))
      kept.push_back(s_.doc.references()[i]);
  s_.doc.setReferences(kept);
  refPicks_.clear();
  update();
  layersChanged();
}
