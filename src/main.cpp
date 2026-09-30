#include "document.h"
#include "spiropen.h"
#include "view.h"
#include "ora.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QConicalGradient>
#include <QPainterPath>
#include <QMouseEvent>
#include <algorithm>
#include <functional>
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
    void setDocument(Document doc) { doc_ = std::move(doc); view_ = View(); selected_ = -1; update(); if (onLayersChange) onLayersChange(); }
    Document& doc() { return doc_; }
    std::function<void()> onLayersChange;  // fired when the layer stack may have changed outside the Layers dock (undo, redo, new document)
    void setColor(const QColor& c) { color_ = c; if (brush_) setBrushColor(brush_.get(), c); }
    std::function<void()> onRadiusChange;  // fired after a drag resize so the Size slider can follow
    std::function<void(const QColor&)> onColorPick;  // fired after Ctrl+click so the Color dock can follow
    void setRadius(float r) { if (brush_) setBrushRadius(brush_.get(), r); update(); }
    void setOpacity(float o) { if (brush_) setBrushOpacity(brush_.get(), o); }
    float radius() const { return std::exp(mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC)); }
    float opacity() const { return mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_OPAQUE); }
    void setNodeType(SpiroType t) { pen_.type = t; }
    std::function<void(bool)> onToolChange;  // fired with whether the Spiro Pen is now the tool, so the Tool Options dock can follow
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
        QPainter p(this);
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
        }
        if (selectTool_ && selected_ >= 0) {  // a handle per Width Knot of the selected path
            QPen pen(Qt::blue, 1);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.setBrush(Qt::white);
            double r = 4 / view_.screenRadius(1);
            for (QPointF h : doc_.knotHandles(doc_.current(), selected_)) p.drawEllipse(h, r, r);
        }
        if (cursor_ && brush_ && !fillTool_ && !spiroTool_ && !selectTool_) {  // brush outline: dark with a light line beside it, readable on any color
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
        resizing_ = e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ShiftModifier);
        if (e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ControlModifier)) pickAt(e->position());
        else if (e->button() == Qt::RightButton && spiroTool_) finishPath(false);
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && selectTool_) selectAt(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && spiroTool_) spiroClick(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_ && fillTool_) lassoAt(e->position(), eraseMode_);
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_) paintAt(e->position(), 0.5f, 0, 0, e->timestamp());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!resizing_) cursor_ = e->position();  // the circle stays at the press point while Shift+drag resizes
        update();
        if (resizing_) return resizeTo(e->position());
        if (lassoing_) return lassoAt(e->position(), eraseMode_);
        if (painting_) return paintAt(e->position(), 0.5f, 0, 0, e->timestamp());
        if (!dragging_) return;
        QPointF d = e->position() - last_;
        last_ = e->position();
        if (e->modifiers() & Qt::ControlModifier) view_.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
        else if (e->modifiers() & Qt::ShiftModifier) view_.rotateAt(rect().center(), d.x() * 0.5);
        else view_.pan(d);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { endLasso(); dragging_ = painting_ = resizing_ = false; }
    void tabletEvent(QTabletEvent* e) override {
        if (space_) return e->ignore();  // Qt turns it into mouse events, so Space+pen drag pans
        e->accept();
        if (e->type() == QEvent::TabletLeaveProximity) { cursor_.reset(); update(); }
        else if (!resizing_ || e->type() == QEvent::TabletPress) { cursor_ = e->position(); update(); }
        bool eraser = e->pointerType() == QPointingDevice::PointerType::Eraser;
        if (e->type() == QEvent::TabletRelease) { endLasso(); painting_ = resizing_ = false; }
        else if (e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ControlModifier)) pickAt(e->position());
        else if (e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ShiftModifier)) { resizing_ = true; last_ = e->position(); }
        else if (resizing_) resizeTo(e->position());
        else if (selectTool_) { if (e->type() == QEvent::TabletPress) selectAt(e->position()); }
        else if (spiroTool_) { if (e->type() == QEvent::TabletPress) spiroClick(e->position()); }
        else if (fillTool_ && (e->type() == QEvent::TabletPress || lassoing_)) lassoAt(e->position(), eraseMode_ || eraser);
        else if ((e->pointerType() == QPointingDevice::PointerType::Pen || e->pointerType() == QPointingDevice::PointerType::Eraser) && (e->type() == QEvent::TabletPress || painting_))
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
        else if (e->key() == Qt::Key_F) { fillTool_ = true; spiroTool_ = selectTool_ = false; }
        else if (e->key() == Qt::Key_B) fillTool_ = spiroTool_ = selectTool_ = false;
        else if (e->key() == Qt::Key_P) { spiroTool_ = true; fillTool_ = selectTool_ = false; }
        else if (e->key() == Qt::Key_V) { selectTool_ = true; selected_ = -1; fillTool_ = spiroTool_ = false; update(); }
        else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) finishPath(e->modifiers() & Qt::ShiftModifier);
        else if (e->key() == Qt::Key_Backspace) { pen_.backspace(); update(); }
        else if (e->key() == Qt::Key_Escape) { pen_.cancel(); update(); }
        else if (e->key() == Qt::Key_E) eraseMode_ = !eraseMode_;
        else if (e->key() == Qt::Key_M) { view_.mirrorAt(rect().center()); update(); }
        else if (e->key() == Qt::Key_Z && (e->modifiers() & Qt::ControlModifier)) {
            (e->modifiers() & Qt::ShiftModifier ? doc_.redo() : doc_.undo());
            selected_ = -1;
            update();
            if (onLayersChange) onLayersChange();
        }
        if (onToolChange) onToolChange(spiroTool_);
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (!e->isAutoRepeat() && e->key() == Qt::Key_Space) space_ = false;
    }

