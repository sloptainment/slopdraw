#include "document.h"

#include <QApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QPixmap>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QMainWindow window;
    auto* canvas = new QLabel;
    window.setCentralWidget(canvas);

    auto* fileNew = window.menuBar()->addMenu("&File")->addAction("&New...");
    fileNew->setShortcut(QKeySequence::New);
    QObject::connect(fileNew, &QAction::triggered, [&] {
        bool ok = false;
        int w = QInputDialog::getInt(&window, "New", "Width (px):", 1024, 1, 32768, 1, &ok);
        if (!ok) return;
        int h = QInputDialog::getInt(&window, "New", "Height (px):", 768, 1, 32768, 1, &ok);
        if (!ok) return;
        canvas->setPixmap(QPixmap::fromImage(Document(QSize(w, h)).render()));
    });

    window.resize(1024, 768);
    window.show();
    return app.exec();
}
