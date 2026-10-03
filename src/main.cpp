#include "document.h"
#include "spiropen.h"
#include "tools.h"
#include "view.h"
#include "ora.h"
#include "stabilizer.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QActionGroup>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QConicalGradient>
#include <QPainterPath>
#include <QMouseEvent>
#include <algorithm>
#include <functional>
#include <qboxlayout.h>
#include <qdialogbuttonbox.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qwidget.h>
#include <set>
#include <optional>
#include <QTabletEvent>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

// Krita Default navigation (/r/5): middle drag or Space+left drag pans; +Ctrl zooms, +Shift rotates; wheel zooms; M mirrors; Ctrl+Z undoes, Ctrl+Shift+Z redoes.
// Shift+left drag (mouse or pen) resizes the brush instead of painting: 100 px right = x e.
// Ctrl+left click (mouse or pen) picks the visible color at that point instead of painting.
// Painting: the pen tip, or plain left button at pressure 0.5, feeds pen samples to the Document.
// One open document as the Canvas works on it: the Document, its view, its file and its saved mark (#279).
struct Session {
    Document doc{QSize()};
    View view;
    QString path;  // the document's file, empty until opened or first saved (#277)
    int saved = 0;  // Document::stepId() at the last open or save (#240)
    int untitled = 0;  // N of "Untitled N", the tab label until the first save (#282)
};

class Canvas : public QWidget {
public:
    Canvas() { setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); }
    void setDocument(Document doc, QString path = {}) { Session s{std::move(doc), View(), path, 0}; s.saved = s.doc.stepId(); setSession(std::move(s)); }
    void setSession(Session s) { s_ = std::move(s); floating_ = false; floatM_ = {}; frame_.reset(); selected_ = -1; nodeSel_.clear(); picked_.clear(); box_ = QRect(); update(); if (onLayersChange) onLayersChange(); }
    Document& doc() { return s_.doc; }
    Session& session() { return s_; }
    Session takeSession() { commitFloat(); pen_.cancel(); return std::move(s_); }  // the tab being left: a lifted piece lands, an unfinished path drops; setSession must follow
    bool hasDoc() const { return !s_.doc.image(0).isNull(); }  // the app starts on Document(QSize()), which has no pixels (#157)
    std::function<void()> onLayersChange;  // fired when the layer stack or a layer's pixels may have changed outside the Layers dock (undo, redo, new document, pointer release, path finish)
    // The Color dock and Ctrl+click set this: in the Pen and Select, the Pen's active swatch (#302, #303); elsewhere the brush color.
    void setColor(const QColor& c) {
        if (editPicks([&](Document::VPath& p) { (fillActive_ ? p.fillColor : p.color) = c; })) return;
        if (spiroTool_ || objectTool_) (fillActive_ ? penFill_ : penStroke_) = c;
        else { color_ = c; if (brush_) setBrushColor(brush_.get(), c); }
    }
    QColor color() const { return spiroTool_ || objectTool_ ? (fillActive_ ? penFill_ : penStroke_) : color_; }
    void setFillActive(bool on) { fillActive_ = on; }  // which Pen swatch setColor targets
    QColor penStroke() const { return penStroke_; }
    QColor penFill() const { return penFill_; }
    double pathWidth() const { return pathWidth_; }
    Document::PaintMode paintMode() const { return paintMode_; }
    bool selecting() const { return objectTool_; }
    std::vector<Document::VPath> pickedPaths() const {  // Select's picks, for Tool Options to show (#303)
        std::vector<Document::VPath> ps;
        for (int i : picked()) ps.push_back(s_.doc.paths(s_.doc.current())[i]);
        return ps;
    }
    std::function<void()> onRadiusChange;  // fired after a drag resize so the Size slider can follow
    std::function<void(const QColor&)> onColorPick;  // fired after Ctrl+click so the Color dock can follow
    void setRadius(float r) { if (brush_) setBrushRadius(brush_.get(), r); update(); }
    void setOpacity(float o) { if (brush_) setBrushOpacity(brush_.get(), o); }
    void setLassoOpacity(float o) { lassoOpacity_ = o; }
    void setSmoothing(int n) { smooth_.n = n; }  // the Brush's Stabilizer, 0 = off (#292)  // Lasso Fill's own, apart from the Brush's (#213)
    void setFinishLine(bool on) { finishLine_ = on; }
    void setLassoSmoothing(int n) { lassoSmooth_.n = n; }  // Lasso Fill's own Stabilizer, apart from the Brush's (#261)
    void setLassoFinishLine(bool on) { lassoFinish_ = on; }  // pen-up flushes the Stabilizer to the pen (#293)
    void setStabilizeSensors(bool on) { smooth_.sensors = on; }  // pressure and tilt averaged too (#294)
    float radius() const { return std::exp(mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC)); }
    float opacity() const { return mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_OPAQUE); }
    void setNodeType(SpiroType t) { pen_.type = t; }  // the Spiro Pen's, for new nodes (#215)
    // The Spiro/Bezier toggle: with picks in Select, Bezier converts the picked Spiro paths (#317; Spiro is not built yet); else the Pen's next path (#308), dropping a half-placed one
    void setBezier(bool on) {
        if (objectTool_ && !picked().empty()) {
            if (on) editPicks([](Document::VPath& p) { if (p.kind == Document::PathKind::Spiro) p = Document::toBezier(p); });
            return;
        }
        pen_.bezier = on; pen_.cancel(); update();
    }
    bool bezier() const { return pen_.bezier; }
    // Node/Knot edit's picker: retypes the selected nodes (one undo step); the pen's type is left alone.
    void retypeSelected(SpiroType t) {
        editType_ = t;
        if (selectTool_ && selected_ >= 0 && !nodeSel_.empty() && !bezierSelected()) editNodes(retypeNodes(s_.doc.nodes(s_.doc.current(), selected_), nodeSel_, t));
    }
    // Its Bezier list (#314): retypes the selected Bezier nodes, turning their handles to suit, as one setPaths step.
    void retypeSelected(BezierType t) {
        if (!bezierSelected() || nodeSel_.empty()) return;
        auto paths = s_.doc.paths(s_.doc.current());
        paths[selected_].bezier = retypeBezier(paths[selected_].bezier, nodeSel_, t, paths[selected_].closed);
        s_.doc.setPaths(s_.doc.current(), paths);
        update();
    }
    bool editingNodes() const { return selectTool_; }
    // Node/Knot edit has a Bezier path selected, so the picker lists cusp/smooth/symmetric.
    bool bezierSelected() const {
        if (!selectTool_ || selected_ < 0 || selected_ >= int(s_.doc.paths(s_.doc.current()).size())) return false;
        return s_.doc.paths(s_.doc.current())[selected_].kind == Document::PathKind::Bezier;
    }
    std::function<void(const QString&)> onHint;  // fired with the status bar line whenever it changes
    std::function<void()> onPaint;  // fired on every repaint, so the Navigation rectangle follows any view change
    std::function<void(int)> onToolChange;  // fired with the new tool (as setTool numbers it), so the Tool Options dock can follow
    // The shell's tool actions call this: 0 Brush, 1 Lasso Fill, 2 Spiro Pen, 3 Node/Knot edit (selectTool_), 4 Select (objectTool_: picks and deletes whole paths), 5 Box Select (boxTool_).
    void setTool(int t) {
        commitFloat();
        fillTool_ = t == 1; spiroTool_ = t == 2; selectTool_ = t == 3; objectTool_ = t == 4; boxTool_ = t == 5;
        if (selectTool_) { selected_ = -1; nodeSel_.clear(); }
        if (!spiroTool_) pen_.cancel();
        picked_.clear();
        boxing_ = false;
        unsetCursor();
        update();
        if (onToolChange) onToolChange(t);
    }
    // The Edit, View and Tools menu actions call these; their keys are window-wide shortcuts.
    void undo(bool redo) {
        commitFloat();  // so undo takes the whole piece back
        redo ? s_.doc.redo() : s_.doc.undo();
        selected_ = -1;
        nodeSel_.clear();
        picked_.clear();
        box_ = QRect();
        update();
        if (onLayersChange) onLayersChange();
    }
    void mirror() { s_.view.mirrorAt(rect().center()); update(); }
    // Erase is the Brush's or Lasso Fill's own (#215); the other tools have none.
    bool erase() const { return fillTool_ ? eraseFill_ : !spiroTool_ && !selectTool_ && !objectTool_ && !boxTool_ && eraseBrush_; }
    void setErase(bool on) { if (!spiroTool_ && !selectTool_ && !objectTool_ && !boxTool_) (fillTool_ ? eraseFill_ : eraseBrush_) = on; }
    void setScaleWidths(bool on) { scaleWidths_ = on; }
    // The picks' width in Select, knots scaled (#305), else a new Vector Path's width in px, from the Spiro Pen's Tool Options
    void setPathWidth(double w) { if (!editPicks([w](Document::VPath& p) { p = rewidthPath(p, w); })) pathWidth_ = w; }
    void setPaintMode(Document::PaintMode m) { if (!editPicks([m](Document::VPath& p) { p.mode = m; })) paintMode_ = m; }  // the picks' in Select (#304), else a new Vector Path's, the Pen's own (#301)
    // Select with picks: f edits every picked path, as one undo step (#304). False, and nothing done, with no pick.
    bool editPicks(const std::function<void(Document::VPath&)>& f) {
        if (!objectTool_ || picked().empty()) return false;
        auto ps = s_.doc.paths(s_.doc.current());
        for (int i : picked()) f(ps[i]);
        s_.doc.setPaths(s_.doc.current(), ps);
        update();
        if (onLayersChange) onLayersChange();
        return true;
    }
    void finishPath(bool closed) {
        if (floating_) return commitFloat();
        if (spiroTool_) pen_.finish(s_.doc, closed, penStroke_, pathWidth_, paintMode_, penFill_);  // refused on a Raster Layer
        update();
        if (onLayersChange) onLayersChange();
    }
    // Swaps the brush; color, size and opacity carry over (the dock's sliders are global, not per brush).
    void setBrush(const QString& mybPath) {
        auto b = loadBrush(mybPath);
        if (!b) return;
        for (auto s : {MYPAINT_BRUSH_SETTING_COLOR_H, MYPAINT_BRUSH_SETTING_COLOR_S, MYPAINT_BRUSH_SETTING_COLOR_V, MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, MYPAINT_BRUSH_SETTING_OPAQUE})
            mypaint_brush_set_base_value(b.get(), s, mypaint_brush_get_base_value(brush_.get(), s));
        brush_ = b;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (boxLayer_ != s_.doc.current()) { box_ = QRect(); floating_ = false; floatM_ = {}; frame_.reset(); }  // a layer switch clears the Box Select rectangle; setCurrent already sealed any floating piece
        flushPreview();
        if (penLayer_ != s_.doc.current()) { pen_.cancel(); penLayer_ = s_.doc.current(); }  // a layer switch drops the unfinished path
        QString h = hint();  // every tool or state change calls update(), so paint is where a change of hint is noticed
        if (h != hint_) { hint_ = h; if (onHint) onHint(h); }
        if (onPaint) onPaint();
        QPainter p(this);
        if (!hasDoc()) {
            p.fillRect(rect(), QColor(90, 90, 90));
            p.setPen(Qt::white);
            p.drawText(rect(), Qt::AlignCenter, "File > New or Open");
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(s_.view.matrix());
        p.drawImage(0, 0, s_.doc.render());
        if (lasso_.size() > 1) {
            QPen pen(Qt::gray, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.drawPolyline(lasso_.data(), lasso_.size());  // the layer is untouched until pen-up
        }
        if (spiroTool_ && !pen_.nodes.empty()) {  // the path so far, plus a node under the pointer that a click would place
            auto nodes = pen_.nodes;
            if (cursor_ && !penDrag_) nodes.push_back({s_.view.toDoc(*cursor_).x(), s_.view.toDoc(*cursor_).y(), pen_.bezier ? SpiroType::Corner : pen_.type});
            std::optional<QPainterPath> path;
            if (pen_.bezier) {  // a Bezier path in progress: the dragged handles shape it (#309)
                Document::VPath v{{}, pen_.closing, Qt::black, 1, {}, Document::PathKind::Bezier};  // closed while a press on the first node is down (#368)
                v.bezier = pen_.bezierNodes();
                if (nodes.size() > v.bezier.size()) v.bezier.push_back({{nodes.back().x, nodes.back().y}, {nodes.back().x, nodes.back().y}, {nodes.back().x, nodes.back().y}, BezierType::Cusp});
                path = Document::curve(v);
            } else path = spiroToPath(nodes, false);
            if (path) {
                QPen pen(Qt::gray, 1);
                pen.setCosmetic(true);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawPath(*path);
            }
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            double r = 4 / s_.view.screenRadius(1);
            bool closing = cursor_ && pen_.nodes.size() > 2 && pen_.onFirst(s_.view.toDoc(*cursor_), 6 / s_.view.screenRadius(1));
            for (size_t i = 0; i < pen_.nodes.size(); i++) {  // a square per placed node; the first is filled when a click would close (#180)
                p.setBrush(i == 0 && closing ? Qt::blue : Qt::white);
                p.drawRect(QRectF(pen_.nodes[i].x - r, pen_.nodes[i].y - r, 2 * r, 2 * r));
            }
        }
        if (selectTool_ && selected_ >= 0) {  // a square per Spiro Node of the selected path, filled when selected, then a circle per Width Knot
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            double r = 4 / s_.view.screenRadius(1);
            auto nodes = s_.doc.nodes(s_.doc.current(), selected_);
            auto& path = s_.doc.paths(s_.doc.current())[selected_];
            for (int i : nodeSel_)  // a selected Bezier node's handles: a line out to a circle, skipped where a handle sits on its point (#313)
                if (i < int(path.bezier.size()))
                    for (QPointF h : {path.bezier[i].in, path.bezier[i].out})
                        if (h != path.bezier[i].p) p.setBrush(Qt::white), p.drawLine(path.bezier[i].p, h), p.drawEllipse(h, r, r);
            for (int i = 0; i < int(nodes.size()); i++) {
                p.setBrush(nodeSel_.count(i) ? Qt::blue : Qt::white);
                p.drawRect(QRectF(nodes[i].x - r, nodes[i].y - r, 2 * r, 2 * r));
            }
        }
        if (selectTool_ && selected_ >= 0) {  // a handle per Width Knot of the selected path
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::white);
            double r = 4 / s_.view.screenRadius(1);
            for (QPointF h : s_.doc.knotHandles(s_.doc.current(), selected_)) p.drawEllipse(h, r, r);
        }
        if (objectTool_) {  // Select: the picked paths' outlines, and the box being dragged
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            for (int i : picked()) p.drawPath(Document::outline(s_.doc.paths(s_.doc.current())[i]));
            if (!picked().empty() || rasterBox()) drawTransformBox(p, frame());
            if (boxing_ && cursor_) {
                pen.setStyle(Qt::DashLine);
                p.setPen(pen);
                p.drawRect(QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized());
            }
        }
        QRectF marked = boxTool_ && boxing_ && cursor_ ? QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized() : QRectF(box_);  // Box Select: the rectangle, or the one being dragged
        if (!marked.isEmpty()) {
            QPen pen(Qt::blue, 1, Qt::DashLine);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(marked);
        }
        if (cursor_ && brush_ && !fillTool_ && !spiroTool_ && !selectTool_ && !objectTool_ && !boxTool_) {  // brush outline: dark with a light line beside it, readable on any color
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
    void mousePressEvent(QMouseEvent* e) override {
        dragging_ = e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_);
        last_ = e->position();
        resizing_ = e->button() == Qt::LeftButton && !space_ && !selectTool_ && !objectTool_ && !boxTool_ && !spiroTool_ && (e->modifiers() & Qt::ShiftModifier);
        if (e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ControlModifier) && !editKnot(e->position(), e->modifiers())) pickAt(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ControlModifier)) return;
        else if (e->button() == Qt::RightButton && spiroTool_) finishPath(false);
        else if (e->button() == Qt::RightButton && objectTool_) flipMenu(e->globalPosition().toPoint());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && selectTool_) selectPress(e->position(), e->modifiers());
        else if (e->button() == Qt::LeftButton && !space_ && objectTool_) objectPress(e->position(), e->modifiers());
        else if (e->button() == Qt::LeftButton && !space_ && boxTool_) boxPress(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && spiroTool_) spiroClick(e->position(), e->modifiers() & Qt::ShiftModifier);
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && fillTool_) lassoAt(e->position(), erase());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && !objectTool_ && !boxTool_) paintAt(e->position(), 0.5f, 0, 0, e->timestamp(), erase());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!resizing_) cursor_ = e->position();  // the circle stays at the press point while Shift+drag resizes
        update();
        if (resizing_) return resizeTo(e->position());
        if (knotDrag_ >= 0) return dragKnotTo(e->position());
        if (nodeDrag_) return dragNodesTo(e->position());
        if (handleDrag_ >= 0) return dragHandleTo(e->position());
        if (moveDrag_) return dragMoveTo(e->position(), e->modifiers() & Qt::ShiftModifier);
        if (penDrag_) return pen_.drag(s_.view.toDoc(e->position()));
        if (boxing_) return;
        if (objectTool_) setCursor(!picked().empty() && onRotateHandle(e->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
        if (lassoing_) return lassoAt(e->position(), erase());
        if (painting_) return paintAt(e->position(), 0.5f, 0, 0, e->timestamp(), erase());
        if (!dragging_) return;
        panZoom(e->position(), e->modifiers());
    }
    void panZoom(QPointF pos, Qt::KeyboardModifiers mods) {
        QPointF d = pos - last_;
        last_ = pos;
        if (mods & Qt::ControlModifier) s_.view.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
        else if (mods & Qt::ShiftModifier) s_.view.rotateAt(rect().center(), d.x() * 0.5);
        else s_.view.pan(d);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { endPenClose(); endLasso(); endKnotDrag(); endNodeDrag(); endHandleDrag(); endMoveDrag(); endBox(); dragging_ = painting_ = resizing_ = penDrag_ = false; if (onLayersChange) onLayersChange(); }
    void tabletEvent(QTabletEvent* e) override {
        if (space_) return e->ignore();  // Qt turns it into mouse events, so Space+pen drag pans
        e->accept();
        if (e->type() == QEvent::TabletPress) setFocus();  // an accepted tablet press makes no mouse press, so nothing else gives the canvas the keys (Esc)
        if (e->type() == QEvent::TabletLeaveProximity) { cursor_.reset(); update(); }
        else if (!resizing_ || e->type() == QEvent::TabletPress) { cursor_ = e->position(); update(); }
        if (e->button() == Qt::MiddleButton && e->type() != QEvent::TabletMove) { dragging_ = e->type() == QEvent::TabletPress; last_ = e->position(); return; }  // barrel button pans like the mouse's middle button
        if (dragging_) return panZoom(e->position(), e->modifiers());
        bool eraser = e->pointerType() == QPointingDevice::PointerType::Eraser;
        if (e->type() == QEvent::TabletRelease) { endPenClose(); endLasso(); endKnotDrag(); endNodeDrag(); endHandleDrag(); endMoveDrag(); endBox(); painting_ = resizing_ = penDrag_ = false; if (onLayersChange) onLayersChange(); }
        else if (e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ControlModifier)) { if (!editKnot(e->position(), e->modifiers())) pickAt(e->position()); }
        else if (e->type() == QEvent::TabletPress && !selectTool_ && !objectTool_ && !boxTool_ && !spiroTool_ && (e->modifiers() & Qt::ShiftModifier)) { resizing_ = true; last_ = e->position(); }
        else if (resizing_) resizeTo(e->position());
        else if (selectTool_) { if (e->type() == QEvent::TabletPress) selectPress(e->position(), e->modifiers()); else if (knotDrag_ >= 0) dragKnotTo(e->position()); else if (nodeDrag_) dragNodesTo(e->position()); else if (handleDrag_ >= 0) dragHandleTo(e->position()); }
        else if (objectTool_) { if (e->type() == QEvent::TabletPress) objectPress(e->position(), e->modifiers()); else if (moveDrag_) dragMoveTo(e->position(), e->modifiers() & Qt::ShiftModifier); }
        else if (boxTool_) { if (e->type() == QEvent::TabletPress) boxPress(e->position()); }
        else if (spiroTool_) { if (e->type() == QEvent::TabletPress) { if (e->button() == Qt::RightButton) finishPath(false); else spiroClick(e->position(), e->modifiers() & Qt::ShiftModifier); } else if (penDrag_) pen_.drag(s_.view.toDoc(e->position())); }  // the barrel button arrives as right (#178)
        else if (fillTool_ && (e->type() == QEvent::TabletPress || lassoing_)) lassoAt(e->position(), erase() || eraser);
        else if (!objectTool_ && !boxTool_ && (e->pointerType() == QPointingDevice::PointerType::Pen || e->pointerType() == QPointingDevice::PointerType::Eraser) && (e->type() == QEvent::TabletPress || painting_))
            paintAt(e->position(), e->pressure(), e->xTilt() / 60, e->yTilt() / 60, e->timestamp(),  // libmypaint tilt is -1..1 for +-60 degrees
                    eraser);  // the flag is per call (b20)
    }
    void leaveEvent(QEvent*) override { cursor_.reset(); update(); }
    void wheelEvent(QWheelEvent* e) override {
        s_.view.zoomAt(e->position(), std::pow(1.1, e->angleDelta().y() / 120.0));
        update();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->isAutoRepeat()) return;
        if (e->key() == Qt::Key_Space) space_ = true;
        else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            if (selectTool_) deleteSelectedNodes();
            else if (objectTool_) deletePicked();
            else if (boxTool_) deleteBox();
            else pen_.backspace();
            update();
        }
        else if (e->key() == Qt::Key_Escape) { pen_.cancel(); cancelFloat(); box_ = QRect(); update(); }
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (!e->isAutoRepeat() && e->key() == Qt::Key_Space) space_ = false;
    }

