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

void buildToolOptionsDock(Shell& sh) {
    Canvas* canvas = sh.canvas;
    ColorPicker* picker = sh.picker;
    QDockWidget* colorDock = sh.colorDock;
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

    // Tool Options dock (#58, #171): Erase for Brush and Lasso Fill, path width and node picker for the Spiro Pen, node picker for Node/Knot edit.
    auto* eraseBox = sh.eraseBox = new QCheckBox("Erase");
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
    sh.optionsDock = new QDockWidget("Tool Options", &sh.window);
    sh.optionsDock->setWidget(optionsPanel);
    sh.window.addDockWidget(Qt::RightDockWidgetArea, sh.optionsDock);
    sh.onEraseChange = [canvas, opacity, lassoOpacity] { canvas->onRadiusChange(); opacity->setValue(std::lround(canvas->opacity() * 100)); lassoOpacity->setValue(std::lround(canvas->lassoOpacity() * 100)); };
}
