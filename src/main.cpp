#include "document.h"
#include "spiropen.h"
#include "tools.h"
#include "view.h"
#include "ora.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QToolButton>
#include <QActionGroup>
#include <QSlider>
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
#include <set>
#include <optional>
#include <QTabletEvent>
#include <QWheelEvent>
#include <cmath>

// Krita Default navigation (/r/5): middle drag or Space+left drag pans; +Ctrl zooms, +Shift rotates; wheel zooms; M mirrors; Ctrl+Z undoes, Ctrl+Shift+Z redoes.
// Shift+left drag (mouse or pen) resizes the brush instead of painting: 100 px right = x e.
// Ctrl+left click (mouse or pen) picks the visible color at that point instead of painting.
// Painting: the pen tip, or plain left button at pressure 0.5, feeds pen samples to the Document.
class Canvas : public QWidget {
public:
    Canvas() { setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); }
    void setDocument(Document doc) { doc_ = std::move(doc); view_ = View(); selected_ = -1; nodeSel_.clear(); picked_.clear(); update(); if (onLayersChange) onLayersChange(); }
    Document& doc() { return doc_; }
    bool hasDoc() const { return !doc_.image(0).isNull(); }  // the app starts on Document(QSize()), which has no pixels (#157)
    std::function<void()> onLayersChange;  // fired when the layer stack or a layer's pixels may have changed outside the Layers dock (undo, redo, new document, pointer release, path finish)
    void setColor(const QColor& c) { color_ = c; if (brush_) setBrushColor(brush_.get(), c); }
    std::function<void()> onRadiusChange;  // fired after a drag resize so the Size slider can follow
    std::function<void(const QColor&)> onColorPick;  // fired after Ctrl+click so the Color dock can follow
    void setRadius(float r) { if (brush_) setBrushRadius(brush_.get(), r); update(); }
    void setOpacity(float o) { if (brush_) setBrushOpacity(brush_.get(), o); }
    void setLassoOpacity(float o) { lassoOpacity_ = o; }  // Lasso Fill's own, apart from the Brush's (#213)
    float radius() const { return std::exp(mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC)); }
    float opacity() const { return mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_OPAQUE); }
    void setNodeType(SpiroType t) { pen_.type = t; }  // the Spiro Pen's, for new nodes (#215)
    // Node/Knot edit's picker: retypes the selected nodes (one undo step); the pen's type is left alone.
    void retypeSelected(SpiroType t) {
        if (selectTool_ && selected_ >= 0 && !nodeSel_.empty()) editNodes(retypeNodes(doc_.nodes(doc_.current(), selected_), nodeSel_, t));
    }
    std::function<void(const QString&)> onHint;  // fired with the status bar line whenever it changes
    std::function<void(int)> onToolChange;  // fired with the new tool (as setTool numbers it), so the Tool Options dock can follow
    // The shell's tool actions call this: 0 Brush, 1 Lasso Fill, 2 Spiro Pen, 3 Node/Knot edit (selectTool_), 4 Select (objectTool_: picks and deletes whole paths).
    void setTool(int t) {
        fillTool_ = t == 1; spiroTool_ = t == 2; selectTool_ = t == 3; objectTool_ = t == 4;
        if (selectTool_) { selected_ = -1; nodeSel_.clear(); }
        picked_.clear();
        boxing_ = false;
        update();
        if (onToolChange) onToolChange(t);
    }
    // The Edit, View and Tools menu actions call these; their keys are window-wide shortcuts.
    void undo(bool redo) {
        redo ? doc_.redo() : doc_.undo();
        selected_ = -1;
        nodeSel_.clear();
        picked_.clear();
        update();
        if (onLayersChange) onLayersChange();
    }
    void mirror() { view_.mirrorAt(rect().center()); update(); }
    // Erase is the Brush's or Lasso Fill's own (#215); the other tools have none.
    bool erase() const { return fillTool_ ? eraseFill_ : !spiroTool_ && !selectTool_ && !objectTool_ && eraseBrush_; }
    void setErase(bool on) { if (!spiroTool_ && !selectTool_ && !objectTool_) (fillTool_ ? eraseFill_ : eraseBrush_) = on; }
    void setPathWidth(double w) { pathWidth_ = w; }  // a new Vector Path's width in px, from the Spiro Pen's Tool Options
    void finishPath(bool closed) {
        if (spiroTool_) pen_.finish(doc_, closed, color_, pathWidth_);  // refused on a Raster Layer
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
        QString h = hint();  // every tool or state change calls update(), so paint is where a change of hint is noticed
        if (h != hint_) { hint_ = h; if (onHint) onHint(h); }
        QPainter p(this);
        if (!hasDoc()) {
            p.fillRect(rect(), QColor(90, 90, 90));
            p.setPen(Qt::white);
            p.drawText(rect(), Qt::AlignCenter, "File > New or Open");
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(view_.matrix());
        p.drawImage(0, 0, doc_.render());
        if (lasso_.size() > 1) {
            QPen pen(Qt::gray, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.drawPolyline(lasso_.data(), lasso_.size());  // the layer is untouched until pen-up
        }
        if (spiroTool_ && !pen_.nodes.empty()) {  // the path so far, plus a node under the pointer that a click would place
            auto nodes = pen_.nodes;
            if (cursor_) nodes.push_back({view_.toDoc(*cursor_).x(), view_.toDoc(*cursor_).y(), pen_.type});
            if (auto path = spiroToPath(nodes, false)) {
                QPen pen(Qt::gray, 1);
                pen.setCosmetic(true);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawPath(*path);
            }
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            double r = 4 / view_.screenRadius(1);
            bool closing = cursor_ && pen_.nodes.size() > 2 && pen_.onFirst(view_.toDoc(*cursor_), 6 / view_.screenRadius(1));
            for (size_t i = 0; i < pen_.nodes.size(); i++) {  // a square per placed node; the first is filled when a click would close (#180)
                p.setBrush(i == 0 && closing ? Qt::blue : Qt::white);
                p.drawRect(QRectF(pen_.nodes[i].x - r, pen_.nodes[i].y - r, 2 * r, 2 * r));
            }
        }
        if (selectTool_ && selected_ >= 0) {  // a square per Spiro Node of the selected path, filled when selected, then a circle per Width Knot
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            double r = 4 / view_.screenRadius(1);
            auto nodes = doc_.nodes(doc_.current(), selected_);
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
            double r = 4 / view_.screenRadius(1);
            for (QPointF h : doc_.knotHandles(doc_.current(), selected_)) p.drawEllipse(h, r, r);
        }
        if (objectTool_) {  // Select: the picked paths' outlines, and the box being dragged
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            for (int i : picked()) p.drawPath(Document::outline(doc_.paths(doc_.current())[i]));
            if (boxing_ && cursor_) {
                pen.setStyle(Qt::DashLine);
                p.setPen(pen);
                p.drawRect(QRectF(boxStart_, view_.toDoc(*cursor_)).normalized());
            }
        }
        if (cursor_ && brush_ && !fillTool_ && !spiroTool_ && !selectTool_ && !objectTool_) {  // brush outline: dark with a light line beside it, readable on any color
            p.resetTransform();
            p.setRenderHint(QPainter::Antialiasing);
            double r = view_.screenRadius(radius());
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
        resizing_ = e->button() == Qt::LeftButton && !space_ && !selectTool_ && !objectTool_ && (e->modifiers() & Qt::ShiftModifier);
        if (e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ControlModifier) && !editKnot(e->position(), e->modifiers())) pickAt(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ControlModifier)) return;
        else if (e->button() == Qt::RightButton && spiroTool_) finishPath(false);
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && selectTool_) selectPress(e->position(), e->modifiers());
        else if (e->button() == Qt::LeftButton && !space_ && objectTool_) objectPress(e->position(), e->modifiers());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && spiroTool_) spiroClick(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && fillTool_) lassoAt(e->position(), erase());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && !objectTool_) paintAt(e->position(), 0.5f, 0, 0, e->timestamp(), erase());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!resizing_) cursor_ = e->position();  // the circle stays at the press point while Shift+drag resizes
        update();
        if (resizing_) return resizeTo(e->position());
        if (knotDrag_ >= 0) return dragKnotTo(e->position());
        if (nodeDrag_) return dragNodesTo(e->position());
        if (boxing_) return;
        if (lassoing_) return lassoAt(e->position(), erase());
        if (painting_) return paintAt(e->position(), 0.5f, 0, 0, e->timestamp(), erase());
        if (!dragging_) return;
        QPointF d = e->position() - last_;
        last_ = e->position();
        if (e->modifiers() & Qt::ControlModifier) view_.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
        else if (e->modifiers() & Qt::ShiftModifier) view_.rotateAt(rect().center(), d.x() * 0.5);
        else view_.pan(d);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { endLasso(); endKnotDrag(); endNodeDrag(); endBox(); dragging_ = painting_ = resizing_ = false; if (onLayersChange) onLayersChange(); }
    void tabletEvent(QTabletEvent* e) override {
        if (space_) return e->ignore();  // Qt turns it into mouse events, so Space+pen drag pans
        e->accept();
        if (e->type() == QEvent::TabletLeaveProximity) { cursor_.reset(); update(); }
        else if (!resizing_ || e->type() == QEvent::TabletPress) { cursor_ = e->position(); update(); }
        bool eraser = e->pointerType() == QPointingDevice::PointerType::Eraser;
        if (e->type() == QEvent::TabletRelease) { endLasso(); endKnotDrag(); endNodeDrag(); endBox(); painting_ = resizing_ = false; if (onLayersChange) onLayersChange(); }
        else if (e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ControlModifier)) { if (!editKnot(e->position(), e->modifiers())) pickAt(e->position()); }
        else if (e->type() == QEvent::TabletPress && !selectTool_ && !objectTool_ && (e->modifiers() & Qt::ShiftModifier)) { resizing_ = true; last_ = e->position(); }
        else if (resizing_) resizeTo(e->position());
        else if (selectTool_) { if (e->type() == QEvent::TabletPress) selectPress(e->position(), e->modifiers()); else if (knotDrag_ >= 0) dragKnotTo(e->position()); else if (nodeDrag_) dragNodesTo(e->position()); }
        else if (objectTool_) { if (e->type() == QEvent::TabletPress) objectPress(e->position(), e->modifiers()); }
        else if (spiroTool_) { if (e->type() == QEvent::TabletPress) { if (e->button() == Qt::RightButton) finishPath(false); else spiroClick(e->position()); } }  // the barrel button arrives as right (#178)
        else if (fillTool_ && (e->type() == QEvent::TabletPress || lassoing_)) lassoAt(e->position(), erase() || eraser);
        else if (!objectTool_ && (e->pointerType() == QPointingDevice::PointerType::Pen || e->pointerType() == QPointingDevice::PointerType::Eraser) && (e->type() == QEvent::TabletPress || painting_))
            paintAt(e->position(), e->pressure(), e->xTilt() / 60, e->yTilt() / 60, e->timestamp(),  // libmypaint tilt is -1..1 for +-60 degrees
                    eraser);  // the flag is per call (b20)
    }
    void leaveEvent(QEvent*) override { cursor_.reset(); update(); }
    void wheelEvent(QWheelEvent* e) override {
        view_.zoomAt(e->position(), std::pow(1.1, e->angleDelta().y() / 120.0));
        update();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->isAutoRepeat()) return;
        if (e->key() == Qt::Key_Space) space_ = true;
        else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            if (selectTool_) deleteSelectedNodes();
            else if (objectTool_) deletePicked();
            else pen_.backspace();
            update();
        }
        else if (e->key() == Qt::Key_Escape) { pen_.cancel(); update(); }
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (!e->isAutoRepeat() && e->key() == Qt::Key_Space) space_ = false;
    }