private:
    // What the next click does for the current tool and state (#177).
    QString hint() const {
        if (!hasDoc()) return {};
        if (spiroTool_ && pen_.nodes.empty()) return "Click places the first node";
        if (spiroTool_ && pen_.bezier) return "Click adds a corner · Enter or right-click finishes open · click the first node or Shift+Enter closes · Backspace drops the last node · Esc cancels";
        if (spiroTool_) return "Click adds a node · Shift+click adds a corner · Enter or right-click finishes open · click the first node or Shift+Enter closes · Backspace drops the last node · Esc cancels";
        if (selectTool_ && selected_ < 0) return "Click a path to select it";
        if (selectTool_) return "Click or Shift+click nodes · drag nodes · Delete removes them · drag a knot · Ctrl+click a knot duplicates · Ctrl+click the path adds a knot · Ctrl+Shift+click the path adds a node · Ctrl+Alt+click deletes";
        if (objectTool_ && rasterBox()) return "Drag inside the box moves the pixels · handles scale · circle rotates · right-click flips · Enter or click outside commits · Esc puts them back";
        if (objectTool_ && !s_.doc.vector(s_.doc.current())) return "Make a rectangle with Box Select first, then Select transforms its pixels";
        if (boxTool_ && box_.isEmpty()) return "Drag a rectangle to select pixels";
        if (boxTool_) return "Delete clears the pixels · drag a new rectangle to replace it · click outside or Esc clears the selection";
        if (objectTool_ && picked().empty()) return "Click a path to pick it · Shift+click toggles · drag a box on empty canvas picks the paths it touches";
        if (objectTool_) return QString("%1 picked · drag inside the box moves them · drag a handle scales (Shift keeps the ratio on a corner) · Delete removes them · click or drag a box to pick again · Shift adds").arg(picked().size());
        return "Draw · Shift+drag sizes the brush · Ctrl+click picks a color · E toggles erase";
    }
    // Spiro Pen: a click near the first node closes the path (6 screen px), any other click places a node. In Bezier the close waits
    // for release, so a drag first pulls the first node's handles (#368).
    void spiroClick(QPointF pos, bool corner = false) {
        QPointF d = s_.view.toDoc(pos);
        if (pen_.nodes.size() > 2 && pen_.onFirst(d, 6 / s_.view.screenRadius(1))) { if (pen_.bezier) pen_.closing = penDrag_ = true; else finishPath(true); }
        else pen_.click(d, corner), penDrag_ = pen_.bezier;
        update();
    }
    void endPenClose() { if (pen_.closing) finishPath(true), pen_.closing = false; }
    // Select tool: a click within 6 screen px of a path of the current Vector Layer selects it; any other click deselects.
    void selectAt(QPointF pos) {
        selected_ = s_.doc.pathAt(s_.doc.current(), s_.view.toDoc(pos), 6 / s_.view.screenRadius(1));
        nodeSel_.clear();
        update();
    }
    // Select tool press: on a Width Knot handle of the selected path (within 6 screen px) it starts a drag; on a Spiro Node it selects
    // only that node (Shift toggles it); anywhere else it selects a path and clears the node selection (Shift+click there changes nothing).
    void selectPress(QPointF pos, Qt::KeyboardModifiers mods) {
        int i = knotAt(pos);
        int n = selected_ < 0 ? -1 : s_.doc.nodeAt(s_.doc.current(), selected_, s_.view.toDoc(pos), 6 / s_.view.screenRadius(1));
        if (i >= 0 && n >= 0) {  // a knot handle near a node (the t=0 knot by node 0): the nearer one takes the press (#368)
            QPointF d = s_.view.toDoc(pos), np(s_.doc.nodes(s_.doc.current(), selected_)[n].x, s_.doc.nodes(s_.doc.current(), selected_)[n].y);
            if (QLineF(np, d).length() <= QLineF(s_.doc.knotHandles(s_.doc.current(), selected_)[i], d).length()) i = -1;
        }
        if (i >= 0) {
            knotDrag_ = i;
            knotsBefore_ = s_.doc.knots(s_.doc.current(), selected_);
            knotPress_ = s_.view.toDoc(pos);
            knotStart_ = s_.doc.knotHandles(s_.doc.current(), selected_)[i];
            return;
        }
        if (auto h = handleAt(pos)) {
            std::tie(handleDrag_, handleOut_) = *h;
            pathBefore_ = s_.doc.paths(s_.doc.current())[selected_];
            return;
        }
        if (n < 0 && (mods & Qt::ShiftModifier)) return;
        if (n < 0) return selectAt(pos);
        if (mods & Qt::ShiftModifier) {
            if (!nodeSel_.erase(n)) nodeSel_.insert(n);
        } else {  // a press on a selected node keeps the selection, so all selected nodes drag together
            if (!nodeSel_.count(n)) nodeSel_ = {n};
            nodeDrag_ = true;
            nodeStart_ = s_.view.toDoc(pos);
            pathBefore_ = {s_.doc.nodes(s_.doc.current(), selected_), s_.doc.paths(s_.doc.current())[selected_].closed};  // nodes(), so a Bezier path's points (#310)
        }
        update();
    }
    // Select press on a Vector Layer: on a path it picks that path alone (Shift toggles it); on empty canvas it clears the pick (Shift keeps it)
    // and starts a box that endBox turns into the paths it touches.
    void objectPress(QPointF pos, Qt::KeyboardModifiers mods) {
        if (!s_.doc.vector(s_.doc.current())) return rasterPress(pos);
        bool shift = mods & Qt::ShiftModifier;
        int i = s_.doc.pathAt(s_.doc.current(), s_.view.toDoc(pos), 6 / s_.view.screenRadius(1));
        auto was = picked();
        auto handle = was.empty() ? std::nullopt : frameHandleAt(frame(), s_.view.toDoc(pos), 6 / s_.view.screenRadius(1));
        bool rotate = !was.empty() && onRotateHandle(pos);
        if (handle || rotate) {  // a Transform box handle: drag scales the picked paths about the opposite corner or edge (Shift on a corner keeps the ratio); the circle rotates them about the centre
            moveFrame_ = frame();
            moveDrag_ = true;
            handle_ = handle;
            rotating_ = rotate;
            moveStart_ = s_.view.toDoc(pos);
            dragM_ = {};
            pathsBefore_ = s_.doc.paths(s_.doc.current());
            return;
        }
        if (!shift && !was.empty() && frame().rect.contains(frame().toDoc().inverted().map(s_.view.toDoc(pos))) && (i < 0 || was.count(i))) {  // inside the Transform box: drag moves the picked paths
            moveFrame_ = frame();
            moveDrag_ = true;
            rotating_ = false;
            moveStart_ = s_.view.toDoc(pos);
            handle_.reset();
            dragM_ = {};
            pathsBefore_ = s_.doc.paths(s_.doc.current());
            return;
        }
        pickedLayer_ = s_.doc.current();
        frame_.reset();
        picked_ = shift ? was : std::set<int>{};
        if (i >= 0) {
            if (!shift) picked_ = {i};
            else if (!picked_.erase(i)) picked_.insert(i);
        } else {
            boxing_ = true;
            boxStart_ = s_.view.toDoc(pos);
        }
        update();
    }
    // Select press on a Raster Layer: with a Box Select rectangle, a handle, the rotate circle or the inside of the box starts a drag of the piece; outside commits a floating one.
    void rasterPress(QPointF pos) {
        if (!rasterBox()) return;
        BoxFrame f = frame();
        QPointF d = s_.view.toDoc(pos);
        auto handle = frameHandleAt(f, d, 6 / s_.view.screenRadius(1));
        bool rotate = onRotateHandle(pos);
        if (!handle && !rotate && !f.rect.contains(f.toDoc().inverted().map(d))) return commitFloat();
        moveFrame_ = f;
        moveDrag_ = true;
        handle_ = handle;
        rotating_ = rotate;
        moveStart_ = d;
        dragM_ = {};
    }
    // The piece's pixels are in the document again: one undo step, and the selection ends.
    void commitFloat() {
        if (!floating_) return;
        s_.doc.transformRect(box_, floatM_);
        floating_ = false;
        floatM_ = {};
        frame_.reset();
        box_ = QRect();
        update();
        if (onLayersChange) onLayersChange();
    }
    void cancelFloat() {
        if (!floating_) return;
        s_.doc.cancelFloat();
        floating_ = false;
        floatM_ = {};
        frame_.reset();
        update();
    }
    // A Box Select rectangle on the current Raster Layer: Select's Transform box shows on it.
    bool rasterBox() const { return hasDoc() && !s_.doc.vector(s_.doc.current()) && !box_.isEmpty() && boxLayer_ == s_.doc.current(); }
    // Box Select press: starts a drag; endBox makes it the rectangle, or a click outside the old one clears it.
    void boxPress(QPointF pos) {
        boxing_ = true;
        boxStart_ = s_.view.toDoc(pos);
        update();
    }
    // Delete in Box Select: clears the rectangle's pixels on its layer (transparent, white on the background) as one undo step.
    void deleteBox() {
        if (box_.isEmpty() || boxLayer_ != s_.doc.current() || !s_.doc.clearRect(box_)) return;
        update();
        if (onLayersChange) onLayersChange();
    }
    // Release: a box dragged more than 3 screen px picks the paths it touches (added to the pick, which a press without Shift cleared).
    void endBox() {
        if (!boxing_) return;
        boxing_ = false;
        if (cursor_) {
            QRectF box = QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized();
            bool dragged = std::max(box.width(), box.height()) * s_.view.screenRadius(1) > 3;
            if (boxTool_) {
                if (dragged) { box_ = box.toAlignedRect() & s_.doc.image(0).rect(); boxLayer_ = s_.doc.current(); frame_.reset(); }
                else if (!box_.contains(box.topLeft().toPoint())) box_ = QRect();
            } else if (dragged) {
                auto in = s_.doc.pathsIn(s_.doc.current(), box);
                picked_.insert(in.begin(), in.end());
            }
        }
        update();
    }
    // Select: the Transform box (#206): a 1 px frame, eight 8 px squares on its corners and edge midpoints (grabbable, #231), and the rotate circle (5 px, 24 px up a stem).
    void drawTransformBox(QPainter& p, const BoxFrame& f) {
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
                if (fx != 0.5 || fy != 0.5) p.drawRect(QRectF(b.left() + fx * b.width() - 4 * px, b.top() + fy * b.height() - 4 * px, 8 * px, 8 * px));
        p.restore();
    }
    // The Transform box: the frame a rotate drag left, else the picked paths' bounds, unturned (#202).
    BoxFrame frame() const {
        if (rasterBox()) return moveDrag_ ? frameAfter(moveFrame_, dragM_) : frame_ ? *frame_ : BoxFrame{QRectF(box_), 0};
        if (moveDrag_ && (rotating_ || frame_)) return frameAfter(moveFrame_, dragM_);
        return frame_ ? *frame_ : BoxFrame{pickedBounds(s_.doc.paths(s_.doc.current()), picked()), 0};
    }
    // Whether the screen point is within 6 px of the rotate circle, 24 px up the stem from the box's top edge midpoint.
    bool onRotateHandle(QPointF pos) const {
        BoxFrame f = frame();
        QPointF c = f.toDoc().map(QPointF(f.rect.center().x(), f.rect.top() - 24 / s_.view.screenRadius(1))), d = s_.view.toDoc(pos) - c;
        return std::hypot(d.x(), d.y()) <= 6 / s_.view.screenRadius(1);
    }
    // The picked paths follow the pointer live (no undo step yet).
    void dragMoveTo(QPointF pos, bool shift) {
        preview_ = [=, this] {
            dragM_ = rotating_ ? rotateMatrix(moveFrame_.rect.center(), moveStart_, s_.view.toDoc(pos)) : handle_ ? frameScaleMatrix(moveFrame_, *handle_, s_.view.toDoc(pos), shift) : QTransform::fromTranslate(s_.view.toDoc(pos).x() - moveStart_.x(), s_.view.toDoc(pos).y() - moveStart_.y());
            if (rasterBox()) {  // lift on the first drag; every drag resamples from the untouched pixels
                floating_ = s_.doc.floatRect(box_, floatM_ * dragM_);
                return;
            }
            auto moved = transformPaths(pathsBefore_, picked_, dragM_);  // live preview moves nodes only; widths follow on release
            for (int i : picked_) s_.doc.previewNodes(s_.doc.current(), i, moved[i].nodes, moved[i].closed);
        };
        update();
    }
    // Release: one undo step for the whole move, from the paths it started with.
    void endMoveDrag() {
        if (!moveDrag_) return;
        flushPreview();
        moveDrag_ = false;
        if (rasterBox()) {
            if (floating_) { floatM_ = floatM_ * dragM_; frame_ = frameAfter(moveFrame_, dragM_); }
            return;
        }
        if (dragM_.isIdentity()) return;
        for (int i : picked_) s_.doc.previewNodes(s_.doc.current(), i, pathsBefore_[i].nodes, pathsBefore_[i].closed);
        s_.doc.setPaths(s_.doc.current(), transformPaths(pathsBefore_, picked_, dragM_, scaleWidths_));
        if (rotating_ || frame_) frame_ = frameAfter(moveFrame_, dragM_);  // once rotated the box stays so, through moves and scales, until the next pick
        update();
    }
    // Right-click on Select with paths picked: Flip Horizontal / Vertical about the Transform box centre, each one undo step (#203).
    void flipMenu(QPoint globalPos) {
        if (picked().empty() && !rasterBox()) return;
        QMenu menu;
        auto* h = menu.addAction("Flip Horizontal");
        auto* v = menu.addAction("Flip Vertical");
        auto* c = menu.addAction("Convert to Bezier");
        auto ps = pickedPaths();
        c->setEnabled(std::any_of(ps.begin(), ps.end(), [](auto& p) { return p.kind == Document::PathKind::Spiro; }));
        auto* chosen = menu.exec(globalPos);
        if (chosen == c) return setBezier(true);  // what the Curve toggle's Bezier does (#317)
        if (chosen != h && chosen != v) return;
        BoxFrame f = frame();
        QTransform m = flipMatrix(f.rect.center(), chosen == h);
        if (rasterBox()) {  // joins the floating piece; the commit is the one undo step
            floatM_ = floatM_ * m;
            floating_ = s_.doc.floatRect(box_, floatM_);
            frame_ = frameAfter(f, m);
            return update();
        }
        if (!s_.doc.setPaths(s_.doc.current(), transformPaths(s_.doc.paths(s_.doc.current()), picked(), m, scaleWidths_))) return;
        if (frame_) frame_ = frameAfter(f, m);
        update();
    }
    // Delete: removes the picked paths as one undo step.
    void deletePicked() {
        auto& all = s_.doc.paths(s_.doc.current());
        std::vector<Document::VPath> kept;
        for (int i = 0; i < int(all.size()); i++) if (!picked().count(i)) kept.push_back(all[i]);
        if (picked().empty() || !s_.doc.setPaths(s_.doc.current(), kept)) return;
        picked_.clear();
        update();
        if (onLayersChange) onLayersChange();
    }
    // The paths Select has picked, on the layer they were picked on; none once another layer is current or the list got shorter.
    const std::set<int>& picked() const {
        static const std::set<int> none;
        bool ok = pickedLayer_ == s_.doc.current() && (picked_.empty() || *picked_.rbegin() < int(s_.doc.paths(pickedLayer_).size()));
        return ok ? picked_ : none;
    }
    // Replaces the selected path's nodes with one undo step; a set with no curve is refused and leaves the path as it was.
    void editNodes(const std::vector<SpiroNode>& nodes) {
        s_.doc.setNodes(s_.doc.current(), selected_, nodes, s_.doc.paths(s_.doc.current())[selected_].closed);
        update();
    }
    // Delete: drops the selected nodes unless fewer than 2 would remain.
    void deleteSelectedNodes() {
        if (selected_ < 0 || nodeSel_.empty()) return;
        if (bezierSelected()) {
            auto paths = s_.doc.paths(s_.doc.current());
            if (auto kept = removeBezier(paths[selected_], nodeSel_)) {
                paths[selected_] = *kept;
                s_.doc.setPaths(s_.doc.current(), paths);
                nodeSel_.clear();
            }
            return;
        }
        if (auto kept = removeNodes(s_.doc.nodes(s_.doc.current(), selected_), nodeSel_)) {
            editNodes(*kept);
            nodeSel_.clear();
        }
    }
    // The selected nodes follow the pointer live (no undo step yet).
    void dragNodesTo(QPointF pos) {
        preview_ = [=, this] { s_.doc.previewNodes(s_.doc.current(), selected_, moveNodes(pathBefore_.nodes, nodeSel_, s_.view.toDoc(pos) - nodeStart_), pathBefore_.closed); };
        update();
    }
    // A pen sends far more moves than a re-rasterize can keep up with, so a drag only queues its preview and paint runs the latest one.
    void flushPreview() {
        if (auto f = std::exchange(preview_, nullptr)) f();
    }
    // Release: one undo step for the whole drag, from the nodes it started with.
    void endNodeDrag() {
        if (!nodeDrag_) return;
        flushPreview();
        auto after = s_.doc.nodes(s_.doc.current(), selected_);
        s_.doc.previewNodes(s_.doc.current(), selected_, pathBefore_.nodes, pathBefore_.closed);
        auto& was = pathBefore_.nodes;
        if (!std::equal(after.begin(), after.end(), was.begin(), [](auto& a, auto& b) { return a.x == b.x && a.y == b.y; }))
            s_.doc.setNodes(s_.doc.current(), selected_, after, pathBefore_.closed);
        nodeDrag_ = false;
        update();
    }
    // The selected Bezier node and which handle (out = true) within 6 screen px of pos; a handle on its node's point is the node's (#313).
    std::optional<std::pair<int, bool>> handleAt(QPointF pos) {
        if (!selectTool_ || selected_ < 0) return std::nullopt;
        auto& b = s_.doc.paths(s_.doc.current())[selected_].bezier;
        for (int i : nodeSel_)
            for (bool out : {false, true}) {
                if (i >= int(b.size())) break;
                QPointF h = out ? b[i].out : b[i].in;
                if (h != b[i].p && QLineF(h, s_.view.toDoc(pos)).length() * s_.view.screenRadius(1) <= 6) return std::pair{i, out};
            }
        return std::nullopt;
    }
    // The dragged handle follows the pointer live (no undo step yet).
    void dragHandleTo(QPointF pos) {
        preview_ = [=, this] {
            auto v = pathBefore_;
            v.bezier[handleDrag_] = moveHandle(v.bezier[handleDrag_], handleOut_, s_.view.toDoc(pos));
            s_.doc.previewPath(s_.doc.current(), selected_, v);
        };
        update();
    }
    // Release: one undo step for the whole drag, from the path it started with.
    void endHandleDrag() {
        if (handleDrag_ < 0) return;
        flushPreview();
        auto paths = s_.doc.paths(s_.doc.current());
        s_.doc.previewPath(s_.doc.current(), selected_, pathBefore_);
        auto& a = paths[selected_].bezier[handleDrag_];
        auto& w = pathBefore_.bezier[handleDrag_];
        if (a.in != w.in || a.out != w.out) s_.doc.setPaths(s_.doc.current(), paths);
        handleDrag_ = -1;
        update();
    }
    // Index of the selected path's Width Knot handle within 6 screen px of pos, -1 for none.
    int knotAt(QPointF pos) {
        if (!selectTool_ || selected_ < 0) return -1;
        auto handles = s_.doc.knotHandles(s_.doc.current(), selected_);
        for (int i = 0; i < int(handles.size()); i++)
            if (QLineF(handles[i], s_.view.toDoc(pos)).length() * s_.view.screenRadius(1) <= 6) return i;
        return -1;
    }
    // Ctrl+click on a Width Knot handle duplicates the knot, Ctrl+Alt+click deletes it, Ctrl+click on the selected path off a handle adds one,
    // Ctrl+Shift+click there adds a Spiro Node of the picker's type (a Bezier path: a Smooth node on the curve) instead (one undo step each); false otherwise.
    bool editKnot(QPointF pos, Qt::KeyboardModifiers mods) {
        int i = knotAt(pos);
        if (i < 0) {
            if (!selectTool_ || selected_ < 0 || (mods & Qt::AltModifier) || s_.doc.pathAt(s_.doc.current(), s_.view.toDoc(pos), 6 / s_.view.screenRadius(1)) != selected_) return false;
            if (mods & Qt::ShiftModifier) {
                if (bezierSelected()) {
                    auto k = s_.doc.projectKnot(s_.doc.current(), selected_, s_.view.toDoc(pos));
                    auto paths = s_.doc.paths(s_.doc.current());
                    if (auto split = k ? splitBezier(paths[selected_], k->t) : std::nullopt) {
                        paths[selected_] = *split;
                        s_.doc.setPaths(s_.doc.current(), paths);
                        nodeSel_.clear();  // indexes after the new node moved
                    }
                } else s_.doc.addNode(s_.doc.current(), selected_, s_.view.toDoc(pos), editType_);
            }
            else s_.doc.addKnot(s_.doc.current(), selected_, s_.view.toDoc(pos));
            update();
            return true;
        }
        auto k = s_.doc.knots(s_.doc.current(), selected_);
        s_.doc.setKnots(s_.doc.current(), selected_, (mods & Qt::AltModifier) ? deleteKnot(k, i) : duplicateKnot(k, i));
        update();
        return true;
    }
    // The dragged knot follows an eighth of the pointer's motion live (no undo step yet).
    void dragKnotTo(QPointF pos) {
        preview_ = [=, this] {
            auto k = s_.doc.projectKnot(s_.doc.current(), selected_, knotDragTarget(knotStart_, knotPress_, s_.view.toDoc(pos)));
            if (!k) return;
            auto knots = knotsBefore_;
            knots[knotDrag_] = *k;
            s_.doc.previewKnots(s_.doc.current(), selected_, knots);
        };
        update();
    }
    // Release: one undo step for the whole drag, from the knots it started with.
    void endKnotDrag() {
        if (knotDrag_ < 0) return;
        flushPreview();
        auto after = s_.doc.knots(s_.doc.current(), selected_);
        s_.doc.previewKnots(s_.doc.current(), selected_, knotsBefore_);
        if (after.size() == knotsBefore_.size() && !std::equal(after.begin(), after.end(), knotsBefore_.begin(), [](auto& a, auto& b) { return a.t == b.t && a.width == b.width; }))
            s_.doc.setKnots(s_.doc.current(), selected_, after);
        knotDrag_ = -1;
        update();
    }
    void resizeTo(QPointF pos) {
        if (!brush_) return;
        setRadius(dragRadius(radius(), pos.x() - last_.x()));
        last_ = pos;
        if (onRadiusChange) onRadiusChange();
    }
    void pickAt(QPointF pos) {
        QPointF d = s_.view.toDoc(pos);
        QColor c = s_.doc.colorAt({int(std::floor(d.x())), int(std::floor(d.y()))});  // the pixel under the point, not the nearest corner
        if (!c.isValid() || !brush_ || selectTool_) return;  // Node/Knot edit: a Ctrl+click no knot or node gesture took does nothing
        setColor(c);
        if (onColorPick) onColorPick(c);
    }
    // Lasso Fill: collect the outline in document coordinates; the fill lands in endLasso. The eraser end is per stroke: its press decides.
    void lassoAt(QPointF pos, bool eraser) {
        if (!lassoing_) lassoErase_ = eraser, lassoSmooth_.q.clear();
        lassoing_ = true;
        QPointF d = s_.view.toDoc(pos);
        // ponytail: smoothed per pen event, no timer like the Brush's; the outline waits while the pen holds still
        lassoSmooth_.push({float(d.x()), float(d.y()), 1, 0, 0, 0.0});
        auto m = lassoSmooth_.sample();
        lasso_.push_back({m.x, m.y});
        update();
    }
    void endLasso() {
        if (!lassoing_) return;
        lassoing_ = false;
        if (lassoFinish_) for (auto& m : lassoSmooth_.finish()) lasso_.push_back({m.x, m.y});  // the shape closes at the pen-up point
        QColor c = color_;
        c.setAlphaF(lassoOpacity_);
        if (lasso_.size() > 2) s_.doc.fill(c, {lasso_.begin(), lasso_.end()}, lassoErase_);
        lasso_.clear();
        update();
    }
    void paintAt(QPointF pos, float pressure, float xtilt, float ytilt, ulong ms, bool eraser = false) {
        if (!brush_) return;
        QPointF d = s_.view.toDoc(pos);
        if (smooth_.n > 0) {
            // Smoothing: the pen only sets raw_; smoothTick paints the mean every 10 ms, so the line catches up while the pen holds still (/r/14).
            raw_ = {float(d.x()), float(d.y()), pressure, xtilt, ytilt, 0.0};
            smoothErase_ = eraser;
            if (painting_) return;
            smooth_.q.clear();
            smooth_.push(raw_);
            s_.doc.stroke(brush_, {smooth_.sample()}, false, eraser);  // no dead zone (#246)
            painting_ = true;
            if (!smoothTimer_) {
                smoothTimer_ = new QTimer(this);
                smoothTimer_->setInterval(10);
                QObject::connect(smoothTimer_, &QTimer::timeout, [this] { smoothTick(); });
            }
            smoothTimer_->start();
            return update();
        }
        s_.doc.stroke(brush_, {{float(d.x()), float(d.y()), pressure, xtilt, ytilt, painting_ ? (ms - lastMs_) / 1000.0 : 0.0}}, painting_, eraser);
        painting_ = true;
        lastMs_ = ms;
        update();
    }
    void smoothTick() {
        if (!painting_) {
            if (finishLine_) {
                auto tail = smooth_.finish();
                if (!tail.empty()) s_.doc.stroke(brush_, tail, true, smoothErase_);
                update();
            }
            return smoothTimer_->stop();
        }
        raw_.dt = 0.01;
        smooth_.push(raw_);
        s_.doc.stroke(brush_, {smooth_.sample()}, true, smoothErase_);
        update();
    }
    Session s_;
    Stabilizer smooth_;
    PenSample raw_{};  // the newest pen sample while smoothing, in document coordinates
    bool smoothErase_ = false, finishLine_ = true;
    QTimer* smoothTimer_ = nullptr;
    std::shared_ptr<MyPaintBrush> brush_ = loadBrush(BRUSH_DIR "/pencil.myb");
    ulong lastMs_ = 0;
    QPointF last_;
    QString hint_;
    std::optional<QPointF> cursor_;  // pointer position over the canvas, for the brush outline
    bool dragging_ = false, space_ = false, painting_ = false, resizing_ = false;
    QColor color_ = Qt::black;  // matches pencil.myb and the Color dock's start
    QPolygonF lasso_;
    Stabilizer lassoSmooth_;
    bool lassoFinish_ = true;
    SpiroPen pen_;
    bool penDrag_ = false;  // a Bezier press is down: moving pulls the new node's handle (#309)
    std::function<void()> preview_;  // the latest queued drag preview, run by flushPreview
    int knotDrag_ = -1;  // index of the Width Knot being dragged on the selected path, -1 for none
    std::vector<WidthKnot> knotsBefore_;  // the selected path's knots when the drag began
    QPointF knotPress_, knotStart_;  // doc-space press point and the dragged knot's handle there (#374)
    bool nodeDrag_ = false;  // a Spiro Node drag is under way
    QPointF nodeStart_;  // where it began, in document coordinates
    int handleDrag_ = -1;  // the Bezier node whose handle is being dragged (#313), -1 for none
    bool handleOut_ = false;  // its out handle, else its in handle
    Document::VPath pathBefore_;  // the selected path's nodes and closed when it began
    std::set<int> nodeSel_;  // selected Spiro Node indexes of the selected path
    std::set<int> picked_;  // Select's picked path indexes on layer pickedLayer_
    int pickedLayer_ = -1;
    bool moveDrag_ = false;  // a Transform box move or scale is under way
    std::optional<QPoint> handle_;  // the box handle being dragged (scale), none for a move
    BoxFrame moveFrame_;  // the box when a drag began
    std::optional<BoxFrame> frame_;  // the box a rotate left, until the next pick
    bool floating_ = false;  // a raster selection's piece is lifted: the layer shows box_'s pixels moved through floatM_, not yet an undo step
    QTransform floatM_;
    bool scaleWidths_ = false;  // Select's Scale widths: transforms scale Width Knot widths too
    bool rotating_ = false;  // the drag is a rotate
    QPointF moveStart_;  // where the drag began, in document coordinates
    QTransform dragM_;  // what the drag has done to the picked paths so far
    std::vector<Document::VPath> pathsBefore_;  // the current layer's paths when it began
    bool boxing_ = false;  // a Select box drag is under way
    QPointF boxStart_;  // where it began, in document coordinates
    QRect box_;  // Box Select's rectangle in document pixels, null for none; it belongs to layer boxLayer_
    SpiroType editType_ = SpiroType::G2;  // Node/Knot edit's picker choice: retypes selected nodes and types a node added with Ctrl+Shift+click
    int boxLayer_ = -1;
    int penLayer_ = -1;  // the layer the Spiro Pen's unfinished path was started on
    int selected_ = -1;  // index of the selected path on the current layer, -1 for none
    bool spiroTool_ = false, selectTool_ = false, objectTool_ = false, boxTool_ = false;
    double pathWidth_ = 10;  // px
    Document::PaintMode paintMode_ = Document::PaintMode::Stroke;
    QColor penStroke_ = Qt::black, penFill_ = QColor(0x2d, 0x5d, 0x9a);  // the Pen's Stroke and Fill swatches
    bool fillActive_ = false;
    float lassoOpacity_ = 1;
    bool fillTool_ = false, eraseBrush_ = false, eraseFill_ = false, lassoing_ = false, lassoErase_ = false;
};

