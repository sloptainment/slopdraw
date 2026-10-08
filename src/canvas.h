#pragma once
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <QWidget>
#include "brush.h"
#include "document.h"
#include "references.h"
#include "spiropen.h"
#include "stabilizer.h"
#include "tools.h"
#include "transform.h"
#include "view.h"

class QTimer;

// Krita Default navigation (/r/5): middle drag or Space+left drag pans; +Ctrl
// zooms, +Shift rotates; wheel zooms; M mirrors; Ctrl+Z undoes, Ctrl+Shift+Z
// redoes. Shift+left drag (mouse or pen) resizes the brush instead of painting: 100
// px right = x e. Ctrl+left click (mouse or pen) picks the visible color at that
// point instead of painting. Painting: the pen tip, or plain left button at
// pressure 0.5, feeds pen samples to the Document. One open document as the Canvas
// works on it: the Document, its view, its file and its saved mark (#279).
struct Session
{
  Document doc{QSize()};
  View view;
  QString path;     // the document's file, empty until opened or first saved (#277)
  int saved = 0;    // Document::stepId() at the last open or save (#240)
  int untitled = 0; // N of "Untitled N", the tab label until the first save (#282)
};

class Canvas : public QWidget
{
public:
  Canvas()
  {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
  }
  void setDocument(Document doc, QString path = {})
  {
    Session s{std::move(doc), View(), path, 0};
    s.saved = s.doc.stepId();
    setSession(std::move(s));
  }
  void setSession(Session s)
  {
    cropping_ = -1;
    s_ = std::move(s);
    floating_ = false;
    groupFloat_ = false;
    groupKeyLayer_ = -1;
    pasted_ = QImage();
    floatM_ = {};
    frame_.reset();
    clearSelection();
    update();
    layersChanged();
  }
  Document &doc() { return s_.doc; }
  Session &session() { return s_; }
  Session takeSession()
  {
    commitFloat();
    endCrop();
    pen_.cancel();
    return std::move(s_);
  } // the tab being left: a lifted piece lands, an unfinished path drops;
    // setSession must follow
  bool hasDoc() const
  {
    return !s_.doc.image(0).isNull();
  } // the app starts on Document(QSize()), which has no pixels (#157)
  std::function<void()>
    onLayersChange; // fired when the layer stack or a layer's pixels may have
                    // changed outside the Layers dock (undo, redo, new document,
                    // pointer release, path finish)
  // The Color dock and Ctrl+click set this: in the Pen and Select, the Pen's active
  // swatch (#302, #303); elsewhere the brush color.
  void setColor(const QColor &c);
  QColor color() const
  {
    return tool_ == SpiroPenTool || tool_ == SelectTool
             ? (fillActive_ ? penFill_ : penStroke_)
             : color_;
  }
  void setFillActive(bool on)
  {
    fillActive_ = on;
  } // which Pen swatch setColor targets
  QColor penStroke() const { return penStroke_; }
  QColor penFill() const { return penFill_; }
  double pathWidth() const { return pathWidth_; }
  Document::PaintMode paintMode() const { return paintMode_; }
  bool selecting() const { return tool_ == SelectTool; }
  // The Reference tool's picks' opacity for Tool Options (the first pick's), none
  // without picks (#544).
  std::optional<double> refOpacity() const;
  // Sets the picks' opacity: live previews with no undo step, else one References
  // step from the list before the preview.
  void setRefOpacity(double o, bool live);
  std::vector<Document::VPath>
  pickedPaths() const; // Select's picks, for Tool Options to show (#303)
  std::function<void()>
    onRadiusChange; // fired after a drag resize so the Size slider can follow
  std::function<void(const QColor &)>
    onColorPick; // fired after Ctrl+click so the Color dock can follow
  void setRadius(float r)
  {
    if (brush_)
      setBrushRadius(brush_.get(), r);
    update();
  }
  void setOpacity(float o)
  {
    if (brush_)
      setBrushOpacity(brush_.get(), o);
  }
  void setLassoOpacity(float o) { lassoOpacity_ = o; }
  float lassoOpacity() const { return lassoOpacity_; }
  void setBucketOpacity(float o)
  {
    bucketOpacity_ = o;
  } // Bucket Fill's own Opacity and Threshold, apart from Lasso Fill's (#607)
  void setBucketThreshold(int t) { bucketThreshold_ = t; }
  void setBucketExpand(int px)
  {
    bucketExpand_ = px;
  } // Expand/shrink, Sample merged and Antialiased, passed to the core fill (#608)
  void setBucketMerged(bool on) { bucketMerged_ = on; }
  void setBucketAntialiased(bool on) { bucketAA_ = on; }
  void setSmoothing(int n)
  {
    smooth_.n = n;
  } // the Brush's Stabilizer, 0 = off (#292)
  void setFinishLine(bool on) { finishLine_ = on; }
  void setLassoSmoothing(int n)
  {
    lassoSmooth_.n = n;
  } // Lasso Fill's own Stabilizer, apart from the Brush's (#261)
  void setLassoFinishLine(bool on)
  {
    lassoFinish_ = on;
  } // pen-up flushes the Stabilizer to the pen (#293)
  void setStabilizeSensors(bool on)
  {
    smooth_.sensors = on;
  } // pressure and tilt averaged too (#294)
  float radius() const
  {
    return std::exp(mypaint_brush_get_base_value(
      brush_.get(), MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC));
  }
  float opacity() const
  {
    return mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_OPAQUE);
  }
  void setNodeType(SpiroType t)
  {
    pen_.type = t;
  } // the Spiro Pen's, for new nodes (#215)
  // The Spiro/Bezier toggle: with picks in Select, Bezier converts the picked Spiro
  // paths (#317; Spiro is not built yet); else the Pen's next path (#308), dropping
  // a half-placed one
  void setBezier(bool on);
  bool bezier() const { return pen_.bezier; }
  // Node/Knot edit's picker: retypes the selected nodes (one undo step); the pen's
  // type is left alone.
  void retypeSelected(SpiroType t);
  // Its Bezier list (#314): retypes the selected Bezier nodes of every path,
  // turning their handles to suit, as one setPaths step.
  void retypeSelected(BezierType t);
  bool editingNodes() const { return tool_ == NodeKnotEditTool; }
  // Node/Knot edit has a Bezier path selected, so the picker lists
  // cusp/smooth/symmetric.
  bool bezierSelected() const;
  std::function<void(const QString &)>
    onHint; // fired with the status bar line whenever it changes
  std::function<void()> onPaint; // fired on every repaint, so the Navigation
                                 // rectangle follows any view change
  std::function<void(int)>
    onToolChange; // fired with the new tool (as setTool numbers it), so the Tool
                  // Options dock can follow
  // The shell's tool actions call this with a Tool (tools.h).
  void setTool(int t);
  void setShowReferences(bool on)
  {
    showRefs_ = on;
    update();
  } // View > Show References (#540): not saved, not an undo step
  // The Edit, View and Tools menu actions call these; their keys are window-wide
  // shortcuts.
  void undo(bool redo);
  // Edit > Resize... (#415): Document::resize, then the view and docks refresh.
  void resize(QSize size, QPoint offset, bool scale);
  // File > Import Reference Image (#539): img centred in the view, fitted if
  // larger, as one undo step; the shell has switched to the Reference tool, and the
  // new one is picked.
  void importReference(const QImage &img);
  // Edit > Copy / Cut on a Raster Layer (#506): the Box Select rectangle (a
  // floating piece where it now lies), else the whole layer, onto the system
  // clipboard as an image; Cut then clears it as one undo step. Does nothing on a
  // Vector Layer.
  void copy(bool cut);
  // Edit > Paste (#508): img floats in a new Raster Layer, or on the current one
  // (#509), centred on the document point at, under Select's Transform box (the
  // shell has switched to Select).
  void paste(const QImage &img, QPointF at, bool newLayer);
  // Where Ctrl+V centres a paste: the pointer over the canvas, else (menu Paste,
  // pointer elsewhere) the Document centre.
  QPointF pastePoint() const
  {
    return cursor_ ? s_.view.toDoc(*cursor_)
                   : QRectF(s_.doc.image(0).rect()).center();
  }
  // Where the paste menu opens (#509): at the pointer over the canvas, else the
  // canvas centre; global coordinates.
  QPoint pasteMenuPoint() const
  {
    return mapToGlobal(cursor_ ? cursor_->toPoint() : rect().center());
  }
  std::function<void()> onPasteEnd; // a paste ended by Enter or a click outside:
                                    // the shell gives the previous tool back
  void mirror()
  {
    s_.view.mirrorAt(rect().center());
    update();
  }
  // Erase is the Brush's, Lasso Fill's or Bucket Fill's own (#215, #607); the other
  // tools have none.
  bool erase() const
  {
    return tool_ == LassoFillTool    ? eraseFill_
           : tool_ == BucketFillTool ? eraseBucket_
                                     : tool_ == BrushTool && eraseBrush_;
  }
  void setErase(bool on);
  void setScaleWidths(bool on) { scaleWidths_ = on; }
  // The picks' width in Select, knots scaled (#305), else a new Vector Path's width
  // in px, from the Spiro Pen's Tool Options
  void setPathWidth(double w)
  {
    if (!editPicks([w](Document::VPath &p) { p = rewidthPath(p, w); }))
      pathWidth_ = w;
  }
  void setPaintMode(Document::PaintMode m)
  {
    if (!editPicks([m](Document::VPath &p) { p.mode = m; }))
      paintMode_ = m;
  } // the picks' in Select (#304), else a new Vector Path's, the Pen's own (#301)
  // Select with picks: f edits every picked path, as one undo step (#304). False,
  // and nothing done, with no pick.
  bool editPicks(const std::function<void(Document::VPath &)> &f);
  void finishPath(bool closed);
  // Swaps the brush; color, size and opacity carry over (the dock's sliders are
  // global, not per brush).
  void setBrush(const QString &mybPath);

  // A 1px line of path (doc coordinates) in the inverse of what's behind it; near
  // mid grey, where the inverse is the same grey, it goes black.
  void drawInverted(QPainter &p, const QImage &doc, const QPainterPath &path);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  // Double-click on a reference in the Reference tool enters crop mode on it
  // (#546); elsewhere a second press, as Qt's default.
  void mouseDoubleClickEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *) override { release(); }
  void tabletEvent(QTabletEvent *e) override;
  void leaveEvent(QEvent *) override
  {
    cursor_.reset();
    update();
  }
  // panels showing/hiding move the canvas origin: pan back so the drawing stays put
  // on screen
  void moveEvent(QMoveEvent *) override { pinOrigin(); }
  void resizeEvent(QResizeEvent *) override { pinOrigin(); }
  void wheelEvent(QWheelEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void keyReleaseEvent(QKeyEvent *e) override;

private:
  // Pointer up, mouse or tablet: ends every drag, stroke and smoothing.
  void release()
  {
    cropEdge_.reset();
    cropGrab_.reset();
    endSmooth();
    endPenClose();
    endLasso();
    endKnotDrag();
    endNodeDrag();
    endHandleDrag();
    endMoveDrag();
    endBox();
    dragging_ = painting_ = resizing_ = penDrag_ = false;
    layersChanged();
  }
  // A tablet press or move past the shared handling, routed by tool_.
  void tabletTool(QTabletEvent *e);
  void refTablet(QTabletEvent *e);
  // Ends every selection: Node/Knot edit's path and nodes, Select's and the
  // Reference tool's picks, the Box Select rectangle.
  void clearSelection()
  {
    selected_ = -1;
    nodeSel_.clear();
    others_.clear();
    picked_.clear();
    refPicks_.clear();
    box_ = QRect();
  }
  void layersChanged()
  {
    if (onLayersChange)
      onLayersChange();
  }
  void panZoom(QPointF pos, Qt::KeyboardModifiers mods);
  void pinOrigin()
  {
    QPoint o = mapTo(window(), QPoint());
    if (origin_)
      s_.view.pan(*origin_ - o);
    origin_ = o;
  }
  bool enterCrop(QPointF pos);
  // What the next click does for the current tool and state (#177).
  QString hint() const;
  QString penHint() const; // hint() for the Spiro Pen
  // Spiro Pen: a click near the first node closes the path (6 screen px), any other
  // click places a node. In Bezier the close waits for release, so a drag first
  // pulls the first node's handles (#368).
  void spiroClick(QPointF pos, bool corner = false);
  void endPenClose()
  {
    if (pen_.closing)
      finishPath(true), pen_.closing = false;
  }
  // Select tool: a click within 6 screen px of a path of the current Vector Layer
  // selects it; any other click deselects.
  void selectAt(QPointF pos);
  // Select tool press: on a Width Knot handle of the selected path (within 6 screen
  // px) it starts a drag; on a Spiro Node it selects only that node (Shift toggles
  // it); anywhere else it selects a path and clears the node selection (Shift+click
  // there changes nothing).
  void selectPress(QPointF pos, Qt::KeyboardModifiers mods);
  // selectPress on node n of the selected path: Shift toggles it, else it drags.
  void nodePress(QPointF pos, int n, Qt::KeyboardModifiers mods);
  // Makes shown path p the selected one; the old one and its node selection join
  // others_.
  void makePrimary(int p);
  // A press on a Width Knot, node or handle of another shown path makes that path
  // the selected one, so every edit below works on it.
  void focusHit(QPointF pos);
  // The node selection of every path that has one: the selected path's and
  // others_'.
  std::map<int, std::set<int>> nodeSels() const;
  // Edits every path with selected nodes by f as one undo step; f returns false to
  // refuse the whole edit.
  bool editSelected(
    const std::function<bool(Document::VPath &, const std::set<int> &)> &f);
  // Select press on a Vector Layer: on a path it picks that path alone (Shift
  // toggles it); on empty canvas it clears the pick (Shift keeps it) and starts a
  // box that endBox turns into the paths it touches.
  void objectPress(QPointF pos, Qt::KeyboardModifiers mods);
  // Reference tool press (#542): inside the Transform box (not on an unpicked
  // reference) a drag moves the picks; otherwise a reference is picked alone (Shift
  // toggles it), or empty canvas clears the pick (Shift keeps it) and starts a box.
  void refPress(QPointF pos, Qt::KeyboardModifiers mods);
  // Right-click with references picked: Flip Horizontal / Vertical about the
  // Transform box centre, one undo step each (#543).
  void refMenu(QPoint globalPos);
  // The crop edge or corner of the reference in crop mode within 8 screen px of
  // pos, in its image pixels.
  std::optional<QPoint> cropEdgeUnder(QPointF pos) const;
  // A crop drag previews with no undo step; leaving crop mode is the one step.
  void cropDragTo(QPointF pos);
  // Leaves crop mode: one References step if the crop changed, from the list it
  // began with.
  void endCrop();
  // Crop mode's look (#548, #550): a dotted grey outline of the whole picture, the
  // crop as a black/white dashed frame, white corner brackets (16 px) and edge bars
  // (20 px), 4 px thick.
  void drawCropFrame(QPainter &p);
  // Delete: removes the picked references as one undo step.
  void deleteRefPicks();
  // Select press on a Raster Layer: with a Box Select rectangle, a handle, the
  // rotate circle or the inside of the box starts a drag of the piece; outside
  // commits a floating one.
  void rasterPress(QPointF pos);
  // The floating piece drawn through m: box_'s lifted pixels, or a pasted image
  // sitting on box_ in its own new layer (#508). commit makes it one undo step.
  bool floatPiece(const QTransform &m, bool commit = false);
  // Enter or a click outside: commit, and a paste gives the tool before it back
  // (onPasteEnd).
  void endPaste();
  // The piece's pixels are in the document again: one undo step, and the selection
  // ends.
  void commitFloat();
  void cancelFloat();
  // Select with a group current (#462): the Transform box surrounds everything the
  // group draws, no pick needed; its float is the Document's (#461).
  bool groupBox() const
  {
    return hasDoc() && tool_ == SelectTool && s_.doc.isGroup(s_.doc.current()) &&
           !groupRect_.isNull();
  }
  // Refreshes groupRect_ when the group or the document changed (a float keeps the
  // step id, so it never refreshes mid-drag).
  void updateGroupRect();
  // A Box Select rectangle on the current Raster Layer: Select's Transform box
  // shows on it.
  bool rasterBox() const
  {
    return hasDoc() && !s_.doc.vector(s_.doc.current()) && !box_.isEmpty() &&
           boxLayer_ == s_.doc.current();
  }
  // Box Select press: starts a drag; endBox makes it the rectangle, or a click
  // outside the old one clears it.
  void boxPress(QPointF pos);
  // Delete in Box Select: clears the rectangle's pixels on its layer to transparent
  // as one undo step.
  void deleteBox();
  // Release: a box dragged more than 3 screen px picks the paths it touches (added
  // to the pick, which a press without Shift cleared).
  void endBox();
  // Select: the Transform box (#206): a 1 px frame, eight 8 px squares on its
  // corners and edge midpoints (grabbable, #231), and the rotate circle (5 px, 24
  // px up a stem).
  void drawTransformBox(QPainter &p, const BoxFrame &f);
  // paintEvent's overlays, in its order: crop ghost, references and their picks,
  // then the Spiro Pen's path, Node/Knot edit's handles, Select's and Box Select's
  // boxes, and last the brush outline.
  void drawReferenceOverlays(QPainter &p);
  void drawPenPath(QPainter &p, const QImage &doc);
  void drawNodeHandles(QPainter &p);
  void drawSelectOverlays(QPainter &p);
  void drawBrushOutline(QPainter &p);
  // The Transform box: the frame a rotate drag left, else the picked paths' bounds,
  // unturned (#202).
  BoxFrame frame() const;
  // Whether the screen point is within 6 px of the rotate circle, 24 px up the stem
  // from the box's top edge midpoint.
  bool onRotateHandle(QPointF pos) const;
  // The picked paths follow the pointer live (no undo step yet).
  void dragMoveTo(QPointF pos, bool shift);
  // Release: one undo step for the whole move, from the paths it started with.
  void endMoveDrag();
  // Right-click on Select with paths picked: Flip Horizontal / Vertical about the
  // Transform box centre, each one undo step (#203).
  void flipMenu(QPoint globalPos);
  // Delete: removes the picked paths as one undo step.
  void deletePicked();
  // The paths Select has picked, on the layer they were picked on; none once
  // another layer is current or the list got shorter.
  const std::set<int> &picked() const;
  // Delete: drops the selected nodes of every path as one step, unless fewer than 2
  // would remain on one.
  void deleteSelectedNodes();
  // The selected nodes follow the pointer live (no undo step yet).
  void dragNodesTo(QPointF pos);
  // A pen sends far more moves than a re-rasterize can keep up with, so a drag only
  // queues its preview and paint runs the latest one.
  void flushPreview();
  // Release: one undo step for the whole drag, across every path it moved, from the
  // nodes they started with.
  void endNodeDrag();
  // The selected Bezier node and which handle (out = true) within 6 screen px of
  // pos; a handle on its node's point is the node's (#313).
  std::optional<std::pair<int, bool>> handleAt(QPointF pos);
  // The dragged handle follows the pointer live (no undo step yet).
  void dragHandleTo(QPointF pos);
  // Release: one undo step for the whole drag, from the path it started with.
  void endHandleDrag();
  // Index of the selected path's Width Knot handle within 6 screen px of pos, -1
  // for none.
  int knotAt(QPointF pos);
  // Ctrl+click on a Width Knot handle duplicates the knot, Ctrl+Alt+click deletes
  // it, Ctrl+click on the selected path off a handle adds one, Ctrl+Shift+click
  // there adds a Spiro Node of the picker's type (a Bezier path: a Smooth node on
  // the curve) instead (one undo step each); false otherwise.
  bool editKnot(QPointF pos, Qt::KeyboardModifiers mods);
  // The dragged knot follows an eighth of the pointer's motion live (no undo step
  // yet).
  void dragKnotTo(QPointF pos);
  // Release: one undo step for the whole drag, from the knots it started with.
  void endKnotDrag();
  void resizeTo(QPointF pos);
  void pickAt(QPointF pos);
  // Lasso Fill: collect the outline in document coordinates; the fill lands in
  // endLasso. The eraser end is per stroke: its press decides.
  void lassoAt(QPointF pos, bool eraser);
  // Bucket Fill: a click fills the connected region under it in the current colour
  // (#607).
  void bucketAt(QPointF pos);
  void endLasso();
  void paintAt(
    QPointF pos,
    float pressure,
    float xtilt,
    float ytilt,
    ulong ms,
    bool eraser = false);
  // Pen-up: the Finish Line tail is painted now, before anything seals the stroke's
  // undo step, so it stays one step.
  void endSmooth();
  void smoothTick();
  Session s_;
  // A tablet press makes no mouse events, so no mouseDoubleClickEvent: time two
  // presses ourselves, with the slack a pen needs (it never lands twice on one
  // pixel).
  ulong lastTabletPress_ = 0;
  QPointF lastTabletPos_;
  Stabilizer smooth_;
  PenSample
    raw_{}; // the newest pen sample while smoothing, in document coordinates
  bool smoothErase_ = false, finishLine_ = true;
  QTimer *smoothTimer_ = nullptr;
  std::shared_ptr<MyPaintBrush> brush_ =
    [] { // the first brush listed: the Brushes list starts on the same one
      auto all = listBrushes(BRUSH_DIR);
      return all.empty() ? nullptr : loadBrush(all[0].path);
    }();
  ulong lastMs_ = 0;
  QPointF last_;
  QString hint_;
  std::optional<QPoint>
    origin_; // canvas top-left in window coordinates, for pinOrigin()
  std::optional<QPointF>
    cursor_; // pointer position over the canvas, for the brush outline
  bool dragging_ = false, space_ = false, painting_ = false, resizing_ = false;
  QColor color_ = Qt::black; // the Color dock's start
  QPolygonF lasso_;
  Stabilizer lassoSmooth_;
  bool lassoFinish_ = true;
  SpiroPen pen_;
  bool penDrag_ =
    false; // a Bezier press is down: moving pulls the new node's handle (#309)
  std::function<void()>
    preview_; // the latest queued drag preview, run by flushPreview
  int knotDrag_ =
    -1; // index of the Width Knot being dragged on the selected path, -1 for none
  std::vector<WidthKnot>
    knotsBefore_; // the selected path's knots when the drag began
  QPointF knotPress_,
    knotStart_; // doc-space press point and the dragged knot's handle there (#374)
  bool nodeDrag_ = false; // a Spiro Node drag is under way
  QPointF nodeStart_;     // where it began, in document coordinates
  int handleDrag_ =
    -1; // the Bezier node whose handle is being dragged (#313), -1 for none
  bool handleOut_ = false; // its out handle, else its in handle
  Document::VPath
    pathBefore_; // the selected path's nodes and closed when a handle drag began
  std::map<int, Document::VPath>
    nodesBefore_; // each moved path's nodes and closed when a node drag began
  std::set<int> nodeSel_; // selected Spiro Node indexes of the selected path
  std::map<int, std::set<int>>
    others_; // other paths shown by Shift+click, with their selected node indexes
  std::set<int> picked_; // Select's picked path indexes on layer pickedLayer_
  int pickedLayer_ = -1;
  bool moveDrag_ = false; // a Transform box move or scale is under way
  std::optional<QPoint>
    handle_;           // the box handle being dragged (scale), none for a move
  BoxFrame moveFrame_; // the box when a drag began
  std::optional<BoxFrame> frame_; // the box a rotate left, until the next pick
  QImage pasted_; // the image a paste floats (#508); null when the floating piece
                  // is a lifted Box Select rectangle
  bool pasteNew_ =
    true;            // pasted_ floats in a layer of its own, not the current one
  QRectF groupRect_; // what the current group draws, as of document step
                     // groupKeyStep_ on layer groupKeyLayer_
  int groupKeyLayer_ = -1, groupKeyStep_ = -1;
  bool groupFloat_ = false; // the group is lifted: the Document shows it moved
                            // through groupM_, not yet an undo step
  QTransform groupM_;
  std::optional<BoxFrame> groupFrame_;
  bool floating_ =
    false; // a raster selection's piece is lifted: the layer shows box_'s pixels
           // moved through floatM_, not yet an undo step
  QTransform floatM_;
  bool scaleWidths_ =
    false; // Select's Scale widths: transforms scale Width Knot widths too
  bool rotating_ = false; // the drag is a rotate
  QPointF moveStart_;     // where the drag began, in document coordinates
  QTransform dragM_;      // what the drag has done to the picked paths so far
  std::vector<Document::VPath>
    pathsBefore_;       // the current layer's paths when it began
  bool boxing_ = false; // a Select box drag is under way
  QPointF boxStart_;    // where it began, in document coordinates
  QRect box_; // Box Select's rectangle in document pixels, null for none; it
              // belongs to layer boxLayer_
  SpiroType editType_ =
    SpiroType::G2; // Node/Knot edit's picker choice: retypes selected nodes and
                   // types a node added with Ctrl+Shift+click
  int boxLayer_ = -1;
  int penLayer_ = -1; // the layer the Spiro Pen's unfinished path was started on
  int selected_ =
    -1; // index of the selected path on the current layer, -1 for none
  Tool tool_ = BrushTool; // the tool in use (setTool)
  bool showRefs_ = true;
  std::set<int> refPicks_; // the Reference tool's picked references, indexes into
                           // Document::references()
  std::vector<Reference>
    refsBefore_;      // the references when a Reference tool drag began
  int cropping_ = -1; // the reference in crop mode (#546), -1 for none
  std::optional<QPoint> cropEdge_; // the crop edge being dragged
  std::optional<QPointF>
    cropGrab_; // where in the image a crop move began, and the crop then
  QRectF cropGrabFrom_;
  std::vector<Reference> cropBefore_; // the references when crop mode began
  std::optional<std::vector<Reference>>
    opacityBefore_;       // the references when an opacity slider drag began
  double pathWidth_ = 10; // px
  Document::PaintMode paintMode_ = Document::PaintMode::Stroke;
  QColor penStroke_ = Qt::black,
         penFill_ = QColor(0x2d, 0x5d, 0x9a); // the Pen's Stroke and Fill swatches
  bool fillActive_ = false;
  float lassoOpacity_ = 1, otherLassoOpacity_ = 1, bucketOpacity_ = 1;
  int bucketThreshold_ = 15, bucketExpand_ = 0;
  bool bucketMerged_ = true, bucketAA_ = true;
  float otherRadius_ = radius(),
        otherOpacity_ =
          opacity(); // the Brush's Size and Opacity for the mode it isn't in
  bool eraseBucket_ = false, eraseBrush_ = false, eraseFill_ = false,
       lassoing_ = false, lassoErase_ = false;
};
