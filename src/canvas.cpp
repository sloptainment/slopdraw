// Canvas: tool switching, the Edit commands, and routing mouse, tablet and key
// input to the tool in use.
#include "canvas.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyleHints>
#include <QTabletEvent>
#include <QWheelEvent>

void Canvas::setColor(const QColor &c)
{
  if (editPicks([&](Document::VPath &p)
                { (fillActive_ ? p.fillColor : p.color) = c; }))
    return;
  if (tool_ == SpiroPenTool || tool_ == SelectTool)
    (fillActive_ ? penFill_ : penStroke_) = c;
  else
  {
    color_ = c;
    if (brush_)
      setBrushColor(brush_.get(), c);
  }
}

void Canvas::setTool(int t)
{
  commitFloat();
  tool_ = Tool(t);
  if (tool_ == NodeKnotEditTool)
  {
    selected_ = -1;
    nodeSel_.clear();
    others_.clear();
  }
  if (tool_ != SpiroPenTool)
    pen_.cancel();
  picked_.clear();
  refPicks_.clear();
  frame_.reset();
  endCrop();
  boxing_ = false;
  unsetCursor();
  update();
  if (onToolChange)
    onToolChange(t);
}

void Canvas::undo(bool redo)
{
  commitFloat(); // so undo takes the whole piece back
  endCrop();     // and the whole crop
  redo ? s_.doc.redo() : s_.doc.undo();
  clearSelection();
  update();
  layersChanged();
}

void Canvas::resize(QSize size, QPoint offset, bool scale)
{
  commitFloat();
  endCrop();
  if (!s_.doc.resize(size, offset, scale))
    return;
  clearSelection();
  update();
  layersChanged();
}

void Canvas::copy(bool cut)
{
  if (
    !hasDoc() || s_.doc.vector(s_.doc.current()) ||
    s_.doc.isGroup(s_.doc.current()))
    return;
  QRect all = s_.doc.image(0).rect();
  QRect r = !rasterBox() ? all : floating_ ? floatM_.mapRect(box_) & all : box_;
  QGuiApplication::clipboard()->setImage(s_.doc.image(s_.doc.current()).copy(r));
  if (!cut)
    return;
  commitFloat();
  s_.doc.clearRect(r);
  update();
  layersChanged();
}

void Canvas::paste(const QImage &img, QPointF at, bool newLayer)
{
  commitFloat();
  endCrop();
  pasted_ = img;
  pasteNew_ = newLayer;
  box_ = QRect(
    QPoint(int(at.x() - img.width() / 2.0), int(at.y() - img.height() / 2.0)),
    img.size());
  floating_ = floatPiece({});
  boxLayer_ = s_.doc.current();
  update();
  layersChanged();
}

void Canvas::setErase(bool on)
{
  if (tool_ != BrushTool && tool_ != LassoFillTool && tool_ != BucketFillTool)
    return;
  if (tool_ == BrushTool && eraseBrush_ != on)
  { // the Brush keeps a Size and Opacity per mode: swap in the other mode's
    float r = radius(), o = opacity();
    setRadius(otherRadius_), setOpacity(otherOpacity_);
    otherRadius_ = r, otherOpacity_ = o;
  }
  if (tool_ == LassoFillTool && eraseFill_ != on)
    std::swap(
      lassoOpacity_, otherLassoOpacity_); // Lasso Fill's Opacity per mode too
  (tool_ == LassoFillTool    ? eraseFill_
   : tool_ == BucketFillTool ? eraseBucket_
                             : eraseBrush_) = on;
}

void Canvas::finishPath(bool closed)
{
  if (floating_ || groupFloat_)
    return endPaste();
  if (cropping_ >= 0)
    return endCrop(); // Enter leaves crop mode (#546)
  if (tool_ == SpiroPenTool)
    pen_.finish(
      s_.doc, closed, penStroke_, pathWidth_, paintMode_,
      penFill_); // refused on a Raster Layer
  update();
  layersChanged();
}

void Canvas::setBrush(const QString &mybPath)
{
  auto b = loadBrush(mybPath);
  if (!b)
    return;
  for (auto s :
       {MYPAINT_BRUSH_SETTING_COLOR_H, MYPAINT_BRUSH_SETTING_COLOR_S,
        MYPAINT_BRUSH_SETTING_COLOR_V, MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC,
        MYPAINT_BRUSH_SETTING_OPAQUE})
    mypaint_brush_set_base_value(
      b.get(), s, mypaint_brush_get_base_value(brush_.get(), s));
  brush_ = b;
}

