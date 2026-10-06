// The Tool Options dock (#58, #171): each tool's settings, shown for the tool in use.
#include "shell.h"
#include <cmath>
#include <memory>
#include <optional>
#include <QCheckBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include "colorpicker.h"

namespace {

const std::pair<const char*, Document::PaintMode> kPaints[] = {{"Stroke", Document::PaintMode::Stroke}, {"Fill", Document::PaintMode::Fill}, {"Stroke+Fill", Document::PaintMode::Both}};
const BezierType kBezierTypes[] = {BezierType::Cusp, BezierType::Smooth, BezierType::Symmetric};

// The dock's widgets that showOptions, showPicks and the repaint hook read, apart from the boxes' own controls.
struct Options {
    Canvas* canvas;
    ColorPicker* picker;
    QDockWidget* colorDock;
    QCheckBox* eraseBox = nullptr;
    QWidget *brushBox = nullptr, *lassoBox = nullptr, *bucketBox = nullptr, *refBox = nullptr, *widthBox = nullptr, *paintBox = nullptr, *swatchBox = nullptr, *curveBox = nullptr;
    QSlider *brushOpacity = nullptr, *lassoOpacity = nullptr, *refOpacity = nullptr, *pathWidth = nullptr;
    QLabel *refOpacityValue = nullptr, *pathWidthValue = nullptr;
    QToolButton *paintButtons[3] = {}, *swatches[2] = {}, *curveButtons[2] = {};
    QListWidget *penTypes = nullptr, *editTypes = nullptr, *bezierTypes = nullptr;
    QCheckBox* scaleWidths = nullptr;
};

QVBoxLayout* column(QWidget* box) {
    auto* col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    return col;
}

// The global Size and Opacity sliders (#209), the Stabilizer (#292), for the Brush only.
QWidget* brushOptions(Options& o) {
    Canvas* canvas = o.canvas;
    auto [size, sizeValue, sizeRow] = sliderRow("Size", kMinRadius, kMaxRadius, "%1 px");  // radius in px
    size->setValue(std::lround(canvas->radius()));
    QObject::connect(size, &QSlider::valueChanged, [canvas](int v) { canvas->setRadius(v); });
    canvas->onRadiusChange = [canvas, size, sizeValue] {
        QSignalBlocker b(size);  // blocked so the slider's int doesn't round the radius back
        size->setValue(std::lround(canvas->radius()));
        sizeValue->setText(QString("%1 px").arg(size->value()));
    };
    auto opacity = sliderRow("Opacity", 1, 100, "%1 %");  // percent
    o.brushOpacity = opacity.slider;
    opacity.slider->setValue(std::lround(canvas->opacity() * 100));
    QObject::connect(opacity.slider, &QSlider::valueChanged, [canvas](int v) { canvas->setOpacity(v / 100.f); });
    auto* box = new QWidget;
    auto* col = column(box);
    col->addLayout(sizeRow);
    col->addLayout(opacity.row);
    auto smoothing = sliderRow("Smoothing", 0, 50, "%1");  // samples averaged, 0 = off
    QObject::connect(smoothing.slider, &QSlider::valueChanged, canvas, &Canvas::setSmoothing);
    col->addLayout(smoothing.row);
    auto* finishLine = new QCheckBox("Finish Line");
    finishLine->setChecked(true);
    QObject::connect(finishLine, &QCheckBox::toggled, canvas, &Canvas::setFinishLine);
    col->addWidget(finishLine);
    auto* sensors = new QCheckBox("Stabilize Sensors");
    QObject::connect(sensors, &QCheckBox::toggled, canvas, &Canvas::setStabilizeSensors);
    col->addWidget(sensors);
    return box;
}

// Lasso Fill's own Opacity (#212), starting solid, and its own Smoothing and Finish Line (#264).
QWidget* lassoOptions(Options& o) {
    Canvas* canvas = o.canvas;
    auto* box = new QWidget;
    auto* col = column(box);
    auto opacity = sliderRow("Opacity", 1, 100, "%1 %");  // percent
    o.lassoOpacity = opacity.slider;
    QObject::connect(opacity.slider, &QSlider::valueChanged, [canvas](int v) { canvas->setLassoOpacity(v / 100.f); });
    opacity.slider->setValue(100);
    col->addLayout(opacity.row);
    col->addWidget(new QLabel("Smoothing"));
    auto smoothing = sliderRow("", 0, 50, "%1");
    QObject::connect(smoothing.slider, &QSlider::valueChanged, canvas, &Canvas::setLassoSmoothing);
    col->addLayout(smoothing.row);
    auto* finish = new QCheckBox("Finish Line");
    finish->setChecked(true);
    QObject::connect(finish, &QCheckBox::toggled, canvas, &Canvas::setLassoFinishLine);
    col->addWidget(finish);
    return box;
}

// Bucket Fill's own Threshold (0-255) and Opacity (#607), then Expand/shrink, Sample merged and Antialiased (#608).
QWidget* bucketOptions(Options& o) {
    Canvas* canvas = o.canvas;
    auto* box = new QWidget;
    auto* col = column(box);
    auto threshold = sliderRow("Threshold", 0, 255, "%1");
    QObject::connect(threshold.slider, &QSlider::valueChanged, canvas, &Canvas::setBucketThreshold);
    threshold.slider->setValue(15);
    col->addLayout(threshold.row);
    auto opacity = sliderRow("Opacity", 1, 100, "%1 %");  // percent
    QObject::connect(opacity.slider, &QSlider::valueChanged, [canvas](int v) { canvas->setBucketOpacity(v / 100.f); });
    opacity.slider->setValue(100);
    col->addLayout(opacity.row);
    auto* expandRow = new QHBoxLayout;
    auto* expand = new QSpinBox;  // px, negative shrinks
    expand->setRange(-100, 100);
    expand->setSuffix(" px");
    QObject::connect(expand, &QSpinBox::valueChanged, [canvas](int v) { canvas->setBucketExpand(v); });
    expandRow->addWidget(new QLabel("Expand/shrink"));
    expandRow->addWidget(expand);
    col->addLayout(expandRow);
    auto* sampleCurrent = new QRadioButton("Sample current only");
    auto* sampleMerged = new QRadioButton("Sample merged");
    sampleMerged->setChecked(true);
    QObject::connect(sampleMerged, &QRadioButton::toggled, [canvas](bool on) { canvas->setBucketMerged(on); });
    col->addWidget(sampleCurrent);
    col->addWidget(sampleMerged);
    auto* aa = new QCheckBox("Antialiased");
    aa->setChecked(true);
    QObject::connect(aa, &QCheckBox::toggled, [canvas](bool on) { canvas->setBucketAntialiased(on); });
    col->addWidget(aa);
    return box;
}

// The Reference tool's picks' Opacity (#544), shown only with picks.
QWidget* refOptions(Options& o) {
    Canvas* canvas = o.canvas;
    auto* box = new QWidget;
    auto [refOpacity, refOpacityValue, refRow] = sliderRow("Opacity", 1, 100, "%1 %");  // percent
    o.refOpacity = refOpacity;
    o.refOpacityValue = refOpacityValue;
    refRow->setContentsMargins(0, 0, 0, 0);
    box->setLayout(refRow);
    QObject::connect(refOpacity, &QSlider::valueChanged, [canvas, refOpacity](int v) { canvas->setRefOpacity(v / 100.0, refOpacity->isSliderDown()); });
    QObject::connect(refOpacity, &QSlider::sliderReleased, [canvas, refOpacity] { canvas->setRefOpacity(refOpacity->value() / 100.0, false); });
    box->hide();
    return box;
}

QWidget* widthOptions(Options& o) {
    Canvas* canvas = o.canvas;
    auto* box = new QWidget;
    auto [pathWidth, pathWidthValue, widthRow] = sliderRow("Width", kMinRadius, kMaxRadius, "%1 px");  // px, of a new Vector Path
    o.pathWidth = pathWidth;
    o.pathWidthValue = pathWidthValue;
    widthRow->setContentsMargins(0, 0, 0, 0);
    box->setLayout(widthRow);
    // On picks a drag applies once, on release: one undo step (#305)
    QObject::connect(pathWidth, &QSlider::valueChanged, [canvas, pathWidth](int v) { if (!pathWidth->isSliderDown() || canvas->pickedPaths().empty()) canvas->setPathWidth(v); });
    QObject::connect(pathWidth, &QSlider::sliderReleased, [canvas, pathWidth] { canvas->setPathWidth(pathWidth->value()); });
    pathWidth->setValue(10);
    return box;
}

// Paint mode of a new Vector Path, segmented (#323 take A).
QWidget* paintOptions(Options& o) {
    Canvas* canvas = o.canvas;
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(new QLabel("Paint"));
    for (int k = 0; k < 3; k++) {
        auto [name, mode] = kPaints[k];
        auto* b = o.paintButtons[k] = new QToolButton;
        b->setText(name);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(mode == Document::PaintMode::Stroke);
        QObject::connect(b, &QToolButton::toggled, [canvas, mode](bool on) { if (on) canvas->setPaintMode(mode); });
        row->addWidget(b);
    }
    return box;
}

// The Pen's Stroke and Fill swatches side by side; the checked one, ringed orange, is the Color dock's target (#323 take A).
QWidget* swatchOptions(Options& o) {
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        auto* b = o.swatches[i] = new QToolButton;
        b->setText(i ? "Fill" : "Stroke");
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(24, 24));
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(!i);
        b->setStyleSheet("QToolButton { border: 2px solid transparent; } QToolButton:checked { border-color: #ffb84d; }");
        row->addWidget(b);
    }
    row->addStretch();
    return box;
}

