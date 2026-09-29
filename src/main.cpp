#include "document.h"
#include "view.h"

#include <QApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>

// Krita Default navigation (/r/5): middle drag or Space+left drag pans; +Ctrl zooms, +Shift rotates; wheel zooms; M mirrors.
class Canvas : public QWidget {
public:
    Canvas() { setFocusPolicy(Qt::StrongFocus); }
    void setImage(const QImage& image) { image_ = image; view_ = View(); update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(view_.matrix());
        p.drawImage(0, 0, image_);
    }
    void mousePressEvent(QMouseEvent* e) override {
        dragging_ = e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_);
        last_ = e->position();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!dragging_) return;
        QPointF d = e->position() - last_;
        last_ = e->position();
        if (e->modifiers() & Qt::ControlModifier) view_.zoomAt(rect().center(), std::exp(-d.y() * 0.01));
        else if (e->modifiers() & Qt::ShiftModifier) view_.rotateAt(rect().center(), d.x() * 0.5);
        else view_.pan(d);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }
    void wheelEvent(QWheelEvent* e) override {
        view_.zoomAt(e->position(), std::pow(1.1, e->angleDelta().y() / 120.0));
        update();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->isAutoRepeat()) return;
        if (e->key() == Qt::Key_Space) space_ = true;
        else if (e->key() == Qt::Key_M) { view_.mirrorAt(rect().center()); update(); }
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (!e->isAutoRepeat() && e->key() == Qt::Key_Space) space_ = false;
    }

private:
    QImage image_;
    View view_;
    QPointF last_;
    bool dragging_ = false, space_ = false;
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
        canvas->setImage(Document(QSize(w, h)).render());
    });

    window.resize(1024, 768);
    window.show();
    return app.exec();
}