void Canvas::mousePressEvent(QMouseEvent *e)
{
  dragging_ =
    e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_);
  last_ = e->position();
  resizing_ = e->button() == Qt::LeftButton && !space_ &&
              (tool_ == BrushTool || tool_ == LassoFillTool) &&
              (e->modifiers() & Qt::ShiftModifier);
  if (
    e->button() == Qt::LeftButton && !space_ &&
    (e->modifiers() & Qt::ControlModifier) &&
    !editKnot(e->position(), e->modifiers()))
    pickAt(e->position());
  else if (
    e->button() == Qt::LeftButton && !space_ &&
    (e->modifiers() & Qt::ControlModifier))
    return;
  else if (tool_ == ReferenceTool)
  {
    if (e->button() == Qt::LeftButton && !space_)
      refPress(e->position(), e->modifiers());
    else if (e->button() == Qt::RightButton)
      refMenu(e->globalPosition().toPoint());
  }
  else if (e->button() == Qt::RightButton && tool_ == SpiroPenTool)
    finishPath(false);
  else if (e->button() == Qt::RightButton && tool_ == SelectTool)
    flipMenu(e->globalPosition().toPoint());
  else if (
    e->button() == Qt::LeftButton && !space_ && !resizing_ &&
    tool_ == NodeKnotEditTool)
    selectPress(e->position(), e->modifiers());
  else if (e->button() == Qt::LeftButton && !space_ && tool_ == SelectTool)
    objectPress(e->position(), e->modifiers());
  else if (e->button() == Qt::LeftButton && !space_ && tool_ == BoxSelectTool)
    boxPress(e->position());
  else if (
    e->button() == Qt::LeftButton && !space_ && !resizing_ && tool_ == SpiroPenTool)
    spiroClick(e->position(), e->modifiers() & Qt::ShiftModifier);
  else if (
    e->button() == Qt::LeftButton && !space_ && !resizing_ &&
    tool_ == LassoFillTool)
    lassoAt(e->position(), erase());
  else if (tool_ == BucketFillTool)
  {
    if (e->button() == Qt::LeftButton && !space_)
      bucketAt(e->position());
  }
  else if (
    e->button() == Qt::LeftButton && !space_ && !resizing_ && tool_ != SelectTool &&
    tool_ != BoxSelectTool)
    paintAt(e->position(), 0.5f, 0, 0, e->timestamp(), erase());
}

void Canvas::mouseMoveEvent(QMouseEvent *e)
{
  if (!resizing_)
    cursor_ =
      e->position(); // the circle stays at the press point while Shift+drag resizes
  update();
  if (resizing_)
    return resizeTo(e->position());
  if (knotDrag_ >= 0)
    return dragKnotTo(e->position());
  if (nodeDrag_)
    return dragNodesTo(e->position());
  if (handleDrag_ >= 0)
    return dragHandleTo(e->position());
  if (cropEdge_ || cropGrab_)
    return cropDragTo(e->position());
  if (moveDrag_)
    return dragMoveTo(e->position(), e->modifiers() & Qt::ShiftModifier);
  if (penDrag_)
    return pen_.drag(s_.view.toDoc(e->position()));
  if (boxing_)
    return;
  if (tool_ == SelectTool || tool_ == ReferenceTool)
    setCursor(
      !(tool_ == ReferenceTool ? refPicks_.empty() : picked().empty()) &&
          onRotateHandle(e->position())
        ? Qt::OpenHandCursor
        : Qt::ArrowCursor);
  if (cropping_ >= 0)
  { // crop mode: a crosshair, resize arrows over an edge or corner (#548)
    auto h = cropEdgeUnder(e->position());
    setCursor(
      !h                 ? Qt::CrossCursor
      : !h->x()          ? Qt::SizeVerCursor
      : !h->y()          ? Qt::SizeHorCursor
      : h->x() == h->y() ? Qt::SizeFDiagCursor
                         : Qt::SizeBDiagCursor);
  }
  if (lassoing_)
    return lassoAt(e->position(), erase());
  if (painting_)
    return paintAt(e->position(), 0.5f, 0, 0, e->timestamp(), erase());
  if (!dragging_)
    return;
  panZoom(e->position(), e->modifiers());
}

void Canvas::panZoom(QPointF pos, Qt::KeyboardModifiers mods)
{
  QPointF d = pos - last_;
  last_ = pos;
  if (mods & Qt::ControlModifier)
    s_.view.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
  else if (mods & Qt::ShiftModifier)
    s_.view.rotateAt(rect().center(), d.x() * 0.5);
  else
    s_.view.pan(d);
  update();
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e)
{
  if (!(tool_ == ReferenceTool && e->button() == Qt::LeftButton && !space_ &&
        enterCrop(e->position())))
    QWidget::mouseDoubleClickEvent(e);
}