// In Select with paths picked the swatches show theirs, hatched where they differ (#303); otherwise the Pen's.
void paintSwatches(const Options& o) {
    Canvas* canvas = o.canvas;
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
        o.swatches[i]->setIcon(icon);
    }
}

void nameColorDock(const Options& o, int tool) {
    o.colorDock->setWindowTitle(tool != SpiroPenTool && tool != SelectTool ? "Color" : o.swatches[1]->isChecked() ? "Color: Fill" : "Color: Stroke");
}

// The Color dock and the swatches: a toggled swatch is the dock's target; with picks, a drag recolors them once on release, so the
// drag is one undo step (#304).
void wireColor(const std::shared_ptr<Options>& o) {
    Canvas* canvas = o->canvas;
    ColorPicker* picker = o->picker;
    for (int i = 0; i < 2; i++)
        QObject::connect(o->swatches[i], &QToolButton::toggled, [o, i](bool on) { if (!on) return; o->canvas->setFillActive(i); o->picker->setColor(o->canvas->color()); nameColorDock(*o, SpiroPenTool); });
    auto pending = std::make_shared<std::optional<QColor>>();
    picker->onChange = [o, pending](const QColor& c) {
        if (o->canvas->selecting() && !o->canvas->pickedPaths().empty()) *pending = c;
        else o->canvas->setColor(c), paintSwatches(*o);
    };
    picker->onRelease = [canvas, pending] { if (*pending) canvas->setColor(**pending); pending->reset(); };
    canvas->onColorPick = [o](const QColor& c) { o->picker->setColor(c); paintSwatches(*o); };
}