private:
    // What the next click does for the current tool and state (#177).
    QString hint() const {
        if (!hasDoc()) return {};
        if (spiroTool_ && pen_.nodes.empty()) return "Click places the first node";
        if (spiroTool_) return "Click adds a node · Enter or right-click finishes open · click the first node or Shift+Enter closes · Backspace drops the last node · Esc cancels";
        if (selectTool_ && selected_ < 0) return "Click a path to select it";
        if (selectTool_) return "Click or Shift+click nodes · drag nodes · Delete removes them · drag a knot · Ctrl+click a knot duplicates · Ctrl+click the path adds a knot · Ctrl+Alt+click deletes";
        if (objectTool_ && !doc_.vector(doc_.current())) return "Select does nothing on a Raster Layer";
        if (objectTool_ && picked().empty()) return "Click a path to pick it · Shift+click toggles · drag a box on empty canvas picks the paths it touches";
        if (objectTool_) return QString("%1 picked · Delete removes them · click or drag a box to pick again · Shift adds").arg(picked().size());
        return "Draw · Shift+drag sizes the brush · Ctrl+click picks a color · E toggles erase";
    }
    // Spiro Pen: a click near the first node closes the path (6 screen px), any other click places a node.
    void spiroClick(QPointF pos) {
        QPointF d = view_.toDoc(pos);
        if (pen_.nodes.size() > 2 && pen_.onFirst(d, 6 / view_.screenRadius(1))) finishPath(true);
        else pen_.click(d);
        update();
    }
    // Select tool: a click within 6 screen px of a path of the current Vector Layer selects it; any other click deselects.
    void selectAt(QPointF pos) {
        selected_ = doc_.pathAt(doc_.current(), view_.toDoc(pos), 6 / view_.screenRadius(1));
        nodeSel_.clear();
        update();
    }
    // Select tool press: on a Width Knot handle of the selected path (within 6 screen px) it starts a drag; on a Spiro Node it selects
    // only that node (Shift toggles it); anywhere else it selects a path and clears the node selection (Shift+click there changes nothing).
    void selectPress(QPointF pos, Qt::KeyboardModifiers mods) {
        int i = knotAt(pos);
        if (i >= 0) {
            knotDrag_ = i;
            knotsBefore_ = doc_.knots(doc_.current(), selected_);
            return;
        }
        int n = selected_ < 0 ? -1 : doc_.nodeAt(doc_.current(), selected_, view_.toDoc(pos), 6 / view_.screenRadius(1));
        if (n < 0 && (mods & Qt::ShiftModifier)) return;
        if (n < 0) return selectAt(pos);
        if (mods & Qt::ShiftModifier) {
            if (!nodeSel_.erase(n)) nodeSel_.insert(n);
        } else {  // a press on a selected node keeps the selection, so all selected nodes drag together
            if (!nodeSel_.count(n)) nodeSel_ = {n};
            nodeDrag_ = true;
            nodeStart_ = view_.toDoc(pos);
            pathBefore_ = doc_.paths(doc_.current())[selected_];
        }
        update();
    }
    // Select press on a Vector Layer: on a path it picks that path alone (Shift toggles it); on empty canvas it clears the pick (Shift keeps it)
    // and starts a box that endBox turns into the paths it touches.
    void objectPress(QPointF pos, Qt::KeyboardModifiers mods) {
        if (!doc_.vector(doc_.current())) return;
        bool shift = mods & Qt::ShiftModifier;
        int i = doc_.pathAt(doc_.current(), view_.toDoc(pos), 6 / view_.screenRadius(1));
        auto was = picked();
        pickedLayer_ = doc_.current();
        picked_ = shift ? was : std::set<int>{};
        if (i >= 0) {
            if (!shift) picked_ = {i};
            else if (!picked_.erase(i)) picked_.insert(i);
        } else {
            boxing_ = true;
            boxStart_ = view_.toDoc(pos);
        }
        update();
    }
    // Release: a box dragged more than 3 screen px picks the paths it touches (added to the pick, which a press without Shift cleared).
    void endBox() {
        if (!boxing_) return;
        boxing_ = false;
        if (cursor_) {
            QRectF box = QRectF(boxStart_, view_.toDoc(*cursor_)).normalized();
            if (std::max(box.width(), box.height()) * view_.screenRadius(1) > 3) {
                auto in = doc_.pathsIn(doc_.current(), box);
                picked_.insert(in.begin(), in.end());
            }
        }
        update();
    }
    // Delete: removes the picked paths as one undo step.
    void deletePicked() {
        auto& all = doc_.paths(doc_.current());
        std::vector<Document::VPath> kept;
        for (int i = 0; i < int(all.size()); i++) if (!picked().count(i)) kept.push_back(all[i]);
        if (picked().empty() || !doc_.setPaths(doc_.current(), kept)) return;
        picked_.clear();
        update();
        if (onLayersChange) onLayersChange();
    }
    // The paths Select has picked, on the layer they were picked on; none once another layer is current or the list got shorter.
    const std::set<int>& picked() const {
        static const std::set<int> none;
        bool ok = pickedLayer_ == doc_.current() && (picked_.empty() || *picked_.rbegin() < int(doc_.paths(pickedLayer_).size()));
        return ok ? picked_ : none;
    }
    // Replaces the selected path's nodes with one undo step; a set with no curve is refused and leaves the path as it was.
    void editNodes(const std::vector<SpiroNode>& nodes) {
        doc_.setNodes(doc_.current(), selected_, nodes, doc_.paths(doc_.current())[selected_].closed);
        update();
    }
    // Delete: drops the selected nodes unless fewer than 2 would remain.
    void deleteSelectedNodes() {
        if (selected_ < 0 || nodeSel_.empty()) return;
        if (auto kept = removeNodes(doc_.nodes(doc_.current(), selected_), nodeSel_)) {
            editNodes(*kept);
            nodeSel_.clear();
        }
    }
    // The selected nodes follow the pointer live (no undo step yet).
    void dragNodesTo(QPointF pos) {
        doc_.previewNodes(doc_.current(), selected_, moveNodes(pathBefore_.nodes, nodeSel_, view_.toDoc(pos) - nodeStart_), pathBefore_.closed);
        update();
    }
    // Release: one undo step for the whole drag, from the nodes it started with.
    void endNodeDrag() {
        if (!nodeDrag_) return;
        auto after = doc_.nodes(doc_.current(), selected_);
        doc_.previewNodes(doc_.current(), selected_, pathBefore_.nodes, pathBefore_.closed);
        auto& was = pathBefore_.nodes;
        if (!std::equal(after.begin(), after.end(), was.begin(), [](auto& a, auto& b) { return a.x == b.x && a.y == b.y; }))
            doc_.setNodes(doc_.current(), selected_, after, pathBefore_.closed);
        nodeDrag_ = false;
        update();
    }
    // Index of the selected path's Width Knot handle within 6 screen px of pos, -1 for none.
    int knotAt(QPointF pos) {
        if (!selectTool_ || selected_ < 0) return -1;
        auto handles = doc_.knotHandles(doc_.current(), selected_);
        for (int i = 0; i < int(handles.size()); i++)
            if (QLineF(handles[i], view_.toDoc(pos)).length() * view_.screenRadius(1) <= 6) return i;
        return -1;
    }
    // Ctrl+click on a Width Knot handle duplicates the knot, Ctrl+Alt+click deletes it, Ctrl+click on the selected path off a handle adds one (one undo step each); false otherwise.
    bool editKnot(QPointF pos, Qt::KeyboardModifiers mods) {
        int i = knotAt(pos);
        if (i < 0) {
            if (!selectTool_ || selected_ < 0 || (mods & Qt::AltModifier) || doc_.pathAt(doc_.current(), view_.toDoc(pos), 6 / view_.screenRadius(1)) != selected_) return false;
            doc_.addKnot(doc_.current(), selected_, view_.toDoc(pos));
            update();
            return true;
        }
        auto k = doc_.knots(doc_.current(), selected_);
        doc_.setKnots(doc_.current(), selected_, (mods & Qt::AltModifier) ? deleteKnot(k, i) : duplicateKnot(k, i));
        update();
        return true;
    }
    // The dragged knot follows the pointer live (no undo step yet).
    void dragKnotTo(QPointF pos) {
        auto k = doc_.projectKnot(doc_.current(), selected_, view_.toDoc(pos));
        if (!k) return;
        auto knots = knotsBefore_;
        knots[knotDrag_] = *k;
        doc_.previewKnots(doc_.current(), selected_, knots);
        update();
    }
    // Release: one undo step for the whole drag, from the knots it started with.
    void endKnotDrag() {
        if (knotDrag_ < 0) return;
        auto after = doc_.knots(doc_.current(), selected_);
        doc_.previewKnots(doc_.current(), selected_, knotsBefore_);
        if (after.size() == knotsBefore_.size() && !std::equal(after.begin(), after.end(), knotsBefore_.begin(), [](auto& a, auto& b) { return a.t == b.t && a.width == b.width; }))
            doc_.setKnots(doc_.current(), selected_, after);
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
        QPointF d = view_.toDoc(pos);
        QColor c = doc_.colorAt({int(std::floor(d.x())), int(std::floor(d.y()))});  // the pixel under the point, not the nearest corner
        if (!c.isValid() || !brush_) return;
        setColor(c);
        if (onColorPick) onColorPick(c);
    }
    // Lasso Fill: collect the outline in document coordinates; the fill lands in endLasso. The eraser end is per stroke: its press decides.
    void lassoAt(QPointF pos, bool eraser) {
        if (!lassoing_) lassoErase_ = eraser;
        lassoing_ = true;
        lasso_.push_back(view_.toDoc(pos));
        update();
    }
    void endLasso() {
        if (!lassoing_) return;
        lassoing_ = false;
        QColor c = color_;
        c.setAlphaF(lassoOpacity_);
        if (lasso_.size() > 2) doc_.fill(c, {lasso_.begin(), lasso_.end()}, lassoErase_);
        lasso_.clear();
        update();
    }
    void paintAt(QPointF pos, float pressure, float xtilt, float ytilt, ulong ms, bool eraser = false) {
        if (!brush_) return;
        QPointF d = view_.toDoc(pos);
        doc_.stroke(brush_, {{float(d.x()), float(d.y()), pressure, xtilt, ytilt, painting_ ? (ms - lastMs_) / 1000.0 : 0.0}}, painting_, eraser);
        painting_ = true;
        lastMs_ = ms;
        update();
    }
    Document doc_{QSize()};
    std::shared_ptr<MyPaintBrush> brush_ = loadBrush(BRUSH_DIR "/pencil.myb");
    ulong lastMs_ = 0;
    View view_;
    QPointF last_;
    QString hint_;
    std::optional<QPointF> cursor_;  // pointer position over the canvas, for the brush outline
    bool dragging_ = false, space_ = false, painting_ = false, resizing_ = false;
    QColor color_ = Qt::black;  // matches pencil.myb and the Color dock's start
    QPolygonF lasso_;
    SpiroPen pen_;
    int knotDrag_ = -1;  // index of the Width Knot being dragged on the selected path, -1 for none
    std::vector<WidthKnot> knotsBefore_;  // the selected path's knots when the drag began
    bool nodeDrag_ = false;  // a Spiro Node drag is under way
    QPointF nodeStart_;  // where it began, in document coordinates
    Document::VPath pathBefore_;  // the selected path when it began
    std::set<int> nodeSel_;  // selected Spiro Node indexes of the selected path
    std::set<int> picked_;  // Select's picked path indexes on layer pickedLayer_
    int pickedLayer_ = -1;
    bool boxing_ = false;  // a Select box drag is under way
    QPointF boxStart_;  // where it began, in document coordinates
    int selected_ = -1;  // index of the selected path on the current layer, -1 for none
    bool spiroTool_ = false, selectTool_ = false, objectTool_ = false;
    double pathWidth_ = 10;  // px
    float lassoOpacity_ = 1;
    bool fillTool_ = false, eraseBrush_ = false, eraseFill_ = false, lassoing_ = false, lassoErase_ = false;
};