// Hue ring around a saturation/value square. Dragging either calls onChange with the new color; setColor moves it from outside.
class ColorPicker : public QWidget {
public:
    std::function<void(const QColor&)> onChange;
    std::function<void()> onRelease;  // a drag ended
    ColorPicker() { setMinimumSize(180, 180); }
    void setColor(const QColor& c) {
        if (c.hsvHueF() >= 0) h_ = c.hsvHueF();  // grays have no hue: keep the last one
        s_ = c.hsvSaturationF();
        v_ = c.valueF();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QConicalGradient g(center(), 0);
        for (int i = 0; i <= 6; i++) g.setColorAt(i / 6.0, QColor::fromHsvF(i % 6 / 6.0, 1, 1));  // conical angles run counter-clockwise, like the hue math below
        p.setPen(QPen(QBrush(g), ring()));
        p.drawEllipse(center(), outer() - ring() / 2, outer() - ring() / 2);
        QImage sq(kN, kN, QImage::Format_RGB32);
        for (int y = 0; y < kN; y++)
            for (int x = 0; x < kN; x++) sq.setPixelColor(x, y, QColor::fromHsvF(h_, x / double(kN - 1), 1 - y / double(kN - 1)));
        p.drawImage(square(), sq);
        QPointF hue = center() + (outer() - ring() / 2) * QPointF(std::cos(h_ * 2 * M_PI), -std::sin(h_ * 2 * M_PI));
        QPointF sv = square().topLeft() + QPointF(s_ * square().width(), (1 - v_) * square().height());
        for (QPointF m : {hue, sv}) {  // white ring on black, visible on any color
            p.setPen(QPen(Qt::black, 3));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(m, 5, 5);
            p.setPen(QPen(Qt::white, 1.5));
            p.drawEllipse(m, 5, 5);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        QPointF d = e->position() - center();
        double r = std::hypot(d.x(), d.y());
        mode_ = r >= outer() - ring() && r <= outer() ? Ring : square().contains(e->position()) ? Square : None;
        mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        QPointF d = e->position() - center();
        if (mode_ == Ring) h_ = std::fmod(std::atan2(-d.y(), d.x()) / (2 * M_PI) + 1, 1.0);
        else if (mode_ == Square) {
            s_ = std::clamp((e->position().x() - square().left()) / square().width(), 0.0, 1.0);
            v_ = 1 - std::clamp((e->position().y() - square().top()) / square().height(), 0.0, 1.0);
        } else return;
        update();
        if (onChange) onChange(QColor::fromHsvF(h_, s_, v_));
    }
    void mouseReleaseEvent(QMouseEvent*) override { if (mode_ != None && onRelease) onRelease(); mode_ = None; }

private:
    static constexpr int kN = 64;  // resolution of the square's image, scaled up to fit
    enum Mode { None, Ring, Square } mode_ = None;
    double outer() const { return std::min(width(), height()) / 2.0 - 2; }  // the wheel fills the widget, whatever its size
    double ring() const { return outer() * 16 / 88; }
    QPointF center() const { return {width() / 2.0, height() / 2.0}; }
    QRectF square() const {  // the square inscribed in the ring's inner circle
        double half = (outer() - ring() - 4) / std::sqrt(2.0);
        return {center().x() - half, center().y() - half, 2 * half, 2 * half};
    }
    double h_ = 0, s_ = 0, v_ = 0;
};

// Navigation dock (#286): the composite render scaled to fit, outside the view dimmed, the view a white haloed outline turned and flipped as the view is (#322).
class Navigator : public QWidget {
public:
    explicit Navigator(Canvas* c) : canvas_(c) { setMinimumSize(160, 120); }
    void refresh() { thumb_ = {}; update(); }  // the drawing changed; the rectangle alone needs only update()

protected:
    void resizeEvent(QResizeEvent*) override { thumb_ = {}; }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(90, 90, 90));
        if (!canvas_->hasDoc()) return;
        QTransform toThumb = fit();
        QRectF r = toThumb.mapRect(QRectF(QPointF(), QSizeF(canvas_->doc().image(0).size())));
        if (thumb_.isNull()) thumb_ = canvas_->doc().render().scaled(r.size().toSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.fillRect(r, Qt::white);
        p.drawImage(r.topLeft(), thumb_);
        QTransform m = canvas_->session().view.matrix().inverted() * toThumb;  // canvas pixels -> thumbnail pixels
        QRectF c = canvas_->rect();
        QPolygonF view = m.map(QPolygonF(c));
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath outside;
        outside.addRect(r);
        outside.addPolygon(view);  // odd-even fill: the view's inside stays clear
        p.fillPath(outside, QColor(0, 0, 0, 115));
        QLineF tick(m.map(c.topLeft() + QPointF(c.width() / 4, 0)), m.map(c.topLeft() + QPointF(c.width() / 4, 1)));  // into the view from its top edge, a quarter in from the screen's left, so rotation and mirror show
        tick.setLength(6);
        for (auto [pen, w] : {std::pair{QColor(0, 0, 0, 160), 3.0}, {Qt::white, 1.0}}) {  // dark halo under the white line
            p.setPen(QPen(pen, w));
            p.drawPolygon(view);
            p.drawLine(tick);
        }
    }