void Canvas::tabletEvent(QTabletEvent *e)
{
  if (space_)
    return e->ignore(); // Qt turns it into mouse events, so Space+pen drag pans
  e->accept();
  if (e->type() == QEvent::TabletPress)
    setFocus(); // an accepted tablet press makes no mouse press, so nothing else
                // gives the canvas the keys (Esc)
  if (e->type() == QEvent::TabletLeaveProximity)
  {
    cursor_.reset();
    update();
  }
  else if (!resizing_ || e->type() == QEvent::TabletPress)
  {
    cursor_ = e->position();
    update();
  }
  if (e->button() == Qt::MiddleButton && e->type() != QEvent::TabletMove)
  {
    dragging_ = e->type() == QEvent::TabletPress;
    last_ = e->position();
    return;
  } // barrel button pans like the mouse's middle button
  if (dragging_)
    return panZoom(e->position(), e->modifiers());
  if (e->type() == QEvent::TabletRelease)
    release();
  else if (
    e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ControlModifier))
  {
    if (!editKnot(e->position(), e->modifiers()))
      pickAt(e->position());
  }
  else if (
    e->type() == QEvent::TabletPress &&
    (tool_ == BrushTool || tool_ == LassoFillTool) &&
    (e->modifiers() & Qt::ShiftModifier))
  {
    resizing_ = true;
    last_ = e->position();
  }
  else if (resizing_)
    resizeTo(e->position());
  else
    tabletTool(e);
}

void Canvas::refTablet(QTabletEvent *e)
{
  if (e->type() == QEvent::TabletPress)
  {
    bool dbl = e->timestamp() - lastTabletPress_ <=
                 ulong(QGuiApplication::styleHints()->mouseDoubleClickInterval()) &&
               (e->position() - lastTabletPos_).manhattanLength() <=
                 4 * QGuiApplication::styleHints()->startDragDistance();
    lastTabletPress_ = e->timestamp();
    lastTabletPos_ = e->position();
    if (!(dbl && enterCrop(e->position())))
      refPress(e->position(), e->modifiers());
  }
  else if (cropEdge_ || cropGrab_)
    cropDragTo(e->position());
  else if (moveDrag_)
    dragMoveTo(e->position(), e->modifiers() & Qt::ShiftModifier);
}

void Canvas::tabletTool(QTabletEvent *e)
{
  bool eraser = e->pointerType() == QPointingDevice::PointerType::Eraser;
  if (tool_ == ReferenceTool)
    refTablet(e);
  else if (tool_ == NodeKnotEditTool)
  {
    if (e->type() == QEvent::TabletPress)
      selectPress(e->position(), e->modifiers());
    else if (knotDrag_ >= 0)
      dragKnotTo(e->position());
    else if (nodeDrag_)
      dragNodesTo(e->position());
    else if (handleDrag_ >= 0)
      dragHandleTo(e->position());
  }
  else if (tool_ == SelectTool)
  {
    if (e->type() == QEvent::TabletPress)
      objectPress(e->position(), e->modifiers());
    else if (moveDrag_)
      dragMoveTo(e->position(), e->modifiers() & Qt::ShiftModifier);
  }
  else if (tool_ == BoxSelectTool)
  {
    if (e->type() == QEvent::TabletPress)
      boxPress(e->position());
  }
  else if (tool_ == SpiroPenTool)
  {
    if (e->type() == QEvent::TabletPress)
    {
      if (e->button() == Qt::RightButton)
        finishPath(false);
      else
        spiroClick(e->position(), e->modifiers() & Qt::ShiftModifier);
    }
    else if (penDrag_)
      pen_.drag(s_.view.toDoc(e->position()));
  } // the barrel button arrives as right (#178)
  else if (tool_ == BucketFillTool)
  {
    if (e->type() == QEvent::TabletPress)
      bucketAt(e->position());
  }
  else if (
    tool_ == LassoFillTool && (e->type() == QEvent::TabletPress || lassoing_))
    lassoAt(e->position(), erase() || eraser);
  else if (
    tool_ != SelectTool && tool_ != BoxSelectTool &&
    (e->pointerType() == QPointingDevice::PointerType::Pen ||
     e->pointerType() == QPointingDevice::PointerType::Eraser) &&
    (e->type() == QEvent::TabletPress || painting_))
    paintAt(
      e->position(), e->pressure(), e->xTilt() / 60, e->yTilt() / 60,
      e->timestamp(),     // libmypaint tilt is -1..1 for +-60 degrees
      erase() || eraser); // the flag is per call (b20)
}

void Canvas::wheelEvent(QWheelEvent *e)
{
  s_.view.zoomAt(e->position(), std::pow(1.1, e->angleDelta().y() / 120.0));
  update();
}