private:
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
        update();
    }
    void finishPath(bool closed) {
        if (spiroTool_) pen_.finish(doc_, closed, color_, 2 * radius());  // width follows the brush size; refused on a Raster Layer
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
        if (lasso_.size() > 2) doc_.fill(color_, {lasso_.begin(), lasso_.end()}, lassoErase_);
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
    std::optional<QPointF> cursor_;  // pointer position over the canvas, for the brush outline
    bool dragging_ = false, space_ = false, painting_ = false, resizing_ = false;
    QColor color_ = Qt::black;  // matches pencil.myb and the Color dock's start
    QPolygonF lasso_;
    SpiroPen pen_;
    int selected_ = -1;  // index of the selected path on the current layer, -1 for none
    bool spiroTool_ = false, selectTool_ = false;
    bool fillTool_ = false, eraseMode_ = false, lassoing_ = false, lassoErase_ = false;
};

// Hue ring around a saturation/value square. Dragging either calls onChange with the new color; setColor moves it from outside.
class ColorPicker : public QWidget {
public:
    std::function<void(const QColor&)> onChange;
    ColorPicker() { setFixedSize(180, 180); }
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
        p.setPen(QPen(QBrush(g), kRing));
        p.drawEllipse(center(), kOuter - kRing / 2, kOuter - kRing / 2);
        QImage sq(kN, kN, QImage::Format_RGB32);
        for (int y = 0; y < kN; y++)
            for (int x = 0; x < kN; x++) sq.setPixelColor(x, y, QColor::fromHsvF(h_, x / double(kN - 1), 1 - y / double(kN - 1)));
        p.drawImage(square(), sq);
        QPointF hue = center() + (kOuter - kRing / 2) * QPointF(std::cos(h_ * 2 * M_PI), -std::sin(h_ * 2 * M_PI));
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
        mode_ = r >= kOuter - kRing && r <= kOuter ? Ring : square().contains(e->position()) ? Square : None;
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
    static constexpr int kOuter = 88, kRing = 16, kN = 64;  // kN: resolution of the square's image, scaled up to fit
    enum Mode { None, Ring, Square } mode_ = None;
    QPointF center() const { return {width() / 2.0, height() / 2.0}; }
    QRectF square() const {  // the square inscribed in the ring's inner circle
        double half = (kOuter - kRing - 4) / std::sqrt(2.0);
        return {center().x() - half, center().y() - half, 2 * half, 2 * half};
    }
    double h_ = 0, s_ = 0, v_ = 0;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    auto* canvas = new Canvas;
    window.setCentralWidget(canvas);

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
    canvas->onRadiusChange = [canvas, size] { QSignalBlocker b(size); size->setValue(std::lround(canvas->radius())); };  // blocked so the slider's int doesn't round the radius back
    auto* opacity = new QSlider(Qt::Horizontal);  // percent
    opacity->setRange(1, 100);
    opacity->setValue(std::lround(canvas->opacity() * 100));
    QObject::connect(opacity, &QSlider::valueChanged, [canvas](int v) { canvas->setOpacity(v / 100.f); });
    auto* brushPanel = new QWidget;
    auto* col = new QVBoxLayout(brushPanel);
    col->addWidget(brushList);
    col->addWidget(new QLabel("Size"));
    col->addWidget(size);
    col->addWidget(new QLabel("Opacity"));
    col->addWidget(opacity);
    auto* brushDock = new QDockWidget("Brushes", &window);
    brushDock->setWidget(brushPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, brushDock);