// The node type lists: the Spiro Pen's for new nodes, Node/Knot edit's retyping the selection apart from the pen's (#215), and its
// Bezier list when a Bezier path is selected (#314).
void nodeTypeLists(Options& o) {
    Canvas* canvas = o.canvas;
    o.penTypes = new QListWidget;
    o.editTypes = new QListWidget;
    for (auto* l : {o.penTypes, o.editTypes}) for (const auto& t : kNodeTypes) l->addItem(t.name);
    o.penTypes->setCurrentRow(2);  // G2, SpiroPen's starting type
    QObject::connect(o.penTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->setNodeType(kNodeTypes[row].type); });
    QObject::connect(o.editTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->retypeSelected(kNodeTypes[row].type); });
    o.bezierTypes = new QListWidget;
    o.bezierTypes->addItems({"Cusp", "Smooth", "Symmetric"});
    QObject::connect(o.bezierTypes, &QListWidget::currentRowChanged, [canvas](int row) { if (row >= 0) canvas->retypeSelected(kBezierTypes[row]); });
}

// The Pen's Spiro / Bezier toggle, Spiro by default (#308); Bezier has no node picker.
QWidget* curveOptions(Options& o) {
    Canvas* canvas = o.canvas;
    QListWidget* penTypes = o.penTypes;
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(new QLabel("Curve"));
    for (bool bez : {false, true}) {
        auto* b = new QToolButton;
        b->setText(bez ? "Bezier" : "Spiro");
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setChecked(!bez);
        o.curveButtons[bez] = b;
        QObject::connect(b, &QToolButton::toggled, [canvas, penTypes, bez](bool on) { if (on) { canvas->setBezier(bez); penTypes->setVisible(!bez && !canvas->selecting()); } });
        row->addWidget(b);
    }
    row->addStretch();
    return box;
}

void showOptions(const Options& o, int tool) {
    o.scaleWidths->setVisible(tool == SelectTool);
    o.eraseBox->setVisible(tool <= LassoFillTool || tool == BucketFillTool);
    o.brushBox->setVisible(tool == BrushTool);
    o.lassoBox->setVisible(tool == LassoFillTool);
    o.bucketBox->setVisible(tool == BucketFillTool);
    o.widthBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
    o.paintBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
    o.swatchBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
    o.picker->setColor(o.canvas->color());  // the Pen's active swatch or the brush color
    nameColorDock(o, tool);
    o.curveBox->setVisible(tool == SpiroPenTool || tool == SelectTool);
    o.penTypes->setVisible(tool == SpiroPenTool && !o.canvas->bezier());
    o.editTypes->setVisible(tool == NodeKnotEditTool && !o.canvas->bezierSelected());
    o.bezierTypes->setVisible(tool == NodeKnotEditTool && o.canvas->bezierSelected());
}

