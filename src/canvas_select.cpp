// Canvas input for Select and Box Select: picks, the Transform box, floating pixels and groups.
#include "canvas.h"
#include <algorithm>
#include <QMenu>

std::vector<Document::VPath> Canvas::pickedPaths() const {
    std::vector<Document::VPath> ps;
    for (int i : picked()) ps.push_back(s_.doc.paths(s_.doc.current())[i]);
    return ps;
}

void Canvas::setBezier(bool on) {
    if (objectTool_ && !picked().empty()) {
        if (on) editPicks([](Document::VPath& p) { if (p.kind == Document::PathKind::Spiro) p = Document::toBezier(p); });
        return;
    }
    pen_.bezier = on; pen_.cancel(); update();
}

bool Canvas::editPicks(const std::function<void(Document::VPath&)>& f) {
    if (!objectTool_ || picked().empty()) return false;
    auto ps = s_.doc.paths(s_.doc.current());
    for (int i : picked()) f(ps[i]);
    s_.doc.setPaths(s_.doc.current(), ps);
    update();
    if (onLayersChange) onLayersChange();
    return true;
}

void Canvas::objectPress(QPointF pos, Qt::KeyboardModifiers mods) {
    if (!s_.doc.vector(s_.doc.current())) return rasterPress(pos);  // a group too: groupBox() is the box
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

void Canvas::rasterPress(QPointF pos) {
    if (!rasterBox() && !groupBox()) return;
    BoxFrame f = frame();
    QPointF d = s_.view.toDoc(pos);
    auto handle = frameHandleAt(f, d, 6 / s_.view.screenRadius(1));
    bool rotate = onRotateHandle(pos);
    if (!handle && !rotate && !f.rect.contains(f.toDoc().inverted().map(d))) return endPaste();
    moveFrame_ = f;
    moveDrag_ = true;
    handle_ = handle;
    rotating_ = rotate;
    moveStart_ = d;
    dragM_ = {};
}

bool Canvas::floatPiece(const QTransform& m, bool commit) {
    if (pasted_.isNull()) return commit ? s_.doc.transformRect(box_, m) : s_.doc.floatRect(box_, m);
    QTransform at = QTransform::fromTranslate(box_.left(), box_.top()) * m;
    return commit ? s_.doc.pasteImage(pasted_, at, pasteNew_) : s_.doc.floatImage(pasted_, at, pasteNew_);
}

void Canvas::endPaste() {
    bool pasting = !pasted_.isNull();
    commitFloat();
    if (pasting && onPasteEnd) onPasteEnd();
}

void Canvas::commitFloat() {
    if (groupFloat_) {
        s_.doc.transformGroup(s_.doc.current(), groupM_, scaleWidths_);
        groupFloat_ = false;
        groupM_ = {};
        groupFrame_.reset();
        update();
        if (onLayersChange) onLayersChange();
    }
    if (!floating_) return;
    floatPiece(floatM_, true);
    pasted_ = QImage();
    floating_ = false;
    floatM_ = {};
    frame_.reset();
    box_ = QRect();
    update();
    if (onLayersChange) onLayersChange();
}

void Canvas::cancelFloat() {
    if (groupFloat_) {
        s_.doc.cancelFloat();
        groupFloat_ = false;
        groupM_ = {};
        groupFrame_.reset();
        update();
    }
    if (!floating_) return;
    s_.doc.cancelFloat();
    pasted_ = QImage();
    floating_ = false;
    floatM_ = {};
    frame_.reset();
    update();
}

void Canvas::updateGroupRect() {
    if (!hasDoc() || !objectTool_ || !s_.doc.isGroup(s_.doc.current())) {  // stepId seals a stroke in progress, so only ask with Select on a group
        groupRect_ = QRectF();
        groupKeyLayer_ = -1;
        return;
    }
    int cur = s_.doc.current(), id = s_.doc.stepId();
    if (cur == groupKeyLayer_ && id == groupKeyStep_) return;
    groupKeyLayer_ = cur;
    groupKeyStep_ = id;
    groupFloat_ = false;
    groupM_ = {};
    groupFrame_.reset();
    groupRect_ = s_.doc.groupBounds(cur);
}

void Canvas::boxPress(QPointF pos) {
    boxing_ = true;
    boxStart_ = s_.view.toDoc(pos);
    update();
}

void Canvas::deleteBox() {
    if (box_.isEmpty() || boxLayer_ != s_.doc.current() || !s_.doc.clearRect(box_)) return;
    update();
    if (onLayersChange) onLayersChange();
}

void Canvas::endBox() {
    if (!boxing_) return;
    boxing_ = false;
    if (cursor_) {
        QRectF box = QRectF(boxStart_, s_.view.toDoc(*cursor_)).normalized();
        bool dragged = std::max(box.width(), box.height()) * s_.view.screenRadius(1) > 3;
        if (boxTool_) {
            if (dragged) { box_ = box.toAlignedRect() & s_.doc.image(0).rect(); boxLayer_ = s_.doc.current(); frame_.reset(); }
            else if (!box_.contains(box.topLeft().toPoint())) box_ = QRect();
        } else if (dragged && referenceTool_) {
            auto in = referencesIn(s_.doc.references(), box);
            refPicks_.insert(in.begin(), in.end());
        } else if (dragged) {
            auto in = s_.doc.pathsIn(s_.doc.current(), box);
            picked_.insert(in.begin(), in.end());
        }
    }
    update();
}

BoxFrame Canvas::frame() const {
    if (referenceTool_) return moveDrag_ ? frameAfter(moveFrame_, dragM_) : frame_ ? *frame_ : BoxFrame{referencesBounds(s_.doc.references(), refPicks_), 0};
    if (groupBox()) return moveDrag_ ? frameAfter(moveFrame_, dragM_) : groupFrame_ ? *groupFrame_ : BoxFrame{groupRect_, 0};
    if (rasterBox()) return moveDrag_ ? frameAfter(moveFrame_, dragM_) : frame_ ? *frame_ : BoxFrame{QRectF(box_), 0};
    if (moveDrag_ && (rotating_ || frame_)) return frameAfter(moveFrame_, dragM_);
    return frame_ ? *frame_ : BoxFrame{pickedBounds(s_.doc.paths(s_.doc.current()), picked()), 0};
}

bool Canvas::onRotateHandle(QPointF pos) const {
    BoxFrame f = frame();
    QPointF c = f.toDoc().map(QPointF(f.rect.center().x(), f.rect.top() - 24 / s_.view.screenRadius(1))), d = s_.view.toDoc(pos) - c;
    return std::hypot(d.x(), d.y()) <= 6 / s_.view.screenRadius(1);
}

void Canvas::dragMoveTo(QPointF pos, bool shift) {
    preview_ = [=, this] {
        dragM_ = rotating_ ? rotateMatrix(moveFrame_.rect.center(), moveStart_, s_.view.toDoc(pos)) : handle_ ? frameScaleMatrix(moveFrame_, *handle_, s_.view.toDoc(pos), shift) : QTransform::fromTranslate(s_.view.toDoc(pos).x() - moveStart_.x(), s_.view.toDoc(pos).y() - moveStart_.y());
        if (referenceTool_ && handle_) dragM_ = frameScaleMatrix(moveFrame_, *handle_, s_.view.toDoc(pos), true);  // aspect always kept
        if (referenceTool_) return s_.doc.previewReferences(transformReferences(refsBefore_, refPicks_, dragM_));
        if (groupBox()) {
            groupFloat_ = s_.doc.floatGroup(s_.doc.current(), groupM_ * dragM_, scaleWidths_);
            return;
        }
        if (rasterBox()) {  // lift on the first drag; every drag resamples from the untouched pixels
            floating_ = floatPiece(floatM_ * dragM_);
            return;
        }
        auto moved = transformPaths(pathsBefore_, picked_, dragM_);  // live preview moves nodes only; widths follow on release
        for (int i : picked_) s_.doc.previewNodes(s_.doc.current(), i, moved[i].nodes, moved[i].closed);
    };
    update();
}

void Canvas::endMoveDrag() {
    if (!moveDrag_) return;
    flushPreview();
    moveDrag_ = false;
    if (referenceTool_) {  // one References step from the list the drag began with
        if (dragM_.isIdentity()) return;
        s_.doc.previewReferences(refsBefore_);
        s_.doc.setReferences(transformReferences(refsBefore_, refPicks_, dragM_));
        if (rotating_ || frame_) frame_ = frameAfter(moveFrame_, dragM_);
        return update();
    }
    if (groupBox()) {
        if (groupFloat_) { groupM_ = groupM_ * dragM_; groupFrame_ = frameAfter(moveFrame_, dragM_); }
        return;
    }
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

void Canvas::flipMenu(QPoint globalPos) {
    if (picked().empty() && !rasterBox() && !groupBox()) return;
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
    if (groupBox()) {  // joins the floating group
        groupM_ = groupM_ * m;
        groupFloat_ = s_.doc.floatGroup(s_.doc.current(), groupM_, scaleWidths_);
        groupFrame_ = frameAfter(f, m);
        return update();
    }
    if (rasterBox()) {  // joins the floating piece; the commit is the one undo step
        floatM_ = floatM_ * m;
        floating_ = floatPiece(floatM_);
        frame_ = frameAfter(f, m);
        return update();
    }
    if (!s_.doc.setPaths(s_.doc.current(), transformPaths(s_.doc.paths(s_.doc.current()), picked(), m, scaleWidths_))) return;
    if (frame_) frame_ = frameAfter(f, m);
    update();
}

void Canvas::deletePicked() {
    auto& all = s_.doc.paths(s_.doc.current());
    std::vector<Document::VPath> kept;
    for (int i = 0; i < int(all.size()); i++) if (!picked().count(i)) kept.push_back(all[i]);
    if (picked().empty() || !s_.doc.setPaths(s_.doc.current(), kept)) return;
    picked_.clear();
    update();
    if (onLayersChange) onLayersChange();
}

const std::set<int>& Canvas::picked() const {
    static const std::set<int> none;
    bool ok = pickedLayer_ == s_.doc.current() && (picked_.empty() || *picked_.rbegin() < int(s_.doc.paths(pickedLayer_).size()));
    return ok ? picked_ : none;
}