    // Dragging inside the view rectangle pans the canvas by the same document distance (#287).
    void mousePressEvent(QMouseEvent* e) override {
        if (!canvas_->hasDoc()) return;
        QTransform m = canvas_->session().view.matrix().inverted() * fit();
        dragging_ = m.map(QPolygonF(QRectF(canvas_->rect()))).containsPoint(e->position(), Qt::OddEvenFill);
        last_ = e->position();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!dragging_) return;
        QTransform toDoc = fit().inverted();
        View& v = canvas_->session().view;
        v.pan(v.toView(toDoc.map(last_)) - v.toView(toDoc.map(e->position())));  // the dragged doc point lands where the last one was
        last_ = e->position();
        canvas_->update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }

private:
    QTransform fit() const {  // document pixels -> thumbnail pixels: scaled to fit, centred
        QSizeF ds = canvas_->doc().image(0).size();
        double k = std::min(width() / ds.width(), height() / ds.height());
        return QTransform::fromScale(k, k) * QTransform::fromTranslate((width() - ds.width() * k) / 2, (height() - ds.height() * k) / 2);
    }
    Canvas* canvas_;
    bool dragging_ = false;
    QPointF last_;
    QImage thumb_;  // the render at thumbnail size; null until the next paint rebuilds it
};

// The window saves its dock state in closeEvent: by aboutToQuit the floating docks are already hidden and would be saved closed.
struct Window : QMainWindow {
    std::function<bool()> canClose;  // false keeps the window open (the quit dialog was cancelled, #285)
    std::function<void()> onClose;
    void closeEvent(QCloseEvent* e) override { if (canClose && !canClose()) { e->ignore(); return; } if (onClose) onClose(); QMainWindow::closeEvent(e); }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("mydraw");  // QSettings keys the saved window state on these
    app.setApplicationName("mydraw");