// Select shows its picks' Width, Paint and colors, "mixed" where they differ; no pick shows the Pen's next path (#303).
void showPicks(const Options& o) {
    Canvas* canvas = o.canvas;
    auto ps = canvas->pickedPaths();
    double w = ps.empty() ? canvas->pathWidth() : ps[0].width;
    auto mode = ps.empty() ? canvas->paintMode() : ps[0].mode;
    bool wMixed = false, mMixed = false;
    for (const auto& p : ps) { wMixed |= p.width != w; mMixed |= p.mode != mode; }
    QSignalBlocker b(o.pathWidth);
    o.pathWidth->setValue(std::lround(w));
    o.pathWidthValue->setText(wMixed ? "mixed" : QString("%1 px").arg(std::lround(w)));
    for (int k = 0; k < 3; k++) {
        QSignalBlocker bb(o.paintButtons[k]);
        o.paintButtons[k]->setAutoExclusive(false);  // so a mixed pick can leave all three unchecked
        o.paintButtons[k]->setChecked(!mMixed && kPaints[k].second == mode);
        o.paintButtons[k]->setAutoExclusive(true);
    }
    bool hasSpiro = false, hasBezier = false;
    for (const auto& p : ps) (p.kind == Document::PathKind::Bezier ? hasBezier : hasSpiro) = true;
    if (ps.empty()) hasBezier = canvas->bezier(), hasSpiro = !hasBezier;
    for (int k = 0; k < 2; k++) {
        QSignalBlocker bb(o.curveButtons[k]);
        o.curveButtons[k]->setAutoExclusive(false);  // so a mixed pick leaves both unchecked
        o.curveButtons[k]->setChecked(k ? hasBezier && !hasSpiro : hasSpiro && !hasBezier);
        o.curveButtons[k]->setAutoExclusive(true);
    }
    paintSwatches(o);
}

// ponytail: re-reads the picks on every repaint; a pick-changed signal if drags ever stutter
void onRepaint(const Options& o) {
    Canvas* canvas = o.canvas;
    if (canvas->selecting()) showPicks(o);
    auto op = canvas->refOpacity();
    o.refBox->setVisible(op.has_value());
    if (op && !o.refOpacity->isSliderDown()) {
        QSignalBlocker b(o.refOpacity);
        o.refOpacity->setValue(std::lround(*op * 100));
        o.refOpacityValue->setText(QString("%1 %").arg(o.refOpacity->value()));
    }
    if (canvas->editingNodes()) o.editTypes->setVisible(!canvas->bezierSelected()), o.bezierTypes->setVisible(canvas->bezierSelected());
}

}  // namespace

void buildToolOptionsDock(Shell& sh) {
    Canvas* canvas = sh.canvas;
    auto o = std::make_shared<Options>(Options{canvas, sh.picker, sh.colorDock});
    o->brushBox = brushOptions(*o);
    o->eraseBox = sh.eraseBox = new QCheckBox("Erase");  // Brush, Lasso Fill and Bucket Fill each keep their own
    o->lassoBox = lassoOptions(*o);
    o->bucketBox = bucketOptions(*o);
    o->refBox = refOptions(*o);
    o->widthBox = widthOptions(*o);
    o->paintBox = paintOptions(*o);
    o->swatchBox = swatchOptions(*o);
    paintSwatches(*o);
    wireColor(o);
    nodeTypeLists(*o);
    o->curveBox = curveOptions(*o);
    o->scaleWidths = new QCheckBox("Scale widths");  // Select's, off by default (#203)
    QObject::connect(o->scaleWidths, &QCheckBox::toggled, [canvas](bool on) { canvas->setScaleWidths(on); });
    canvas->onToolChange = [o](int tool) { showOptions(*o, tool); showPicks(*o); };  // setTool has cleared the picks, so leaving Select shows the Pen's again
    showOptions(*o, BrushTool);
    auto onPaintBefore = canvas->onPaint;
    canvas->onPaint = [onPaintBefore, o] { onPaintBefore(); onRepaint(*o); };
    auto* optionsPanel = new QWidget;
    auto* optionsCol = new QVBoxLayout(optionsPanel);
    for (QWidget* w : std::initializer_list<QWidget*>{o->eraseBox, o->brushBox, o->lassoBox, o->bucketBox, o->refBox, o->curveBox, o->widthBox, o->paintBox,
                                                      o->swatchBox, o->scaleWidths, o->penTypes, o->editTypes, o->bezierTypes})
        optionsCol->addWidget(w);
    optionsCol->addStretch();  // spare height goes below the options, not between them
    sh.optionsDock = new QDockWidget("Tool Options", &sh.window);
    sh.optionsDock->setWidget(optionsPanel);
    sh.window.addDockWidget(Qt::RightDockWidgetArea, sh.optionsDock);
    sh.onEraseChange = [o] { o->canvas->onRadiusChange(); o->brushOpacity->setValue(std::lround(o->canvas->opacity() * 100)); o->lassoOpacity->setValue(std::lround(o->canvas->lassoOpacity() * 100)); };
}
