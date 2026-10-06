#include "document.h"
#include "nodeedit.h"
#include "spiropen.h"
#include "transform.h"
#include "references.h"
#include "tools.h"
#include "view.h"
#include "ora.h"
#include "stabilizer.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
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
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QWidgetAction>
#include <QPainter>
#include <QConicalGradient>
#include <QPainterPath>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <algorithm>
#include <functional>
#include <set>
#include <optional>
#include <QTabletEvent>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

#include "canvas.h"

#include "colorpicker.h"
#include "navigator.h"

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
    brushList->setCurrentRow(0);  // matches Canvas's starting brush
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

    // Layers dock: the list runs top item first; each row holds its Document index in Qt::UserRole. refresh() rebuilds it from the Document.
    // Drag a row (#459, #460): the pointer is over a gap between rows (a green bar) or the middle of a group row (a dashed outline, "inside"). Qt moves nothing:
    // the drop is handed over as the dragged item's Document index, the gap row and the inside row (-1 for none); refresh() rebuilds the rows.
    struct LayerList : QListWidget {
        std::function<void(int, int, int)> onDrop;
        std::function<void(int, Qt::KeyboardModifiers)> onClick;
        std::set<int> sel;  // the Document indexes selected with Ctrl/Shift (#475); empty means the current item alone. Dropped by refresh() once the Document has taken another step
        int selStep = 0;
        std::vector<int> items(int current) const { return sel.empty() ? std::vector<int>{current} : std::vector<int>(sel.begin(), sel.end()); }
        int gap_ = -1, inside_ = -1, pressed_ = -1, from_ = -1;
        QPoint pressPos_;
        // A press that made the row current would rebuild the rows under the mouse and lose the drag; so the row becomes current on release, and the drag starts here.
        void mousePressEvent(QMouseEvent* e) override {
            auto* it = itemAt(e->pos());
            pressed_ = it && e->button() == Qt::LeftButton ? it->data(Qt::UserRole).toInt() : -1;
            pressPos_ = e->pos();
        }
        void mouseMoveEvent(QMouseEvent* e) override {
            if (pressed_ < 0 || !(e->buttons() & Qt::LeftButton) || (e->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) return;
            from_ = pressed_;
            pressed_ = -1;
            auto* drag = new QDrag(this);
            drag->setMimeData(new QMimeData);
            drag->exec(Qt::MoveAction);
        }
        void mouseReleaseEvent(QMouseEvent* e) override {
            if (pressed_ >= 0 && itemAt(e->pos())) onClick(itemAt(e->pos())->data(Qt::UserRole).toInt(), e->modifiers());
            pressed_ = -1;
        }
        void dragEnterEvent(QDragEnterEvent* e) override { e->accept(); }
        void aim(QPoint pos) {
            auto* it = itemAt(pos);
            gap_ = it ? row(it) : count();
            inside_ = -1;
            if (!it) return;
            QRect r = visualItemRect(it);
            if (it->data(Qt::UserRole + 1).toBool() && pos.y() > r.top() + r.height() / 4 && pos.y() < r.bottom() - r.height() / 4) inside_ = gap_;
            else gap_ += pos.y() > r.center().y();
        }
        void dragMoveEvent(QDragMoveEvent* e) override {
            aim(e->position().toPoint());
            e->accept();
            viewport()->update();
        }
        void dragLeaveEvent(QDragLeaveEvent*) override { gap_ = inside_ = -1; viewport()->update(); }
        void dropEvent(QDropEvent* e) override {
            aim(e->position().toPoint());
            e->setDropAction(Qt::IgnoreAction);
            e->accept();
            int g = gap_, in = inside_;
            gap_ = inside_ = -1;
            onDrop(from_, g, in);
        }
        void paintEvent(QPaintEvent* e) override {
            QListWidget::paintEvent(e);
            if (gap_ < 0 && inside_ < 0) return;
            QPainter p(viewport());
            if (inside_ >= 0) {
                p.setPen(QPen(QColor("#7fd27f"), 2, Qt::DashLine));
                p.drawRect(visualItemRect(item(inside_)).adjusted(1, 1, -1, -1));
            } else if (count()) {
                QRect r = visualItemRect(item(std::min(gap_, count() - 1)));
                p.fillRect(0, (gap_ < count() ? r.top() : r.bottom()) - 1, viewport()->width(), 3, QColor("#7fd27f"));
            }
        }
    };
    auto* layerList = new LayerList;
    layerList->setAcceptDrops(true);
    layerList->viewport()->setAcceptDrops(true);
    layerList->setDropIndicatorShown(false);
    layerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* layerOpacity = new QSlider(Qt::Horizontal);  // percent, of the current layer
    layerOpacity->setRange(0, 100);
    auto* layerOpacityValue = new QLabel;
    QObject::connect(layerOpacity, &QSlider::valueChanged, [layerOpacityValue](int v) { layerOpacityValue->setText(QString("%1 %").arg(v)); });
    std::function<void()> kindChanged;  // set with the Tools dock below; refresh() runs on every current-layer change
    std::function<void()> updateTitle;  // set with the File menu below; refresh() runs it after every edit, undo and redo
    std::vector<QAction*> docActions;  // enabled only while a document is open (#173); filled as the menus are built
    std::vector<QAction*> notOnGroup;  // also disabled while a group is current: Cut and Copy (#501)
    auto refresh = [canvas, nav, layerList, layerOpacity, layerOpacityValue, &kindChanged, &updateTitle] {
        Document& d = canvas->doc();
        nav->refresh();
        QSignalBlocker b1(layerList), b2(layerOpacity);
        layerList->clear();
        int currentRow = -1;
        for (int i = d.layerCount() - 1; i >= 0; i--) {  // a row: colour bars, eye, kind icon, thumbnail, name (#169); a group row ends in its arrow (#457)
            const bool group = d.isGroup(i);
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(2, 2, 2, 2);
            static const char* const hues[] = {"#d9534f", "#5aa0ff", "#7fd27f", "#c58af9"};
            std::vector<int> bars;  // the enclosing groups, outermost first, then the row's own if it is a group
            for (int j = i + 1, depth = d.depth(i); j < d.layerCount() && depth > 0; j++)
                if (d.depth(j) < depth) { bars.insert(bars.begin(), j); depth = d.depth(j); }
            if (group) bars.push_back(i);
            for (int g : bars) {
                auto* bar = new QFrame;
                bar->setFixedWidth(4);
                bar->setStyleSheet(QString("background: %1;").arg(hues[g % 4]));
                h->addWidget(bar);
            }
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
            tpp.drawImage(tp.rect(), group ? d.groupImage(i) : d.image(i));
            thumb->setPixmap(tp);
            h->addWidget(eye);
            h->addWidget(new QLabel(group ? (d.collapsed(i) ? "📁" : "📂") : d.vector(i) ? "✒" : "🖌"));
            h->addWidget(thumb);
            auto* name = new QLabel(d.name(i));
            if (group) name->setStyleSheet("font-weight: bold;");
            h->addWidget(name, 1);
            if (group) {
                auto* arrow = new QToolButton;
                arrow->setAutoRaise(true);
                arrow->setText(d.collapsed(i) ? "▸" : "▾");
                QObject::connect(arrow, &QToolButton::clicked, [canvas, i] { canvas->doc().setCollapsed(i, !canvas->doc().collapsed(i)); canvas->onLayersChange(); });
                h->addWidget(arrow);
            }
            // flag icons at the right end (#468): Alpha Lock (Raster Layer rows; Vector rows keep a blank slot) and Inherit Alpha, grey outline off, white on a blue tile on
            for (bool lock : {true, false}) {
                if (group) break;
                if (lock && d.vector(i)) { h->addSpacing(24 + h->spacing()); continue; }
                auto* fb = new QToolButton;
                fb->setCheckable(true);
                fb->setFixedSize(24, 24);
                fb->setToolTip(lock ? "Alpha Lock" : "Inherit Alpha");
                auto paint = [fb, lock](bool on) {
                    QPixmap pm(18, 18);
                    pm.fill(Qt::transparent);
                    QPainter p(&pm);
                    p.setRenderHint(QPainter::Antialiasing);
                    QColor c = on ? Qt::white : QColor(102, 102, 102);
                    p.setPen(QPen(c, 1.5));
                    if (lock) {
                        p.drawRoundedRect(QRectF(1.5, 1.5, 15, 15), 1, 1);
                        p.fillRect(QRectF(1.5, 1.5, 4, 4), c);
                        p.fillRect(QRectF(9.5, 1.5, 4, 4), c);
                        p.drawRoundedRect(QRectF(5, 8, 8, 7), 1, 1);
                        p.drawArc(QRectF(6.5, 3.5, 5, 5), 0, 180 * 16);
                    } else {
                        QFont f("serif", 1);
                        f.setItalic(true);
                        f.setPixelSize(14);
                        p.setFont(f);
                        p.drawText(QPointF(1, 12), "α");
                        p.drawLine(QPointF(13, 3), QPointF(13, 14));
                        p.drawPolyline(QPolygonF({QPointF(10, 11), QPointF(13, 14), QPointF(16, 11)}));
                    }
                    p.end();
                    fb->setIcon(pm);
                    fb->setStyleSheet(on ? "QToolButton { background: #3a5f8f; border: none; border-radius: 4px; }" : "QToolButton { border: none; }");
                };
                fb->setChecked(lock ? d.alphaLock(i) : d.inheritAlpha(i));
                paint(fb->isChecked());
                QObject::connect(fb, &QToolButton::toggled, [canvas, i, lock, paint](bool on) {
                    if (lock) canvas->doc().setAlphaLock(i, on);
                    else canvas->doc().setInheritAlpha(i, on);
                    paint(on);
                    canvas->update();
                });
                h->addWidget(fb);
            }
            auto* it = new QListWidgetItem(layerList);
            it->setData(Qt::UserRole, i);
            it->setData(Qt::UserRole + 1, group);
            it->setSizeHint(row->sizeHint());
            layerList->setItemWidget(it, row);
            if (i == d.current()) currentRow = layerList->count() - 1;
            if (group && d.collapsed(i))  // a collapsed group's children have no rows
                for (int gd = d.depth(i); i > 0 && d.depth(i - 1) > gd;) i--;
        }
        if (layerList->selStep != d.stepId()) layerList->sel.clear();
        layerList->setCurrentRow(currentRow);
        for (int r = 0; r < layerList->count(); r++) if (layerList->sel.count(layerList->item(r)->data(Qt::UserRole).toInt())) layerList->item(r)->setSelected(true);
        layerOpacity->setValue(std::lround(d.opacity(d.current()) * 100));
        layerOpacityValue->setText(QString("%1 %").arg(layerOpacity->value()));
        canvas->update();
        if (kindChanged) kindChanged();
        if (updateTitle) updateTitle();
    };
    canvas->onLayersChange = refresh;
    layerList->onDrop = [canvas, layerList](int from, int gap, int inside) {  // nothing drops below the Background, or into its own block
        Document& d = canvas->doc();
        auto at = [&](int row) { return layerList->item(row)->data(Qt::UserRole).toInt(); };
        bool moved;
        auto items = layerList->sel.count(from) ? layerList->items(from) : std::vector<int>{from};  // dragging a selected row drags the whole selection (#446)
        if (inside >= 0) moved = !layerList->sel.count(at(inside)) && at(inside) != from && d.moveItems(items, at(inside), d.depth(at(inside)) + 1);  // top item of the group
        else moved = gap < layerList->count() && d.moveItems(items, at(gap) + 1, d.depth(at(gap)));  // above that row, at its level
        if (moved) QTimer::singleShot(0, [canvas] { canvas->onLayersChange(); });
    };
    layerList->onClick = [canvas, layerList, refresh](int i, Qt::KeyboardModifiers m) {  // Ctrl toggles a row, Shift selects from the current row; the clicked row is current
        Document& d = canvas->doc();
        auto& sel = layerList->sel;
        if (!(m & (Qt::ControlModifier | Qt::ShiftModifier))) sel.clear();
        else if (m & Qt::ShiftModifier) {
            int a = -1, b = -1;
            for (int r = 0; r < layerList->count(); r++) {
                int k = layerList->item(r)->data(Qt::UserRole).toInt();
                if (k == d.current()) a = r;
                if (k == i) b = r;
            }
            sel.clear();
            for (int r = std::min(a < 0 ? b : a, b); r <= std::max(a, b); r++) sel.insert(layerList->item(r)->data(Qt::UserRole).toInt());
        } else {
            if (sel.empty()) sel.insert(d.current());
            if (!sel.erase(i)) sel.insert(i);
        }
        d.setCurrent(i);
        layerList->selStep = d.stepId();
        refresh();
    };
    QObject::connect(layerList, &QListWidget::currentRowChanged, [canvas, layerList, refresh](int row) {
        if (row < 0) return;
        canvas->doc().setCurrent(layerList->item(row)->data(Qt::UserRole).toInt());
        refresh();
    });
    // double-click a group row (not its eye or arrow, which take the click) opens or closes it; a filter, because refresh() rebuilds the rows on the first click
    struct DoubleClick : QObject {
        std::function<bool(QPoint)> on;
        bool eventFilter(QObject*, QEvent* e) override { return e->type() == QEvent::MouseButtonDblClick && on(static_cast<QMouseEvent*>(e)->pos()); }
    };
    auto* dbl = new DoubleClick;
    dbl->on = [canvas, layerList](QPoint pos) {
        auto* it = layerList->itemAt(pos);
        if (!it) return false;
        int i = it->data(Qt::UserRole).toInt();
        if (!canvas->doc().isGroup(i)) return false;
        canvas->doc().setCollapsed(i, !canvas->doc().collapsed(i));
        canvas->onLayersChange();
        return true;
    };
    layerList->viewport()->installEventFilter(dbl);
    layerList->setContextMenuPolicy(Qt::CustomContextMenu);  // right-click a row: Rasterize, enabled on a Vector Layer only (#200)
    QObject::connect(layerList, &QListWidget::customContextMenuRequested, [canvas, layerList, refresh](QPoint pos) {
        auto* it = layerList->itemAt(pos);
        if (!it) return;
        int i = it->data(Qt::UserRole).toInt();
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
        {"+ Group", "Add &Group", [layerList](Document& d) { if (layerList->sel.size() > 1) d.wrap(layerList->items(d.current())); else d.addGroup(); }},
        {"↑", "&Up", [layerList](Document& d) { d.stepItems(layerList->items(d.current()), true); }},
        {"↓", "Dow&n", [layerList](Document& d) { d.stepItems(layerList->items(d.current()), false); }},
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
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(new QLabel("Size"));
    sizeRow->addWidget(size);
    sizeRow->addWidget(sizeValue);
    brushBoxCol->addLayout(sizeRow);
    auto* opacityRow = new QHBoxLayout;
    opacityRow->addWidget(new QLabel("Opacity"));
    opacityRow->addWidget(opacity);
    opacityRow->addWidget(opacityValue);
    brushBoxCol->addLayout(opacityRow);
    auto* smoothRow = new QHBoxLayout;
    smoothRow->addWidget(new QLabel("Smoothing"));
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
    auto* bucketBox = new QWidget;  // Bucket Fill's own Threshold (0-255) and Opacity (#607)
    auto* bucketCol = new QVBoxLayout(bucketBox);
    bucketCol->setContentsMargins(0, 0, 0, 0);
    auto* thresholdRow = new QHBoxLayout;
    auto* threshold = new QSlider(Qt::Horizontal);
    threshold->setRange(0, 255);
    auto* thresholdValue = new QLabel;
    QObject::connect(threshold, &QSlider::valueChanged, [canvas, thresholdValue](int v) { canvas->setBucketThreshold(v); thresholdValue->setText(QString::number(v)); });
    threshold->setValue(15);
    thresholdRow->addWidget(new QLabel("Threshold"));
    thresholdRow->addWidget(threshold);
    thresholdRow->addWidget(thresholdValue);
    bucketCol->addLayout(thresholdRow);
    auto* bucketOpacityRow = new QHBoxLayout;
    auto* bucketOpacity = new QSlider(Qt::Horizontal);  // percent
    bucketOpacity->setRange(1, 100);
    auto* bucketOpacityValue = new QLabel;
    QObject::connect(bucketOpacity, &QSlider::valueChanged, [canvas, bucketOpacityValue](int v) { canvas->setBucketOpacity(v / 100.f); bucketOpacityValue->setText(QString("%1 %").arg(v)); });
    bucketOpacity->setValue(100);
    bucketOpacityRow->addWidget(new QLabel("Opacity"));
    bucketOpacityRow->addWidget(bucketOpacity);
    bucketOpacityRow->addWidget(bucketOpacityValue);
    bucketCol->addLayout(bucketOpacityRow);
    auto* expandRow = new QHBoxLayout;
    auto* expand = new QSpinBox;  // px, negative shrinks
    expand->setRange(-100, 100);
    expand->setSuffix(" px");
    QObject::connect(expand, &QSpinBox::valueChanged, [canvas](int v) { canvas->setBucketExpand(v); });
    expandRow->addWidget(new QLabel("Expand/shrink"));
    expandRow->addWidget(expand);
    bucketCol->addLayout(expandRow);
    auto* sampleCurrent = new QRadioButton("Sample current only");
    auto* sampleMerged = new QRadioButton("Sample merged");
    sampleMerged->setChecked(true);
    QObject::connect(sampleMerged, &QRadioButton::toggled, [canvas](bool on) { canvas->setBucketMerged(on); });
    bucketCol->addWidget(sampleCurrent);
    bucketCol->addWidget(sampleMerged);
    auto* bucketAA = new QCheckBox("Antialiased");
    bucketAA->setChecked(true);
    QObject::connect(bucketAA, &QCheckBox::toggled, [canvas](bool on) { canvas->setBucketAntialiased(on); });
    bucketCol->addWidget(bucketAA);
    auto* refBox = new QWidget;  // the Reference tool's picks' Opacity (#544), shown only with picks
    auto* refRow = new QHBoxLayout(refBox);
    refRow->setContentsMargins(0, 0, 0, 0);
    auto* refOpacity = new QSlider(Qt::Horizontal);  // percent
    refOpacity->setRange(1, 100);
    auto* refOpacityValue = new QLabel;
    QObject::connect(refOpacity, &QSlider::valueChanged, [canvas, refOpacity, refOpacityValue](int v) { canvas->setRefOpacity(v / 100.0, refOpacity->isSliderDown()); refOpacityValue->setText(QString("%1 %").arg(v)); });
    QObject::connect(refOpacity, &QSlider::sliderReleased, [canvas, refOpacity] { canvas->setRefOpacity(refOpacity->value() / 100.0, false); });
    refRow->addWidget(new QLabel("Opacity"));
    refRow->addWidget(refOpacity);
    refRow->addWidget(refOpacityValue);
    refBox->hide();
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
        eraseBox->setVisible(tool <= LassoFillTool || tool == BucketFillTool);
        brushBox->setVisible(tool == BrushTool);
        lassoBox->setVisible(tool == LassoFillTool);
        bucketBox->setVisible(tool == BucketFillTool);
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
        auto o = canvas->refOpacity();
        refBox->setVisible(o.has_value());
        if (o && !refOpacity->isSliderDown()) {
            QSignalBlocker b(refOpacity);
            refOpacity->setValue(std::lround(*o * 100));
            refOpacityValue->setText(QString("%1 %").arg(refOpacity->value()));
        }
        if (canvas->editingNodes()) editTypes->setVisible(!canvas->bezierSelected()), bezierTypes->setVisible(canvas->bezierSelected());
    };
    auto* optionsPanel = new QWidget;
    auto* optionsCol = new QVBoxLayout(optionsPanel);
    optionsCol->addWidget(eraseBox);
    optionsCol->addWidget(brushBox);
    optionsCol->addWidget(lassoBox);
    optionsCol->addWidget(bucketBox);
    optionsCol->addWidget(refBox);
    optionsCol->addWidget(curveBox);
    optionsCol->addWidget(widthBox);
    optionsCol->addWidget(paintBox);
    optionsCol->addWidget(swatchBox);
    optionsCol->addWidget(scaleWidths);
    optionsCol->addWidget(penTypes);
    optionsCol->addWidget(editTypes);
    optionsCol->addWidget(bezierTypes);
    optionsCol->addStretch();  // spare height goes below the options, not between them
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
        {"\u2B1A Box", ""}, // Box Select
        {"\U0001F5BC Reference", "R"}, // Reference (R, #535)
        {"\U0001FAA3 Bucket Fill", "G"} // Bucket Fill (G, #607)
    };
    int lastTool[2] = {BrushTool, SpiroPenTool};  // last tool used on a Raster, a Vector Layer (#161); Select fits both, so it is no one's last
    for (int i = 0; i < 8; i++) {
        auto* a = toolGroup->addAction(kTools[i].text);
        a->setCheckable(true);
        a->setChecked(i == 0);
        a->setShortcut(QKeySequence(kTools[i].key));
        window.addAction(a);
        QObject::connect(a, &QAction::triggered, [canvas, i, &lastTool] { canvas->setTool(i); if (i != SelectTool && i != ReferenceTool) lastTool[toolFitsKind(true, i)] = i; });
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolCol->addWidget(b);
    }
    toolCol->addStretch();
    // Only the tools that fit the current layer's kind are enabled, so their keys do nothing on the other kind (#170); a switch that disables the tool picks the last one used on the new kind.
    bool inGroup = false;
    kindChanged = [&] {
        bool has = canvas->hasDoc(), vec = canvas->doc().vector(canvas->doc().current()), grp = has && canvas->doc().isGroup(canvas->doc().current());
        canvas->setEnabled(has);  // a disabled widget gets no pointer or key events
        for (auto* w : {colorDock->widget(), layerPanel, optionsPanel, navPanel}) w->setEnabled(has);
        for (auto* a : docActions) a->setEnabled(has);
        for (auto* a : notOnGroup) a->setEnabled(has && !grp);
        auto tools = toolGroup->actions();
        for (int i = 0; i < 8; i++) tools[i]->setEnabled(has && toolFitsKind(vec, i, grp));
        brushPanel->setEnabled(has && !vec);
        int now = toolForLayer(vec, tools.indexOf(toolGroup->checkedAction()), lastTool[0], lastTool[1], grp);
        if (inGroup && !grp) now = lastTool[vec];  // leaving a group restores the kind's last tool (#161)
        inGroup = grp;
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

    auto openPath = [&](QString path) {  // Open and the command line: a tab per file
        auto doc = readOra(path);
        if (!doc) { QMessageBox::warning(&window, "Open", "Could not read " + path); return; }
        openTab(std::move(*doc), path);
    };
    auto* fileOpen = fileMenu->addAction("&Open...");
    fileOpen->setShortcut(QKeySequence::Open);
    QObject::connect(fileOpen, &QAction::triggered, [&] {
        QString path = QFileDialog::getOpenFileName(&window, "Open", {}, "OpenRaster (*.ora)");
        if (!path.isEmpty()) openPath(path);
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
    auto* fileImportRef = fileMenu->addAction("Import &Reference Image...");
    docActions.push_back(fileImportRef);
    QObject::connect(fileImportRef, &QAction::triggered, [&] {
        QString path = QFileDialog::getOpenFileName(&window, "Import Reference Image", {}, "PNG (*.png)");
        if (path.isEmpty()) return;
        QImage img(path);
        if (img.isNull()) return void(QMessageBox::warning(&window, "Import Reference Image", "Could not read " + path));
        toolGroup->actions()[ReferenceTool]->trigger();
        canvas->importReference(img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
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
        auto* discard = btns->addButton("Discard All", QDialogButtonBox::DestructiveRole);  // quits saving nothing, whatever is checked (#271 story 15)
        btns->addButton(QDialogButtonBox::Cancel);
        go->setDefault(true);
        bool discardAll = false;
        QObject::connect(discard, &QPushButton::clicked, &dlg, [&] { discardAll = true; dlg.accept(); });
        col->addWidget(btns);
        auto checked = [&] { std::vector<int> c; for (int r = 0; r < list->count(); ++r) if (list->item(r)->checkState() == Qt::Checked) c.push_back(dirty[r]); return c; };
        QObject::connect(list, &QListWidget::itemChanged, [&] {
            int n = int(checked().size()), m = int(dirty.size()) - n;
            go->setText(!n ? "Discard All" : m ? QString("Save %1, Discard %2").arg(n).arg(m) : "Save All");
        });
        QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return false;
        if (discardAll) return true;
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
    editMenu->addSeparator();
    QObject::connect(key(editMenu->addAction("Cu&t"), {QKeySequence("Ctrl+X")}), &QAction::triggered, [canvas] { canvas->copy(true); });
    QObject::connect(key(editMenu->addAction("&Copy"), {QKeySequence("Ctrl+C")}), &QAction::triggered, [canvas] { canvas->copy(false); });
    notOnGroup = {editMenu->actions().end()[-2], editMenu->actions().end()[-1]};
    auto pasteReference = [canvas, toolGroup](const QImage& img) {  // as an import is placed (#547, #524): the view centre, scaled down to fit, picked
        toolGroup->actions()[ReferenceTool]->trigger();
        canvas->importReference(img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    };
    QAction* beforePaste = nullptr;  // the tool Paste switched away from; Enter or a click outside brings it back
    QObject::connect(key(editMenu->addAction("&Paste"), {QKeySequence("Ctrl+V")}), &QAction::triggered, [canvas, toolGroup, &beforePaste, pasteReference] {
        QImage img = QGuiApplication::clipboard()->image();
        if (img.isNull()) return;  // text or nothing on the clipboard: no paste
        QPointF at = canvas->pastePoint();
        bool newLayer = true;
        Document& d = canvas->doc();
        {  // it always asks (#509, look: #511); a Vector Layer or group has no Same layer row (#564, #625)
            bool raster = !d.vector(d.current()) && !d.isGroup(d.current());
            QMenu menu;
            auto* head = new QWidgetAction(&menu);
            auto* box = new QWidget;
            auto* row = new QHBoxLayout(box);
            auto* thumb = new QLabel;
            thumb->setPixmap(QPixmap::fromImage(img.scaled(48, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            row->addWidget(thumb);
            row->addWidget(new QLabel(QString("<b>Paste image</b><br>%1 × %2 px").arg(img.width()).arg(img.height())), 1);
            head->setDefaultWidget(box);
            menu.addAction(head);
            menu.addSeparator();
            auto* same = raster ? menu.addAction(QString("&Same layer: into “%1”").arg(d.name(d.current()))) : nullptr;
            auto* fresh = menu.addAction("&New layer");
            auto* ref = menu.addAction("&Reference image");
            menu.addAction("Cancel");
            menu.setActiveAction(raster ? same : fresh);  // Enter takes it
            auto* chosen = menu.exec(canvas->pasteMenuPoint());
            if (chosen == ref) return pasteReference(img);
            if (!chosen || (chosen != same && chosen != fresh)) return;  // Cancel, Esc or a click outside
            newLayer = chosen == fresh;
        }
        beforePaste = toolGroup->checkedAction();
        toolGroup->actions()[SelectTool]->trigger();
        canvas->paste(img.convertToFormat(QImage::Format_RGBA8888), at, newLayer);
    });
    QObject::connect(key(editMenu->addAction("Paste as Re&ference Image"), {QKeySequence("Ctrl+Shift+V")}), &QAction::triggered, [pasteReference] {
        QImage img = QGuiApplication::clipboard()->image();
        if (!img.isNull()) pasteReference(img);  // any layer kind, no menu (#549)
    });
    canvas->onPasteEnd = [&beforePaste] { if (beforePaste) beforePaste->trigger(); beforePaste = nullptr; };

    auto* viewMenu = window.menuBar()->addMenu("&View");
    QObject::connect(key(viewMenu->addAction("&Mirror"), {QKeySequence("M")}), &QAction::triggered, [canvas] { canvas->mirror(); });
    auto* showRefs = key(viewMenu->addAction("Show &References"), {QKeySequence("Shift+R")});
    showRefs->setCheckable(true);
    showRefs->setChecked(true);
    QObject::connect(showRefs, &QAction::toggled, [canvas](bool on) { canvas->setShowReferences(on); });
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
    QObject::connect(key(layerMenu->addAction("&Group current item"), {QKeySequence("Ctrl+G")}), &QAction::triggered, [canvas, refresh, layerList] { canvas->doc().wrap(layerList->items(canvas->doc().current())); refresh(); });

    auto* toolsMenu = window.menuBar()->addMenu("&Tools");
    toolsMenu->addActions(toolGroup->actions());
    toolsMenu->addSeparator();
    auto* erase = key(toolsMenu->addAction("&Erase mode"), {QKeySequence("E")});
    erase->setCheckable(true);
    QObject::connect(erase, &QAction::toggled, [canvas, eraseBox, erase, opacity, lassoOpacity](bool on) { canvas->setErase(on); canvas->onRadiusChange(); opacity->setValue(std::lround(canvas->opacity() * 100)); lassoOpacity->setValue(std::lround(canvas->lassoOpacity() * 100)); eraseBox->setChecked(canvas->erase()); erase->setChecked(canvas->erase()); });
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
    for (const QString& path : app.arguments().mid(1)) openPath(path);  // mydraw a.ora b.ora: a tab each
    return app.exec();
}