    Window window;
    auto* canvas = new Canvas;
    // Tab bar over the canvas (#241): the Canvas holds the active tab's Session, tabs[] parks the rest; the active slot is empty.
    auto* tabBar = new QTabBar;
    auto* central = new QWidget;
    auto* centralCol = new QVBoxLayout(central);
    centralCol->setContentsMargins(0, 0, 0, 0);
    centralCol->setSpacing(0);
    centralCol->addWidget(tabBar);
    centralCol->addWidget(canvas);
    window.setCentralWidget(central);
    std::vector<Session> tabs;
    int active = -1;
    canvas->onHint = [&window](const QString& h) { window.statusBar()->showMessage(h); };

    auto* nav = new Navigator(canvas);
    // Zoom slider (#289): log scale, value = 100 * log2(percent), so 0..1164 spans 1..3200 %; it follows the view on every paint.
    struct ZoomSlider : QSlider {
        using QSlider::QSlider;
        std::function<void()> onDoubleClick;
        void mouseDoubleClickEvent(QMouseEvent*) override { if (onDoubleClick) onDoubleClick(); }
    };
    auto* zoom = new ZoomSlider(Qt::Horizontal);
    zoom->setRange(0, std::lround(100 * std::log2(3200)));
    auto* zoomValue = new QLabel;
    auto setZoom = [canvas](double pct) { canvas->session().view.setZoom(QRectF(canvas->rect()).center(), pct / 100); canvas->update(); };
    QObject::connect(zoom, &QSlider::valueChanged, [setZoom](int v) { setZoom(std::exp2(v / 100.0)); });
    zoom->onDoubleClick = [setZoom] { setZoom(100); };
    // Rotation slider (#290): -180..180 degrees as seen, so a mirrored view reads what the user sees (View::rotation).
    auto* rot = new ZoomSlider(Qt::Horizontal);
    rot->setRange(-180, 180);
    auto* rotValue = new QLabel;
    auto setRot = [canvas](int deg) { canvas->session().view.setRotation(QRectF(canvas->rect()).center(), deg); canvas->update(); };
    QObject::connect(rot, &QSlider::valueChanged, setRot);
    rot->onDoubleClick = [setRot] { setRot(0); };
    canvas->onPaint = [nav, canvas, zoom, zoomValue, rot, rotValue] {
        nav->update();
        if (!canvas->hasDoc()) return;
        double pct = canvas->session().view.zoom() * 100;
        QSignalBlocker b(zoom);  // blocked so the slider's int doesn't round the zoom back
        zoom->setValue(std::lround(100 * std::log2(pct)));
        zoomValue->setText(QString("%1 %").arg(pct, 0, 'f', pct < 10 ? 1 : 0));
        double deg = canvas->session().view.rotation();
        QSignalBlocker rb(rot);
        rot->setValue(std::lround(deg));
        rotValue->setText(QString("%1°").arg(std::lround(deg)));
    };
    auto* navPanel = new QWidget;
    auto* navCol = new QVBoxLayout(navPanel);
    navCol->addWidget(nav, 1);
    auto* zoomRow = new QHBoxLayout;
    zoomRow->addWidget(zoom);
    zoomRow->addWidget(zoomValue);
    navCol->addLayout(zoomRow);
    auto* rotRow = new QHBoxLayout;
    rotRow->addWidget(rot);
    rotRow->addWidget(rotValue);
    navCol->addLayout(rotRow);
    auto* navDock = new QDockWidget("Navigation", &window);
    navDock->setWidget(navPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, navDock);

    auto* picker = new ColorPicker;
    auto* colorDock =new QDockWidget("Color", &window);
    colorDock->setWidget(picker);
    window.addDockWidget(Qt::RightDockWidgetArea, colorDock);

    auto* brushList = new QListWidget;
    auto brushes = listBrushes(BRUSH_DIR);
    for (const auto& b : brushes) brushList->addItem(b.name);
    brushList->setCurrentRow(std::find_if(brushes.begin(), brushes.end(), [](const BrushEntry& b) { return b.name == "pencil"; }) - brushes.begin());  // matches Canvas's starting brush
    QObject::connect(brushList, &QListWidget::currentRowChanged, [canvas, brushes](int row) { if (row >= 0) canvas->setBrush(brushes[row].path); });
    auto* size = new QSlider(Qt::Horizontal);  // radius in px
    size->setRange(kMinRadius, kMaxRadius);
    size->setValue(std::lround(canvas->radius()));
    QObject::connect(size, &QSlider::valueChanged, [canvas](int v) { canvas->setRadius(v); });
    auto* sizeValue = new QLabel;
    canvas->onRadiusChange = [canvas, size, sizeValue] {
        QSignalBlocker b(size);  // blocked so the slider's int doesn't round the radius back
        size->setValue(std::lround(canvas->radius()));
        sizeValue->setText(QString("%1 px").arg(size->value()));
    };
    QObject::connect(size, &QSlider::valueChanged, [sizeValue](int v) { sizeValue->setText(QString("%1 px").arg(v)); });
    sizeValue->setText(QString("%1 px").arg(size->value()));
    auto* opacity = new QSlider(Qt::Horizontal);  // percent
    opacity->setRange(1, 100);
    opacity->setValue(std::lround(canvas->opacity() * 100));
    QObject::connect(opacity, &QSlider::valueChanged, [canvas](int v) { canvas->setOpacity(v / 100.f); });
    auto* opacityValue = new QLabel;
    QObject::connect(opacity, &QSlider::valueChanged, [opacityValue](int v) { opacityValue->setText(QString("%1 %").arg(v)); });
    opacityValue->setText(QString("%1 %").arg(opacity->value()));
    auto* brushPanel = new QWidget;
    auto* col = new QVBoxLayout(brushPanel);
    col->addWidget(brushList);
    auto* brushDock = new QDockWidget("Brushes", &window);
    brushDock->setWidget(brushPanel);