// Hue ring around a saturation/value square. Dragging either calls onChange with the new color; setColor moves it from outside.
class ColorPicker : public QWidget {
public:
    std::function<void(const QColor&)> onChange;
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
    void mouseReleaseEvent(QMouseEvent*) override { mode_ = None; }

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

// The window saves its dock state in closeEvent: by aboutToQuit the floating docks are already hidden and would be saved closed.
struct Window : QMainWindow {
    std::function<void()> onClose;
    void closeEvent(QCloseEvent* e) override { if (onClose) onClose(); QMainWindow::closeEvent(e); }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("mydraw");  // QSettings keys the saved window state on these
    app.setApplicationName("mydraw");

    Window window;
    auto* canvas = new Canvas;
    window.setCentralWidget(canvas);
    canvas->onHint = [&window](const QString& h) { window.statusBar()->showMessage(h); };

    auto* picker = new ColorPicker;
    picker->onChange = [canvas](const QColor& c) { canvas->setColor(c); };
    canvas->onColorPick = [picker](const QColor& c) { picker->setColor(c); };
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
    std::vector<QAction*> docActions;  // enabled only while a document is open (#173); filled as the menus are built
    auto refresh = [canvas, layerList, layerOpacity, layerOpacityValue, &kindChanged] {
        Document& d = canvas->doc();
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
    auto* layerButtons = new QHBoxLayout;
    struct LayerOp { const char* button; const char* menu; std::function<void(Document&)> op; };
    const std::initializer_list<LayerOp> layerOps{
        {"+ Raster", "Add &Raster", [](Document& d) { d.addLayer(); }},
        {"+ Vector", "Add &Vector", [](Document& d) { d.addVectorLayer(); }},
        {"↑", "&Up", [](Document& d) { if (d.moveLayer(d.current(), d.current() + 1)) d.setCurrent(d.current() + 1); }},
        {"↓", "Dow&n", [](Document& d) { if (d.moveLayer(d.current(), d.current() - 1)) d.setCurrent(d.current() - 1); }},
        {"🗑", "&Delete", [](Document& d) { d.deleteLayer(d.current()); }}};
    for (const auto& l : layerOps) {
        auto* btn = new QPushButton(l.button);
        QObject::connect(btn, &QPushButton::clicked, [canvas, refresh, op = l.op] { op(canvas->doc()); refresh(); });
        layerButtons->addWidget(btn);
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
    auto* lassoBox = new QWidget;  // Lasso Fill's own Opacity (#212), starting solid
    auto* lassoRow = new QHBoxLayout(lassoBox);
    lassoRow->setContentsMargins(0, 0, 0, 0);
    auto* lassoOpacity = new QSlider(Qt::Horizontal);  // percent
    lassoOpacity->setRange(1, 100);
    auto* lassoOpacityValue = new QLabel;
    QObject::connect(lassoOpacity, &QSlider::valueChanged, [canvas, lassoOpacityValue](int v) { canvas->setLassoOpacity(v / 100.f); lassoOpacityValue->setText(QString("%1 %").arg(v)); });
    lassoOpacity->setValue(100);
    lassoRow->addWidget(new QLabel("Opacity"));
    lassoRow->addWidget(lassoOpacity);
    lassoRow->addWidget(lassoOpacityValue);
    auto* widthBox = new QWidget;
    auto* widthRow = new QHBoxLayout(widthBox);
    widthRow->setContentsMargins(0, 0, 0, 0);
    auto* pathWidth = new QSlider(Qt::Horizontal);  // px, of a new Vector Path
    pathWidth->setRange(kMinRadius, kMaxRadius);
    auto* pathWidthValue = new QLabel;
    QObject::connect(pathWidth, &QSlider::valueChanged, [canvas, pathWidthValue](int v) { canvas->setPathWidth(v); pathWidthValue->setText(QString("%1 px").arg(v)); });
    pathWidth->setValue(10);
    widthRow->addWidget(new QLabel("Width"));
    widthRow->addWidget(pathWidth);
    widthRow->addWidget(pathWidthValue);
    auto* penTypes = new QListWidget;  // the Spiro Pen's type for new nodes
    auto* editTypes = new QListWidget;  // retypes Node/Knot edit's selected nodes, apart from the pen's (#215)
    for (auto* l : {penTypes, editTypes}) for (const auto& t : kNodeTypes) l->addItem(t.name);
    penTypes->setCurrentRow(2);  // G2, SpiroPen's starting type
    QObject::connect(penTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->setNodeType(kNodeTypes[row].type); });
    QObject::connect(editTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->retypeSelected(kNodeTypes[row].type); });
    auto showOptions = [=](int tool) {
        eraseBox->setVisible(tool <= LassoFillTool);
        brushBox->setVisible(tool == BrushTool);
        lassoBox->setVisible(tool == LassoFillTool);
        widthBox->setVisible(tool == SpiroPenTool);
        penTypes->setVisible(tool == SpiroPenTool);
        editTypes->setVisible(tool == NodeKnotEditTool);
    };
    canvas->onToolChange = showOptions;
    showOptions(BrushTool);
    auto* optionsPanel = new QWidget;
    auto* optionsCol = new QVBoxLayout(optionsPanel);
    optionsCol->addWidget(eraseBox);
    optionsCol->addWidget(brushBox);
    optionsCol->addWidget(lassoBox);
    optionsCol->addWidget(widthBox);
    optionsCol->addWidget(penTypes);
    optionsCol->addWidget(editTypes);
    auto* optionsDock = new QDockWidget("Tool Options", &window);
    optionsDock->setWidget(optionsPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, optionsDock);

