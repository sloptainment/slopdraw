// The Navigation, Color and Brushes docks.
#include "shell.h"
#include <cmath>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSlider>
#include <QVBoxLayout>
#include "colorpicker.h"
#include "navigator.h"

namespace {

// Zoom slider (#289): log scale, value = 100 * log2(percent), so 0..1164 spans 1..3200 %; it follows the view on every paint.
struct ZoomSlider : QSlider {
    using QSlider::QSlider;
    std::function<void()> onDoubleClick;
    void mouseDoubleClickEvent(QMouseEvent*) override { if (onDoubleClick) onDoubleClick(); }
};

}  // namespace

void buildNavigationDock(Shell& sh) {
    Canvas* canvas = sh.canvas;
    auto* nav = sh.nav = new Navigator(canvas);
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
    sh.navDock = new QDockWidget("Navigation", &sh.window);
    sh.navDock->setWidget(navPanel);
    sh.window.addDockWidget(Qt::RightDockWidgetArea, sh.navDock);
}

void buildColorDock(Shell& sh) {
    sh.picker = new ColorPicker;
    sh.colorDock = new QDockWidget("Color", &sh.window);
    sh.colorDock->setWidget(sh.picker);
    sh.window.addDockWidget(Qt::RightDockWidgetArea, sh.colorDock);
}

// The brush list alone (#209); the Tools dock adds it to the window, below itself (layout B).
void buildBrushesDock(Shell& sh) {
    Canvas* canvas = sh.canvas;
    auto* brushList = new QListWidget;
    auto brushes = listBrushes(BRUSH_DIR);
    for (const auto& b : brushes) brushList->addItem(b.name);
    brushList->setCurrentRow(0);  // matches Canvas's starting brush
    QObject::connect(brushList, &QListWidget::currentRowChanged, [canvas, brushes](int row) { if (row >= 0) canvas->setBrush(brushes[row].path); });
    auto* brushPanel = new QWidget;
    auto* col = new QVBoxLayout(brushPanel);
    col->addWidget(brushList);
    sh.brushDock = new QDockWidget("Brushes", &sh.window);
    sh.brushDock->setWidget(brushPanel);
}