    // Layers dock: the list runs top layer first, so row = layerCount - 1 - index. refresh() rebuilds it from the Document.
    auto* layerList = new QListWidget;
    auto* layerOpacity = new QSlider(Qt::Horizontal);  // percent, of the current layer
    layerOpacity->setRange(0, 100);
    auto* layerOpacityValue = new QLabel;
    QObject::connect(layerOpacity, &QSlider::valueChanged, [layerOpacityValue](int v) { layerOpacityValue->setText(QString("%1 %").arg(v)); });
    std::function<void()> kindChanged;  // set with the Tools dock below; refresh() runs on every current-layer change
    std::function<void()> updateTitle;  // set with the File menu below; refresh() runs it after every edit, undo and redo
    std::vector<QAction*> docActions;  // enabled only while a document is open (#173); filled as the menus are built
    auto refresh = [canvas, nav, layerList, layerOpacity, layerOpacityValue, &kindChanged, &updateTitle] {
        Document& d = canvas->doc();
        nav->refresh();
        QSignalBlocker b1(layerList), b2(layerOpacity);
        layerList->clear();
        for (int i = d.layerCount() - 1; i >= 0; i--) {  // a row: eye, kind icon, thumbnail, name (#169)
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(2, 2, 2, 2);
            auto* eye = new QToolButton;
            eye->setAutoRaise(true);
            eye->setCheckable(true);
            eye->setChecked(d.visible(i));
            eye->setText(d.visible(i) ? "👁" : "–");
            QObject::connect(eye, &QToolButton::toggled, [canvas, eye, i](bool on) {
                eye->setText(on ? "👁" : "–");
                canvas->doc().setVisible(i, on);
                canvas->update();
            });
            auto* thumb = new QLabel;
            QPixmap tp(40, 30);
            tp.fill(QColor(200, 200, 200));  // so a transparent layer's thumbnail still shows
            QPainter tpp(&tp);
            tpp.setRenderHint(QPainter::SmoothPixmapTransform);
            tpp.drawImage(tp.rect(), d.image(i));
            thumb->setPixmap(tp);
            h->addWidget(eye);
            h->addWidget(new QLabel(d.vector(i) ? "✒" : "🖌"));
            h->addWidget(thumb);
            h->addWidget(new QLabel(d.name(i)), 1);
            auto* it = new QListWidgetItem(layerList);
            it->setSizeHint(row->sizeHint());
            layerList->setItemWidget(it, row);
        }
        layerList->setCurrentRow(d.layerCount() - 1 - d.current());
        layerOpacity->setValue(std::lround(d.opacity(d.current()) * 100));
        layerOpacityValue->setText(QString("%1 %").arg(layerOpacity->value()));
        canvas->update();
        if (kindChanged) kindChanged();
        if (updateTitle) updateTitle();
    };
    canvas->onLayersChange = refresh;
    QObject::connect(layerList, &QListWidget::currentRowChanged, [canvas, layerList, refresh](int row) {
        if (row < 0) return;
        canvas->doc().setCurrent(canvas->doc().layerCount() - 1 - row);
        refresh();
    });
    layerList->setContextMenuPolicy(Qt::CustomContextMenu);  // right-click a row: Rasterize, enabled on a Vector Layer only (#200)
    QObject::connect(layerList, &QListWidget::customContextMenuRequested, [canvas, layerList, refresh](QPoint pos) {
        int row = layerList->row(layerList->itemAt(pos));
        if (row < 0) return;
        int i = canvas->doc().layerCount() - 1 - row;
        QMenu menu;
        menu.addAction("Rasterize")->setEnabled(canvas->doc().vector(i));
        if (menu.exec(layerList->viewport()->mapToGlobal(pos)) && canvas->doc().rasterizeLayer(i)) refresh();
    });
    QObject::connect(layerOpacity, &QSlider::valueChanged, [canvas](int v) {
        canvas->doc().setOpacity(canvas->doc().current(), v / 100.0);
        canvas->update();
    });
    auto* layerButtons = new QGridLayout;
    struct LayerOp { const char* button; const char* menu; std::function<void(Document&)> op; };
    const std::initializer_list<LayerOp> layerOps{
        {"+ Raster", "Add &Raster", [](Document& d) { d.addLayer(); }},
        {"+ Vector", "Add &Vector", [](Document& d) { d.addVectorLayer(); }},
        {"↑", "&Up", [](Document& d) { if (d.moveLayer(d.current(), d.current() + 1)) d.setCurrent(d.current() + 1); }},
        {"↓", "Dow&n", [](Document& d) { if (d.moveLayer(d.current(), d.current() - 1)) d.setCurrent(d.current() - 1); }},
        {"🗑", "&Delete", [](Document& d) { d.deleteLayer(d.current()); }}};
    int layerOpIndex = 0;
    for (const auto& l : layerOps) {
        auto* btn = new QPushButton(l.button);
        btn->setMinimumWidth(1);  // the style's 80px floor would keep the dock about 450px wide; 0 counts as unset and keeps it
        QObject::connect(btn, &QPushButton::clicked, [canvas, refresh, op = l.op] { op(canvas->doc()); refresh(); });
        layerButtons->addWidget(btn, layerOpIndex / 3, layerOpIndex % 3);  // three per row
        layerOpIndex++;
    }
    auto* layerPanel = new QWidget;
    auto* layerCol = new QVBoxLayout(layerPanel);
    layerCol->addWidget(layerList);
    layerCol->addLayout(layerButtons);
    layerCol->addWidget(new QLabel("Opacity"));
    auto* layerOpacityRow = new QHBoxLayout;
    layerOpacityRow->addWidget(layerOpacity);
    layerOpacityRow->addWidget(layerOpacityValue);
    layerCol->addLayout(layerOpacityRow);
    auto* layerDock = new QDockWidget("Layers", &window);
    layerDock->setWidget(layerPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, layerDock);
    refresh();

