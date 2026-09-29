#include "document.h"
#include "view.h"

#include <QApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QTabletEvent>
#include <QWheelEvent>
#include <cmath>

// Krita Default navigation (/r/5): middle drag or Space+left drag pans; +Ctrl zooms, +Shift rotates; wheel zooms; M mirrors; Ctrl+Z undoes, Ctrl+Shift+Z redoes.
// Painting: the pen tip, or plain left button at pressure 0.5, feeds pen samples to the Document.
class Canvas : public QWidget {
public:
    Canvas() { setFocusPolicy(Qt::StrongFocus); }
    void setDocument(Document doc) { doc_ = std::move(doc); view_ = View(); update(); }

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
        if (e->button() == Qt::LeftButton && !space_) paintAt(e->position(), 0.5f, 0, 0, e->timestamp());
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (painting_) return paintAt(e->position(), 0.5f, 0, 0, e->timestamp());
        if (!dragging_) return;
        QPointF d = e->position() - last_;
        last_ = e->position();
        if (e->modifiers() & Qt::ControlModifier) view_.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
        else if (e->modifiers() & Qt::ShiftModifier) view_.rotateAt(rect().center(), d.x() * 0.5);
        else view_.pan(d);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = painting_ = false; }
    void tabletEvent(QTabletEvent* e) override {
        if (space_) return e->ignore();  // Qt turns it into mouse events, so Space+pen drag pans
        e->accept();
        if (e->type() == QEvent::TabletRelease) painting_ = false;
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
    bool dragging_ = false, space_ = false, painting_ = false;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    auto* canvas = new Canvas;
    window.setCentralWidget(canvas);

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