    // Tools dock (#154): one checkable action per tool, exclusive, its key a window-wide shortcut so it works whichever dock has focus.
    auto* toolPanel = new QWidget;
    auto* toolCol = new QVBoxLayout(toolPanel);
    auto* toolGroup = new QActionGroup(&window);
    const struct { const char* text; const char* key; } kTools[] = {{"\U0001F58C Brush (B)", "B"}, {"\u2B2E Lasso Fill (F)", "F"}, {"\u2712 Spiro Pen (P)", "P"}, {"\u2316 Node/Knot edit (N)", "N"}, {"\u25AD Select (S)", "S"}};
    int lastTool[2] = {BrushTool, SpiroPenTool};  // last tool used on a Raster, a Vector Layer (#161); Select fits both, so it is no one's last
    for (int i = 0; i < 5; i++) {
        auto* a = toolGroup->addAction(kTools[i].text);
        a->setCheckable(true);
        a->setChecked(i == 0);
        a->setShortcut(QKeySequence(kTools[i].key));
        window.addAction(a);
        QObject::connect(a, &QAction::triggered, [canvas, i, &lastTool] { canvas->setTool(i); if (i != SelectTool) lastTool[i >= SpiroPenTool] = i; });
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
        for (auto* w : {colorDock->widget(), layerPanel, optionsPanel}) w->setEnabled(has);
        for (auto* a : docActions) a->setEnabled(has);
        auto tools = toolGroup->actions();
        for (int i = 0; i < 5; i++) tools[i]->setEnabled(has && toolFitsKind(vec, i));
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
    auto* fileNew = fileMenu->addAction("&New...");
    fileNew->setShortcut(QKeySequence::New);
    QObject::connect(fileNew, &QAction::triggered, [&] {
        bool ok = false;
        int w = QInputDialog::getInt(&window, "New", "Width (px):", 1024, 1, 32768, 1, &ok);
        if (!ok) return;
        int h = QInputDialog::getInt(&window, "New", "Height (px):", 768, 1, 32768, 1, &ok);
        if (!ok) return;
        canvas->setDocument(Document(QSize(w, h)));
    });

    auto* fileOpen = fileMenu->addAction("&Open...");
    fileOpen->setShortcut(QKeySequence::Open);
    QObject::connect(fileOpen, &QAction::triggered, [&] {
        QString path = QFileDialog::getOpenFileName(&window, "Open", {}, "OpenRaster (*.ora)");
        if (path.isEmpty()) return;
        auto doc = readOra(path);
        if (!doc) { QMessageBox::warning(&window, "Open", "Could not read " + path); return; }
        canvas->setDocument(std::move(*doc));
    });
    auto* fileSave = fileMenu->addAction("&Save...");
    docActions.push_back(fileSave);
    fileSave->setShortcut(QKeySequence::Save);
    QObject::connect(fileSave, &QAction::triggered, [&] {
        QString path = QFileDialog::getSaveFileName(&window, "Save", {}, "OpenRaster (*.ora)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".ora")) path += ".ora";
        if (!writeOra(canvas->doc(), path)) QMessageBox::warning(&window, "Save", "Could not write " + path);
    });
    auto* fileExport = fileMenu->addAction("&Export PNG...");
    docActions.push_back(fileExport);
    QObject::connect(fileExport, &QAction::triggered, [&] {
        QString path = QFileDialog::getSaveFileName(&window, "Export PNG", {}, "PNG (*.png)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".png")) path += ".png";
        if (!exportPng(canvas->doc(), path)) QMessageBox::warning(&window, "Export PNG", "Could not write " + path);
    });

    auto* fileQuit = fileMenu->addAction("&Quit");
    fileQuit->setShortcut(QKeySequence::Quit);
    QObject::connect(fileQuit, &QAction::triggered, &window, &QWidget::close);

    auto key = [&window, &docActions](QAction* a, QList<QKeySequence> keys) { a->setShortcuts(keys); window.addAction(a); docActions.push_back(a); return a; };  // window-wide, so it works whichever dock has focus

    auto* editMenu = window.menuBar()->addMenu("&Edit");
    QObject::connect(key(editMenu->addAction("&Undo"), {QKeySequence("Ctrl+Z")}), &QAction::triggered, [canvas] { canvas->undo(false); });
    QObject::connect(key(editMenu->addAction("&Redo"), {QKeySequence("Ctrl+Shift+Z")}), &QAction::triggered, [canvas] { canvas->undo(true); });

    auto* viewMenu = window.menuBar()->addMenu("&View");
    QObject::connect(key(viewMenu->addAction("&Mirror"), {QKeySequence("M")}), &QAction::triggered, [canvas] { canvas->mirror(); });
    viewMenu->addSeparator();
    const QList<QDockWidget*> docks = {toolDock, colorDock, brushDock, layerDock, optionsDock};
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

    for (auto* m : {editMenu, layerMenu, toolsMenu}) docActions.push_back(m->menuAction());
    kindChanged();  // the menus exist now: the no-document state applies to them too
    window.resize(1024, 768);
    window.show();
    return app.exec();
}