    // Tool Options dock (#58, #171): Erase for Brush and Lasso Fill, path width and node picker for the Spiro Pen, node picker for Node/Knot edit.
    auto* eraseBox = new QCheckBox("Erase");
    auto* brushBox = new QWidget;  // the global Size and Opacity sliders (#209), for the Brush only
    auto* brushBoxCol = new QVBoxLayout(brushBox);
    brushBoxCol->setContentsMargins(0, 0, 0, 0);
    brushBoxCol->addWidget(new QLabel("Size"));
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(size);
    sizeRow->addWidget(sizeValue);
    brushBoxCol->addLayout(sizeRow);
    brushBoxCol->addWidget(new QLabel("Opacity"));
    auto* opacityRow = new QHBoxLayout;
    opacityRow->addWidget(opacity);
    opacityRow->addWidget(opacityValue);
    brushBoxCol->addLayout(opacityRow);
    brushBoxCol->addWidget(new QLabel("Smoothing"));
    auto* smoothRow = new QHBoxLayout;
    auto* smoothing = new QSlider(Qt::Horizontal);  // samples averaged, 0 = off
    smoothing->setRange(0, 50);
    auto* smoothingValue = new QLabel("0");
    QObject::connect(smoothing, &QSlider::valueChanged, [canvas, smoothingValue](int v) { canvas->setSmoothing(v); smoothingValue->setText(QString::number(v)); });
    smoothRow->addWidget(smoothing);
    smoothRow->addWidget(smoothingValue);
    brushBoxCol->addLayout(smoothRow);
    auto* finishLine = new QCheckBox("Finish Line");
    finishLine->setChecked(true);
    QObject::connect(finishLine, &QCheckBox::toggled, canvas, &Canvas::setFinishLine);
    brushBoxCol->addWidget(finishLine);
    auto* sensors = new QCheckBox("Stabilize Sensors");
    QObject::connect(sensors, &QCheckBox::toggled, canvas, &Canvas::setStabilizeSensors);
    brushBoxCol->addWidget(sensors);
    auto* lassoBox = new QWidget;  // Lasso Fill's own Opacity (#212), starting solid
    auto* lassoCol = new QVBoxLayout(lassoBox);
    lassoCol->setContentsMargins(0, 0, 0, 0);
    auto* lassoRow = new QHBoxLayout;
    lassoCol->addLayout(lassoRow);
    auto* lassoOpacity = new QSlider(Qt::Horizontal);  // percent
    lassoOpacity->setRange(1, 100);
    auto* lassoOpacityValue = new QLabel;
    QObject::connect(lassoOpacity, &QSlider::valueChanged, [canvas, lassoOpacityValue](int v) { canvas->setLassoOpacity(v / 100.f); lassoOpacityValue->setText(QString("%1 %").arg(v)); });
    lassoOpacity->setValue(100);
    lassoRow->addWidget(new QLabel("Opacity"));
    lassoRow->addWidget(lassoOpacity);
    lassoRow->addWidget(lassoOpacityValue);
    lassoCol->addWidget(new QLabel("Smoothing"));  // Lasso Fill's own Smoothing and Finish Line (#264)
    auto* lassoSmoothRow = new QHBoxLayout;
    auto* lassoSmoothing = new QSlider(Qt::Horizontal);
    lassoSmoothing->setRange(0, 50);
    auto* lassoSmoothingValue = new QLabel("0");
    QObject::connect(lassoSmoothing, &QSlider::valueChanged, [canvas, lassoSmoothingValue](int v) { canvas->setLassoSmoothing(v); lassoSmoothingValue->setText(QString::number(v)); });
    lassoSmoothRow->addWidget(lassoSmoothing);
    lassoSmoothRow->addWidget(lassoSmoothingValue);
    lassoCol->addLayout(lassoSmoothRow);
    auto* lassoFinish = new QCheckBox("Finish Line");
    lassoFinish->setChecked(true);
    QObject::connect(lassoFinish, &QCheckBox::toggled, canvas, &Canvas::setLassoFinishLine);
    lassoCol->addWidget(lassoFinish);
    auto* widthBox = new QWidget;
    auto* widthRow = new QHBoxLayout(widthBox);
    widthRow->setContentsMargins(0, 0, 0, 0);
    auto* pathWidth = new QSlider(Qt::Horizontal);  // px, of a new Vector Path
    pathWidth->setRange(kMinRadius, kMaxRadius);
    auto* pathWidthValue = new QLabel;
    // On picks a drag applies once, on release: one undo step (#305)
    QObject::connect(pathWidth, &QSlider::valueChanged, [canvas, pathWidth, pathWidthValue](int v) {
        if (!pathWidth->isSliderDown() || canvas->pickedPaths().empty()) canvas->setPathWidth(v);
        pathWidthValue->setText(QString("%1 px").arg(v));
    });
    QObject::connect(pathWidth, &QSlider::sliderReleased, [canvas, pathWidth] { canvas->setPathWidth(pathWidth->value()); });
    pathWidth->setValue(10);
    widthRow->addWidget(new QLabel("Width"));
    widthRow->addWidget(pathWidth);
    widthRow->addWidget(pathWidthValue);
    auto* paintBox = new QWidget;  // Paint mode of a new Vector Path, segmented (#323 take A)
    auto* paintRow = new QHBoxLayout(paintBox);
    paintRow->setContentsMargins(0, 0, 0, 0);
    paintRow->setSpacing(0);
    paintRow->addWidget(new QLabel("Paint"));
    const std::pair<const char*, Document::PaintMode> kPaints[] = {{"Stroke", Document::PaintMode::Stroke}, {"Fill", Document::PaintMode::Fill}, {"Stroke+Fill", Document::PaintMode::Both}};
    QToolButton* paintButtons[3];
    for (int k = 0; k < 3; k++) {
        auto [name, mode] = kPaints[k];
        auto* b = paintButtons[k] = new QToolButton;
        b->setText(name);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(mode == Document::PaintMode::Stroke);
        QObject::connect(b, &QToolButton::toggled, [canvas, mode](bool on) { if (on) canvas->setPaintMode(mode); });
        paintRow->addWidget(b);
    }
    auto* swatchBox = new QWidget;  // the Pen's Stroke and Fill swatches side by side; the checked one, ringed orange, is the Color dock's target (#323 take A)
    auto* swatchRow = new QHBoxLayout(swatchBox);
    swatchRow->setContentsMargins(0, 0, 0, 0);
    QToolButton* swatches[2];
    for (int i = 0; i < 2; i++) {
        auto* b = swatches[i] = new QToolButton;
        b->setText(i ? "Fill" : "Stroke");
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(24, 24));
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(!i);
        b->setStyleSheet("QToolButton { border: 2px solid transparent; } QToolButton:checked { border-color: #ffb84d; }");
        swatchRow->addWidget(b);
    }
    swatchRow->addStretch();
    // In Select with paths picked the swatches show theirs, hatched where they differ (#303); otherwise the Pen's.
    auto paintSwatches = [canvas, swatches] {
        auto ps = canvas->selecting() ? canvas->pickedPaths() : std::vector<Document::VPath>{};
        for (int i = 0; i < 2; i++) {
            QColor c = i ? canvas->penFill() : canvas->penStroke();
            bool mixed = false;
            if (!ps.empty()) c = i ? ps[0].fillColor : ps[0].color;
            for (const auto& p : ps) mixed |= (i ? p.fillColor : p.color) != c;
            QPixmap px(24, 24);
            px.fill(mixed ? Qt::white : c);
            QPainter painter(&px);
            if (mixed) painter.fillRect(px.rect(), Qt::BDiagPattern);
            painter.drawRect(0, 0, 23, 23);
            painter.end();
            QIcon icon(px);
            icon.addPixmap(px, QIcon::Disabled);  // a pick's colors stay readable while the box is display only
            swatches[i]->setIcon(icon);
        }
    };
    paintSwatches();
    auto nameColorDock = [colorDock, swatches](int tool) { colorDock->setWindowTitle(tool != SpiroPenTool && tool != SelectTool ? "Color" : swatches[1]->isChecked() ? "Color: Fill" : "Color: Stroke"); };
    for (int i = 0; i < 2; i++)
        QObject::connect(swatches[i], &QToolButton::toggled, [=](bool on) { if (!on) return; canvas->setFillActive(i); picker->setColor(canvas->color()); nameColorDock(SpiroPenTool); });
    // With picks, a drag recolors them once on release, so the drag is one undo step (#304).
    auto pending = std::make_shared<std::optional<QColor>>();
    picker->onChange = [canvas, paintSwatches, pending](const QColor& c) {
        if (canvas->selecting() && !canvas->pickedPaths().empty()) *pending = c;
        else canvas->setColor(c), paintSwatches();
    };
    picker->onRelease = [canvas, pending] { if (*pending) canvas->setColor(**pending); pending->reset(); };
    canvas->onColorPick = [picker, paintSwatches](const QColor& c) { picker->setColor(c); paintSwatches(); };
    auto* penTypes = new QListWidget;  // the Spiro Pen's type for new nodes
    auto* curveBox = new QWidget;  // the Pen's Spiro / Bezier toggle, Spiro by default (#308); Bezier has no node picker
    QToolButton* curveButtons[2];
    auto* curveRow = new QHBoxLayout(curveBox);
    curveRow->setContentsMargins(0, 0, 0, 0);
    curveRow->setSpacing(0);
    curveRow->addWidget(new QLabel("Curve"));
    for (bool bez : {false, true}) {
        auto* b = new QToolButton;
        b->setText(bez ? "Bezier" : "Spiro");
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(!bez);
        curveButtons[bez] = b;
        QObject::connect(b, &QToolButton::toggled, [canvas, penTypes, bez](bool on) { if (on) { canvas->setBezier(bez); penTypes->setVisible(!bez && !canvas->selecting()); } });
        curveRow->addWidget(b);
    }
    curveRow->addStretch();
    auto* editTypes = new QListWidget;  // retypes Node/Knot edit's selected nodes, apart from the pen's (#215)
    for (auto* l : {penTypes, editTypes}) for (const auto& t : kNodeTypes) l->addItem(t.name);
    penTypes->setCurrentRow(2);  // G2, SpiroPen's starting type
    QObject::connect(penTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->setNodeType(kNodeTypes[row].type); });
    QObject::connect(editTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->retypeSelected(kNodeTypes[row].type); });
    auto* bezierTypes = new QListWidget;  // Node/Knot edit's list when a Bezier path is selected (#314)
    const BezierType kBezierTypes[] = {BezierType::Cusp, BezierType::Smooth, BezierType::Symmetric};
    bezierTypes->addItems({"Cusp", "Smooth", "Symmetric"});
    QObject::connect(bezierTypes, &QListWidget::currentRowChanged, [canvas, kBezierTypes](int row) { if (row >= 0) canvas->retypeSelected(kBezierTypes[row]); });
    auto* scaleWidths = new QCheckBox("Scale widths");  // Select's, off by default (#203)
    QObject::connect(scaleWidths, &QCheckBox::toggled, [canvas](bool on) { canvas->setScaleWidths(on); });
    auto showOptions = [=](int tool) {
        scaleWidths->setVisible(tool == SelectTool);
        eraseBox->setVisible(tool <= LassoFillTool);
        brushBox->setVisible(tool == BrushTool);
        lassoBox->setVisible(tool == LassoFillTool);
        widthBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
        paintBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
        swatchBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
        picker->setColor(canvas->color());  // the Pen's active swatch or the brush color
        nameColorDock(tool);
        curveBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
        penTypes->setVisible(tool == SpiroPenTool && !canvas->bezier());
        editTypes->setVisible(tool == NodeKnotEditTool && !canvas->bezierSelected());
        bezierTypes->setVisible(tool == NodeKnotEditTool && canvas->bezierSelected());
    };
    // Select shows its picks' Width, Paint and colors, "mixed" where they differ; no pick shows the Pen's next path (#303).
    auto showPicks = [=] {
        auto ps = canvas->pickedPaths();
        double w = ps.empty() ? canvas->pathWidth() : ps[0].width;
        auto mode = ps.empty() ? canvas->paintMode() : ps[0].mode;
        bool wMixed = false, mMixed = false;
        for (const auto& p : ps) { wMixed |= p.width != w; mMixed |= p.mode != mode; }
        QSignalBlocker b(pathWidth);
        pathWidth->setValue(std::lround(w));
        pathWidthValue->setText(wMixed ? "mixed" : QString("%1 px").arg(std::lround(w)));
        for (int k = 0; k < 3; k++) {
            QSignalBlocker bb(paintButtons[k]);
            paintButtons[k]->setAutoExclusive(false);  // so a mixed pick can leave all three unchecked
            paintButtons[k]->setChecked(!mMixed && kPaints[k].second == mode);
            paintButtons[k]->setAutoExclusive(true);
        }
        bool hasSpiro = false, hasBezier = false;
        for (const auto& p : ps) (p.kind == Document::PathKind::Bezier ? hasBezier : hasSpiro) = true;
        if (ps.empty()) hasBezier = canvas->bezier(), hasSpiro = !hasBezier;
        for (int k = 0; k < 2; k++) {
            QSignalBlocker bb(curveButtons[k]);
            curveButtons[k]->setAutoExclusive(false);  // so a mixed pick leaves both unchecked
            curveButtons[k]->setChecked(k ? hasBezier && !hasSpiro : hasSpiro && !hasBezier);
            curveButtons[k]->setAutoExclusive(true);
        }
        paintSwatches();
    };
    canvas->onToolChange = [=](int tool) { showOptions(tool); showPicks(); };  // setTool has cleared the picks, so leaving Select shows the Pen's again
    showOptions(BrushTool);
    auto onPaintBefore = canvas->onPaint;
    // ponytail: re-reads the picks on every repaint; a pick-changed signal if drags ever stutter
    canvas->onPaint = [=] {
        onPaintBefore();
        if (canvas->selecting()) showPicks();
        if (canvas->editingNodes()) editTypes->setVisible(!canvas->bezierSelected()), bezierTypes->setVisible(canvas->bezierSelected());
    };
    auto* optionsPanel = new QWidget;
    auto* optionsCol = new QVBoxLayout(optionsPanel);
    optionsCol->addWidget(eraseBox);
    optionsCol->addWidget(brushBox);
    optionsCol->addWidget(lassoBox);
    optionsCol->addWidget(curveBox);
    optionsCol->addWidget(widthBox);
    optionsCol->addWidget(paintBox);
    optionsCol->addWidget(swatchBox);
    optionsCol->addWidget(scaleWidths);
    optionsCol->addWidget(penTypes);
    optionsCol->addWidget(editTypes);
    optionsCol->addWidget(bezierTypes);
    auto* optionsDock = new QDockWidget("Tool Options", &window);
    optionsDock->setWidget(optionsPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, optionsDock);

    // Tools dock (#154): one checkable action per tool, exclusive, its key a window-wide shortcut so it works whichever dock has focus.
    auto* toolPanel = new QWidget;
    auto* toolCol = new QVBoxLayout(toolPanel);
    auto* toolGroup = new QActionGroup(&window);
    const struct { const char* text; const char* key; } kTools[] = {
        {"\U0001F58C Brush", "B"}, // Brush (B)
        {"\u2B2E Fill", "F"}, // Lasso Fill (F)
        {"\u2712 Pen", "P"}, // Spiro Pen (P)
        {"\u2316 Node", "N"}, // Node/Knot edit (N)
        {"\u25AD Select", "S"}, // Select (S)
        {"\u2B1A Box", ""} // Box Select
    };
    int lastTool[2] = {BrushTool, SpiroPenTool};  // last tool used on a Raster, a Vector Layer (#161); Select fits both, so it is no one's last
    for (int i = 0; i < 6; i++) {
        auto* a = toolGroup->addAction(kTools[i].text);
        a->setCheckable(true);
        a->setChecked(i == 0);
        a->setShortcut(QKeySequence(kTools[i].key));
        window.addAction(a);
        QObject::connect(a, &QAction::triggered, [canvas, i, &lastTool] { canvas->setTool(i); if (i != SelectTool) lastTool[toolFitsKind(true, i)] = i; });
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolCol->addWidget(b);
    }
    toolCol->addStretch();
    // Only the tools that fit the current layer's kind are enabled, so their keys do nothing on the other kind (#170); a switch that disables the tool picks the last one used on the new kind.
    kindChanged = [&] {
        bool has = canvas->hasDoc(), vec = canvas->doc().vector(canvas->doc().current());
        canvas->setEnabled(has);  // a disabled widget gets no pointer or key events
        for (auto* w : {colorDock->widget(), layerPanel, optionsPanel, navPanel}) w->setEnabled(has);
        for (auto* a : docActions) a->setEnabled(has);
        auto tools = toolGroup->actions();
        for (int i = 0; i < 6; i++) tools[i]->setEnabled(has && toolFitsKind(vec, i));
        brushPanel->setEnabled(has && !vec);
        int now = toolForLayer(vec, tools.indexOf(toolGroup->checkedAction()), lastTool[0], lastTool[1]);
        if (!tools[now]->isChecked()) tools[now]->trigger();
    };
    kindChanged();
    auto* toolDock = new QDockWidget("Tools", &window);
    toolDock->setWidget(toolPanel);
    window.addDockWidget(Qt::LeftDockWidgetArea, toolDock);
    window.addDockWidget(Qt::LeftDockWidgetArea, brushDock);  // layout B: Brushes below Tools, bottom-left

    auto* fileMenu = window.menuBar()->addMenu("&File");
    updateTitle = [&] {  // "name[*] - mydraw"; Qt shows the [*] as * while the window is modified (#278)
        if (!canvas->hasDoc()) { window.setWindowTitle("mydraw"); window.setWindowModified(false); return; }
        QString name = canvas->session().path.isEmpty() ? QString("Untitled %1").arg(canvas->session().untitled) : QFileInfo(canvas->session().path).fileName();
        bool dirty = canvas->doc().stepId() != canvas->session().saved;
        window.setWindowTitle(name + "[*] - mydraw");
        window.setWindowModified(dirty);
        tabBar->setTabText(active, dirty ? name + "*" : name);
    };
    int untitledCount = 0;
    auto openTab = [&](Document doc, QString path) {  // New and Open: a new tab, made active
        if (active >= 0) tabs[active] = canvas->takeSession();
        tabs.emplace_back();
        active = int(tabs.size()) - 1;
        { QSignalBlocker b(tabBar); tabBar->setCurrentIndex(tabBar->addTab({})); }
        canvas->setDocument(std::move(doc), path);
        if (path.isEmpty()) canvas->session().untitled = ++untitledCount;
        updateTitle();
    };
    QObject::connect(tabBar, &QTabBar::currentChanged, [&](int i) {
        if (i < 0 || i == active) return;
        tabs[active] = canvas->takeSession();
        active = i;
        canvas->setSession(std::move(tabs[i]));
        updateTitle();
    });
    std::function<bool()> saveActive;  // Save on the active tab; false if cancelled or failed (set once Save exists)
    auto closeTab = [&](int i) {  // a dirty tab asks Save / Discard / Cancel (#262); the last close returns to the no-document start (#157)
        tabBar->setCurrentIndex(i);  // a background tab is shown before it is asked about
        if (canvas->doc().stepId() != canvas->session().saved) {
            auto b = QMessageBox::question(&window, "Close", tabBar->tabText(i).chopped(1) + " has unsaved changes.", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (b == QMessageBox::Cancel || (b == QMessageBox::Save && !saveActive())) return;
        }
        bool wasActive = i == active;
        if (wasActive) canvas->takeSession();  // ends a lifted piece or unfinished path; the session itself is dropped
        tabs.erase(tabs.begin() + i);
        { QSignalBlocker b(tabBar); tabBar->removeTab(i); }
        active = tabBar->currentIndex();
        if (active < 0) canvas->setDocument(Document(QSize()));
        else if (wasActive) canvas->setSession(std::move(tabs[active]));  // the bar's new current tab moves into the Canvas
        updateTitle();
    };
    tabBar->setTabsClosable(true);
    QObject::connect(tabBar, &QTabBar::tabCloseRequested, closeTab);
    auto* fileNew = fileMenu->addAction("&New...");
    fileNew->setShortcut(QKeySequence::New);
    QObject::connect(fileNew, &QAction::triggered, [&] {  // take B of #380: preset list, size fields, orientation, preview
        struct SizeBox : QSpinBox {  // Enter on a bad size does nothing (#383); QSpinBox would restore the old value and let OK fire
            void keyPressEvent(QKeyEvent* e) override {
                if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !hasAcceptableInput()) return;
                QSpinBox::keyPressEvent(e);
            }
            QLineEdit* edit() { return lineEdit(); }  // its textChanged fires on typed edits; QSpinBox's only on valid text
        };
        struct Preset { const char* group; const char* name; int w, h; };
        static const Preset presets[] = {  // #384, #388; paper sizes at 300 ppi (/r/16)
            {"Screen", "720p", 1280, 720}, {"Screen", "1080p", 1920, 1080}, {"Screen", "1440p", 2560, 1440}, {"Screen", "4K UHD", 3840, 2160},
            {"Paper, 300 ppi", "A5", 1748, 2480}, {"Paper, 300 ppi", "A4", 2480, 3508}, {"Paper, 300 ppi", "US Letter", 2550, 3300},
            {"Square", "Square 1024", 1024, 1024}, {"Square", "Square 2048", 2048, 2048},
            {"Other", "1000 × 1500", 1000, 1500}, {"Other", "Golden ratio", 1618, 1000},
        };
        QDialog dlg(&window);
        dlg.setWindowTitle("New");
        auto* list = new QListWidget;
        list->setFixedWidth(220);
        list->setMinimumHeight(18 * 15 + 4);  // every row shows: 11 presets, 4 group headers
        QString group;
        for (auto& p : presets) {
            if (group != p.group) (new QListWidgetItem(group = p.group, list))->setFlags(Qt::NoItemFlags);  // a header row
            QString size = QString("%1 × %2").arg(p.w).arg(p.h);
            auto* it = new QListWidgetItem(size == p.name ? size : QString("%1   %2").arg(p.name, size));
            it->setData(Qt::UserRole, QSize(p.w, p.h));
            list->addItem(it);
        }
        auto* ws = new SizeBox; auto* hs = new SizeBox;
        for (auto* s : {ws, hs}) { s->setRange(1, 16384); s->setSuffix(" px"); }  // cap #383
        auto* swap = new QToolButton; swap->setText("⇄");
        auto* portrait = new QToolButton; portrait->setText("Portrait"); portrait->setCheckable(true);
        auto* landscape = new QToolButton; landscape->setText("Landscape"); landscape->setCheckable(true);
        auto* preview = new QLabel; preview->setFixedSize(160, 140); preview->setAlignment(Qt::AlignCenter);
        auto* info = new QLabel;
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        auto* form = new QGridLayout;
        form->addWidget(new QLabel("Width"), 0, 0); form->addWidget(ws, 0, 1);
        form->addWidget(new QLabel("Height"), 1, 0); form->addWidget(hs, 1, 1); form->addWidget(swap, 1, 2);
        auto* orient = new QHBoxLayout; orient->addWidget(portrait); orient->addWidget(landscape);
        form->addWidget(new QLabel("Orientation"), 2, 0); form->addLayout(orient, 2, 1, 1, 2);
        form->addWidget(preview, 3, 0, 1, 3); form->addWidget(info, 4, 0, 1, 3);
        form->setRowStretch(5, 1);
        auto* body = new QHBoxLayout; body->addWidget(list); body->addLayout(form);
        auto* col = new QVBoxLayout(&dlg); col->addLayout(body); col->addWidget(btns);

        auto sync = [=] {  // fields changed: OK, row highlight, orientation, preview and memory line follow
            bool good = ws->hasAcceptableInput() && hs->hasAcceptableInput();
            btns->button(QDialogButtonBox::Ok)->setEnabled(good);
            QSize sz(ws->value(), hs->value());
            { QSignalBlocker b(list); list->setCurrentRow(-1);
              for (int i = 0; i < list->count(); ++i) if (good && list->item(i)->data(Qt::UserRole).toSize() == sz) list->setCurrentRow(i); }
            portrait->setChecked(good && sz.width() < sz.height()); landscape->setChecked(good && sz.width() >= sz.height());
            QPixmap pm(preview->size()); pm.fill(QColor(42, 42, 42));
            if (good) { QPainter p(&pm); QSize r = sz.scaled(140, 120, Qt::KeepAspectRatio).expandedTo({2, 2}); p.fillRect(QRect(QPoint(80 - r.width() / 2, 70 - r.height() / 2), r), Qt::white); }
            preview->setPixmap(pm);
            info->setText(good ? QString("%1 × %2 px · Background layer %3 MB").arg(sz.width()).arg(sz.height()).arg(sz.width() * double(sz.height()) * 4 / 1048576, 0, 'f', 1) : "");  // RGBA8888 (src/document.cpp:64)
        };
        for (auto* s : {ws, hs}) {  // typing reaches the lineEdit; setValue (a row, swap) only valueChanged
            QObject::connect(s->edit(), &QLineEdit::textChanged, sync);
            QObject::connect(s, &QSpinBox::valueChanged, sync);
        }
        QObject::connect(list, &QListWidget::currentRowChanged, [=](int row) {
            QSize sz = row < 0 ? QSize() : list->item(row)->data(Qt::UserRole).toSize();
            if (sz.isValid()) { ws->setValue(sz.width()); hs->setValue(sz.height()); }
        });
        auto flip = [=] { int w = ws->value(); ws->setValue(hs->value()); hs->setValue(w); };
        QObject::connect(swap, &QToolButton::clicked, flip);
        QObject::connect(portrait, &QToolButton::clicked, [=] { if (ws->value() >= hs->value()) flip(); sync(); });
        QObject::connect(landscape, &QToolButton::clicked, [=] { if (ws->value() < hs->value()) flip(); sync(); });
        QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        QSize last = QSettings().value("newSize", QSize(1920, 1080)).toSize();  // the last size OK'd (#385)
        ws->setValue(last.width()); hs->setValue(last.height());
        sync();
        if (dlg.exec() != QDialog::Accepted) return;
        QSize sz(ws->value(), hs->value());
        QSettings().setValue("newSize", sz);
        openTab(Document(sz), {});
    });

    auto* fileOpen = fileMenu->addAction("&Open...");
    fileOpen->setShortcut(QKeySequence::Open);
    QObject::connect(fileOpen, &QAction::triggered, [&] {
        QString path = QFileDialog::getOpenFileName(&window, "Open", {}, "OpenRaster (*.ora)");
        if (path.isEmpty()) return;
        auto doc = readOra(path);
        if (!doc) { QMessageBox::warning(&window, "Open", "Could not read " + path); return; }
        openTab(std::move(*doc), path);
    });
    auto saveTo = [&](QString path) {
        if (path.isEmpty()) return false;
        if (!path.endsWith(".ora")) path += ".ora";
        if (!writeOra(canvas->doc(), path)) { QMessageBox::warning(&window, "Save", "Could not write " + path); return false; }
        canvas->session().path = path;
        canvas->session().saved = canvas->doc().stepId();
        updateTitle();
        return true;
    };
    auto* fileSave = fileMenu->addAction("&Save");
    docActions.push_back(fileSave);
    fileSave->setShortcut(QKeySequence::Save);
    saveActive = [&] { return saveTo(!canvas->session().path.isEmpty() ? canvas->session().path : QFileDialog::getSaveFileName(&window, "Save", {}, "OpenRaster (*.ora)")); };
    QObject::connect(fileSave, &QAction::triggered, [&] { saveActive(); });
    auto* fileSaveAs = fileMenu->addAction("Save &As...");
    docActions.push_back(fileSaveAs);
    fileSaveAs->setShortcut(QKeySequence("Ctrl+Shift+S"));
    QObject::connect(fileSaveAs, &QAction::triggered, [&] {
        saveTo(QFileDialog::getSaveFileName(&window, "Save As", canvas->session().path, "OpenRaster (*.ora)"));
    });
    auto* fileExport = fileMenu->addAction("&Export PNG...");
    docActions.push_back(fileExport);
    QObject::connect(fileExport, &QAction::triggered, [&] {
        QString path = QFileDialog::getSaveFileName(&window, "Export PNG", {}, "PNG (*.png)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".png")) path += ".png";
        if (!exportPng(canvas->doc(), path)) QMessageBox::warning(&window, "Export PNG", "Could not write " + path);
    });

    auto* fileClose = fileMenu->addAction("&Close");
    docActions.push_back(fileClose);
    fileClose->setShortcut(QKeySequence::Close);
    QObject::connect(fileClose, &QAction::triggered, [&] { closeTab(active); });
    auto* fileQuit = fileMenu->addAction("&Quit");
    fileQuit->setShortcut(QKeySequence::Quit);
    QObject::connect(fileQuit, &QAction::triggered, &window, &QWidget::close);
    window.canClose = [&] {  // dirty tabs are listed as a checklist, all checked: the default button saves the checked and discards the rest (#324)
        auto session = [&](int i) -> Session& { return i == active ? canvas->session() : tabs[i]; };  // the active slot of tabs[] is empty
        std::vector<int> dirty;
        for (int i = 0; i < int(tabs.size()); ++i) if (session(i).doc.stepId() != session(i).saved) dirty.push_back(i);
        if (dirty.empty()) return true;
        QDialog dlg(&window);
        dlg.setWindowTitle("Quit");
        auto* col = new QVBoxLayout(&dlg);
        col->addWidget(new QLabel("Save changes before quitting?\nUnchecked documents are closed without saving."));
        auto* list = new QListWidget;
        for (int i : dirty) {
            QString path = session(i).path;
            auto* it = new QListWidgetItem(tabBar->tabText(i).chopped(1) + "\n" + (path.isEmpty() ? "never saved: will ask where to save" : path), list);
            it->setCheckState(Qt::Checked);
        }
        col->addWidget(list);
        auto* btns = new QDialogButtonBox;
        auto* go = btns->addButton("Save All", QDialogButtonBox::AcceptRole);
        btns->addButton(QDialogButtonBox::Cancel);
        go->setDefault(true);
        col->addWidget(btns);
        auto checked = [&] { std::vector<int> c; for (int r = 0; r < list->count(); ++r) if (list->item(r)->checkState() == Qt::Checked) c.push_back(dirty[r]); return c; };
        QObject::connect(list, &QListWidget::itemChanged, [&] {
            int n = int(checked().size()), m = int(dirty.size()) - n;
            go->setText(!n ? "Discard All" : m ? QString("Save %1, Discard %2").arg(n).arg(m) : "Save All");
        });
        QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return false;
        for (int i : checked()) {
            tabBar->setCurrentIndex(i);  // the tab being saved is shown, and its Save path dialog asks over it
            if (!saveActive()) return false;  // a cancelled path dialog stops the quit
        }
        return true;
    };

    auto key = [&window, &docActions](QAction* a, QList<QKeySequence> keys) { a->setShortcuts(keys); window.addAction(a); docActions.push_back(a); return a; };  // window-wide, so it works whichever dock has focus

    auto* editMenu = window.menuBar()->addMenu("&Edit");
    QObject::connect(key(editMenu->addAction("&Undo"), {QKeySequence("Ctrl+Z")}), &QAction::triggered, [canvas] { canvas->undo(false); });
    QObject::connect(key(editMenu->addAction("&Redo"), {QKeySequence("Ctrl+Shift+Z")}), &QAction::triggered, [canvas] { canvas->undo(true); });

    auto* viewMenu = window.menuBar()->addMenu("&View");
    QObject::connect(key(viewMenu->addAction("&Mirror"), {QKeySequence("M")}), &QAction::triggered, [canvas] { canvas->mirror(); });
    viewMenu->addSeparator();
    const QList<QDockWidget*> docks = {toolDock, navDock, colorDock, brushDock, layerDock, optionsDock};
    for (auto* dock : docks) viewMenu->addAction(dock->toggleViewAction());

    // Tab hides every visible dock and remembers them; the next Tab shows those again (#52).
    QList<QDockWidget*> tabHidden;
    auto tabDocks = [&] {
        if (tabHidden.isEmpty()) { for (auto* d : docks) if (d->isVisible()) { tabHidden << d; d->hide(); } }
        else { for (auto* d : tabHidden) d->show(); tabHidden.clear(); }
    };
    viewMenu->addSeparator();
    QObject::connect(key(viewMenu->addAction("&Hide/show docks"), {QKeySequence("Tab")}), &QAction::triggered, tabDocks);

    // The dock arrangement is saved on quit and restored on launch; with nothing saved the layout above stands (#56).
    for (auto* d : docks) d->setObjectName(d->windowTitle());  // QMainWindow::saveState needs each dock named
    window.restoreState(QSettings().value("windowState").toByteArray());
    window.onClose = [&] {
        if (!tabHidden.isEmpty()) tabDocks();  // quit from a Tab-hidden state must not save every dock as closed
        QSettings().setValue("windowState", window.saveState());
    };

    auto* layerMenu = window.menuBar()->addMenu("&Layer");
    for (const auto& l : layerOps) {
        auto* a = layerMenu->addAction(l.menu);
        docActions.push_back(a);
        QObject::connect(a, &QAction::triggered, [canvas, refresh, op = l.op] { op(canvas->doc()); refresh(); });
    }

    auto* toolsMenu = window.menuBar()->addMenu("&Tools");
    toolsMenu->addActions(toolGroup->actions());
    toolsMenu->addSeparator();
    auto* erase = key(toolsMenu->addAction("&Erase mode"), {QKeySequence("E")});
    erase->setCheckable(true);
    QObject::connect(erase, &QAction::toggled, [canvas, eraseBox, erase](bool on) { canvas->setErase(on); eraseBox->setChecked(canvas->erase()); erase->setChecked(canvas->erase()); });
    auto showOptionsFor = canvas->onToolChange;
    canvas->onToolChange = [=](int tool) { showOptionsFor(tool); erase->setChecked(canvas->erase()); };  // the new tool's own Erase
    QObject::connect(eraseBox, &QCheckBox::toggled, erase, &QAction::setChecked);  // each side's setChecked with the same value emits nothing, so they settle
    QObject::connect(key(toolsMenu->addAction("Finish path &open"), {QKeySequence("Return"), QKeySequence("Enter")}), &QAction::triggered, [canvas] { canvas->finishPath(false); });
    QObject::connect(key(toolsMenu->addAction("Finish path &closed"), {QKeySequence("Shift+Return"), QKeySequence("Shift+Enter")}), &QAction::triggered, [canvas] { canvas->finishPath(true); });

    auto* helpMenu = window.menuBar()->addMenu("&Help");
    QObject::connect(helpMenu->addAction("About Qt"), &QAction::triggered, [&] { app.aboutQt(); });

    for (auto* m : {editMenu, layerMenu, toolsMenu}) docActions.push_back(m->menuAction());
    kindChanged();  // the menus exist now: the no-document state applies to them too
    window.resize(1024, 768);
    window.show();
    return app.exec();
}