    // Layers dock: the list runs top layer first, so row = layerCount - 1 - index. refresh() rebuilds it from the Document.
    auto* layerList = new QListWidget;
    auto* layerOpacity = new QSlider(Qt::Horizontal);  // percent, of the current layer
    layerOpacity->setRange(0, 100);
    auto refresh = [canvas, layerList, layerOpacity] {
        Document& d = canvas->doc();
        QSignalBlocker b1(layerList), b2(layerOpacity);
        layerList->clear();
        for (int i = d.layerCount() - 1; i >= 0; i--) {
            auto* it = new QListWidgetItem(i == 0 ? "Background" : QString("Layer %1").arg(i), layerList);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(d.visible(i) ? Qt::Checked : Qt::Unchecked);
        }
        layerList->setCurrentRow(d.layerCount() - 1 - d.current());
        layerOpacity->setValue(std::lround(d.opacity(d.current()) * 100));
        canvas->update();
    };
    canvas->onLayersChange = refresh;
    QObject::connect(layerList, &QListWidget::currentRowChanged, [canvas, layerList, refresh](int row) {
        if (row < 0) return;
        canvas->doc().setCurrent(canvas->doc().layerCount() - 1 - row);
        refresh();
    });
    QObject::connect(layerList, &QListWidget::itemChanged, [canvas](QListWidgetItem* it) {
        Document& d = canvas->doc();
        d.setVisible(d.layerCount() - 1 - it->listWidget()->row(it), it->checkState() == Qt::Checked);
        canvas->update();
    });
    QObject::connect(layerOpacity, &QSlider::valueChanged, [canvas](int v) {
        canvas->doc().setOpacity(canvas->doc().current(), v / 100.0);
        canvas->update();
    });
    auto* layerButtons = new QHBoxLayout;
    for (auto [text, op] : std::initializer_list<std::pair<const char*, std::function<void(Document&)>>>{
             {"Add", [](Document& d) { d.addLayer(); }},
             {"Add Vector", [](Document& d) { d.addVectorLayer(); }},
             {"Delete", [](Document& d) { d.deleteLayer(d.current()); }},
             {"Up", [](Document& d) { if (d.moveLayer(d.current(), d.current() + 1)) d.setCurrent(d.current() + 1); }},
             {"Down", [](Document& d) { if (d.moveLayer(d.current(), d.current() - 1)) d.setCurrent(d.current() - 1); }}}) {
        auto* btn = new QPushButton(text);
        QObject::connect(btn, &QPushButton::clicked, [canvas, refresh, op = op] { op(canvas->doc()); refresh(); });
        layerButtons->addWidget(btn);
    }
    auto* layerPanel = new QWidget;
    auto* layerCol = new QVBoxLayout(layerPanel);
    layerCol->addWidget(layerList);
    layerCol->addLayout(layerButtons);
    layerCol->addWidget(new QLabel("Opacity"));
    layerCol->addWidget(layerOpacity);
    auto* layerDock = new QDockWidget("Layers", &window);
    layerDock->setWidget(layerPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, layerDock);
    refresh();

    // Tool Options dock (#58): shows the Spiro Node type picker while the Spiro Pen is the tool, empty otherwise.
    auto* nodeTypes = new QListWidget;
    for (const auto& t : kNodeTypes) nodeTypes->addItem(t.name);
    nodeTypes->setCurrentRow(2);  // G2, SpiroPen's starting type
    QObject::connect(nodeTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->setNodeType(kNodeTypes[row].type); });
    nodeTypes->hide();
    canvas->onToolChange = [nodeTypes](bool spiro) { nodeTypes->setVisible(spiro); };
    auto* optionsPanel = new QWidget;
    (new QVBoxLayout(optionsPanel))->addWidget(nodeTypes);
    auto* optionsDock = new QDockWidget("Tool Options", &window);
    optionsDock->setWidget(optionsPanel);
    window.addDockWidget(Qt::RightDockWidgetArea, optionsDock);

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
    fileSave->setShortcut(QKeySequence::Save);
    QObject::connect(fileSave, &QAction::triggered, [&] {
        QString path = QFileDialog::getSaveFileName(&window, "Save", {}, "OpenRaster (*.ora)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".ora")) path += ".ora";
        if (!writeOra(canvas->doc(), path)) QMessageBox::warning(&window, "Save", "Could not write " + path);
    });
    auto* fileExport = fileMenu->addAction("&Export PNG...");
    QObject::connect(fileExport, &QAction::triggered, [&] {
        QString path = QFileDialog::getSaveFileName(&window, "Export PNG", {}, "PNG (*.png)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".png")) path += ".png";
        if (!exportPng(canvas->doc(), path)) QMessageBox::warning(&window, "Export PNG", "Could not write " + path);
    });

    window.resize(1024, 768);
    window.show();
    return app.exec();
}
