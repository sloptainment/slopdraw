#include "document.h"
#include "view.h"

#include <QApplication>
#include <QDockWidget>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
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
#include <QTabletEvent>
#include <QWheelEvent>
#include <cmath>

// Krita Default navigation (/r/5): middle drag or Space+left drag pans; +Ctrl zooms, +Shift rotates; wheel zooms; M mirrors; Ctrl+Z undoes, Ctrl+Shift+Z redoes.
// Shift+left drag (mouse or pen) resizes the brush instead of painting: 100 px right = x e.
// Ctrl+left click (mouse or pen) picks the visible color at that point instead of painting.
// Painting: the pen tip, or plain left button at pressure 0.5, feeds pen samples to the Document.
class Canvas : public QWidget {
public:
    Canvas() { setFocusPolicy(Qt::StrongFocus); }
    void setDocument(Document doc) { doc_ = std::move(doc); view_ = View(); update(); }
    void setColor(const QColor& c) { if (brush_) setBrushColor(brush_.get(), c); }
    std::function<void()> onRadiusChange;  // fired after a drag resize so the Size slider can follow
    std::function<void(const QColor&)> onColorPick;  // fired after Ctrl+click so the Color dock can follow
    void setRadius(float r) { if (brush_) setBrushRadius(brush_.get(), r); }
    void setOpacity(float o) { if (brush_) setBrushOpacity(brush_.get(), o); }
    float radius() const { return std::exp(mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC)); }
    float opacity() const { return mypaint_brush_get_base_value(brush_.get(), MYPAINT_BRUSH_SETTING_OPAQUE); }
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
    }
    void mousePressEvent(QMouseEvent* e) override {
        dragging_ = e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_);
        last_ = e->position();
        resizing_ = e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ShiftModifier);
        if (e->button() == Qt::LeftButton && !space_ && (e->modifiers() & Qt::ControlModifier)) pickAt(e->position());
        else if (e->button() == Qt::LeftButton && !space_ && !resizing_) paintAt(e->position(), 0.5f, 0, 0, e->timestamp());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (resizing_) return resizeTo(e->position());
        if (painting_) return paintAt(e->position(), 0.5f, 0, 0, e->timestamp());
        if (!dragging_) return;
        QPointF d = e->position() - last_;
        last_ = e->position();
        if (e->modifiers() & Qt::ControlModifier) view_.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
        else if (e->modifiers() & Qt::ShiftModifier) view_.rotateAt(rect().center(), d.x() * 0.5);
        else view_.pan(d);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = painting_ = resizing_ = false; }
    void tabletEvent(QTabletEvent* e) override {
        if (space_) return e->ignore();  // Qt turns it into mouse events, so Space+pen drag pans
        e->accept();
        if (e->type() == QEvent::TabletRelease) painting_ = resizing_ = false;
        else if (e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ControlModifier)) pickAt(e->position());
        else if (e->type() == QEvent::TabletPress && (e->modifiers() & Qt::ShiftModifier)) { resizing_ = true; last_ = e->position(); }
        else if (resizing_) resizeTo(e->position());
        else if ((e->pointerType() == QPointingDevice::PointerType::Pen || e->pointerType() == QPointingDevice::PointerType::Eraser) && (e->type() == QEvent::TabletPress || painting_))
            paintAt(e->position(), e->pressure(), e->xTilt() / 60, e->yTilt() / 60, e->timestamp(),  // libmypaint tilt is -1..1 for +-60 degrees
                    e->pointerType() == QPointingDevice::PointerType::Eraser);  // the flag is per call (b20)
    }
    void wheelEvent(QWheelEvent* e) override {
        view_.zoomAt(e->position(), std::pow(1.1, e->angleDelta().y() / 120.0));
        update();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->isAutoRepeat()) return;
        if (e->key() == Qt::Key_Space) space_ = true;
        else if (e->key() == Qt::Key_M) { view_.mirrorAt(rect().center()); update(); }
        else if (e->key() == Qt::Key_Z && (e->modifiers() & Qt::ControlModifier)) {
            (e->modifiers() & Qt::ShiftModifier ? doc_.redo() : doc_.undo());
            update();
        }
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (!e->isAutoRepeat() && e->key() == Qt::Key_Space) space_ = false;
    }

private:
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
    bool dragging_ = false, space_ = false, painting_ = false, resizing_ = false;
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

    auto* fileNew = window.menuBar()->addMenu("&File")->addAction("&New...");
    fileNew->setShortcut(QKeySequence::New);
    QObject::connect(fileNew, &QAction::triggered, [&] {
        bool ok = false;
        int w = QInputDialog::getInt(&window, "New", "Width (px):", 1024, 1, 32768, 1, &ok);
        if (!ok) return;
        int h = QInputDialog::getInt(&window, "New", "Height (px):", 768, 1, 32768, 1, &ok);
        if (!ok) return;
        canvas->setDocument(Document(QSize(w, h)));
    });

    window.resize(1024, 768);
    window.show();
    return app.exec();
}