void Canvas::keyPressEvent(QKeyEvent *e)
{
  if (e->isAutoRepeat())
    return;
  if (e->key() == Qt::Key_Space)
    space_ = true;
  else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
  {
    if (tool_ == NodeKnotEditTool)
      deleteSelectedNodes();
    else if (tool_ == SelectTool)
      deletePicked();
    else if (tool_ == ReferenceTool)
      deleteRefPicks();
    else if (tool_ == BoxSelectTool)
      deleteBox();
    else
      pen_.backspace();
    update();
  }
  else if (e->key() == Qt::Key_Escape)
  {
    pen_.cancel();
    cancelFloat();
    box_ = QRect();
    update();
  }
}

void Canvas::keyReleaseEvent(QKeyEvent *e)
{
  if (!e->isAutoRepeat() && e->key() == Qt::Key_Space)
    space_ = false;
}

QString Canvas::penHint() const
{
  if (pen_.nodes.empty())
    return "Click places the first node";
  if (pen_.bezier)
    return "Click adds a corner · click-drag adds a smooth node · Enter or "
           "right-click finishes open · click the first node or Shift+Enter closes "
           "· Backspace drops the last node · Esc cancels";
  return "Click adds a node · Shift+click adds a corner · Enter or right-click "
         "finishes open · click the first node or Shift+Enter closes · Backspace "
         "drops the last node · Esc cancels";
}

QString Canvas::hint() const
{
  if (!hasDoc())
    return {};
  if (tool_ == SpiroPenTool)
    return penHint();
  if (tool_ == NodeKnotEditTool && selected_ < 0)
    return "Click a path to select it";
  if (tool_ == NodeKnotEditTool)
    return "Click or Shift+click nodes · drag nodes · Delete removes them · drag a "
           "knot · Ctrl+click a knot duplicates · Ctrl+click the path adds a knot "
           "· Ctrl+Shift+click the path adds a node · Ctrl+Alt+click deletes";
  if (tool_ == SelectTool && groupBox())
    return "Drag inside the box moves the group · handles scale · circle rotates · "
           "right-click flips · Enter or click outside commits · Esc puts it back";
  if (tool_ == SelectTool && rasterBox())
    return "Drag inside the box moves the pixels · handles scale · circle rotates "
           "· right-click flips · Enter or click outside commits · Esc puts them "
           "back";
  if (tool_ == SelectTool && !s_.doc.vector(s_.doc.current()))
    return "Make a rectangle with Box Select first, then Select transforms its "
           "pixels";
  if (cropping_ >= 0)
    return "Drag an edge or corner to crop, inside to move the crop · the whole "
           "picture is kept · Enter or click outside finishes";
  if (tool_ == ReferenceTool && refPicks_.empty())
    return "Click a reference to pick it · Shift+click toggles · drag a box on "
           "empty canvas picks the ones it touches · File > Import Reference Image "
           "adds a picture";
  if (tool_ == ReferenceTool)
    return QString("%1 picked · drag inside the box moves them · a corner scales · "
                   "the circle rotates · right-click flips · Delete removes them · "
                   "click or drag a box to pick again · Shift adds")
      .arg(refPicks_.size());
  if (tool_ == BoxSelectTool && box_.isEmpty())
    return "Drag a rectangle to select pixels";
  if (tool_ == BoxSelectTool)
    return "Delete clears the pixels · drag a new rectangle to replace it · click "
           "outside or Esc clears the selection";
  if (tool_ == SelectTool && picked().empty())
    return "Click a path to pick it · Shift+click toggles · drag a box on empty "
           "canvas picks the paths it touches";
  if (tool_ == SelectTool)
    return QString("%1 picked · drag inside the box moves them · drag a handle "
                   "scales (Shift keeps the ratio on a corner) · the circle rotates "
                   "· right-click Flips or Converts to Bezier · Delete removes "
                   "them · click or drag a box to pick again · Shift adds")
      .arg(picked().size());
  if (tool_ == BucketFillTool)
    return "Click fills the connected region within Threshold · Ctrl+click picks a "
           "color · E toggles erase";
  return "Draw · Shift+drag sizes the brush · Ctrl+click picks a color · E toggles "
         "erase";
}

void Canvas::spiroClick(QPointF pos, bool corner)
{
  QPointF d = s_.view.toDoc(pos);
  if (pen_.nodes.size() > 2 && pen_.onFirst(d, 6 / s_.view.screenRadius(1)))
  {
    if (pen_.bezier)
      pen_.closing = penDrag_ = true;
    else
      finishPath(true);
  }
  else
    pen_.click(d, corner), penDrag_ = pen_.bezier;
  update();
}

void Canvas::flushPreview()
{
  if (auto f = std::exchange(preview_, nullptr))
    f();
}
