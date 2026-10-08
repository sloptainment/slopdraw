// The Layers dock and the Layer menu.
#include "shell.h"
#include <cmath>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include "layerlist.h"
#include "navigator.h"

namespace
{

struct DoubleClick : QObject
{
  std::function<bool(QPoint)> on;
  bool eventFilter(QObject *, QEvent *e) override
  {
    return e->type() == QEvent::MouseButtonDblClick &&
           on(static_cast<QMouseEvent *>(e)->pos());
  }
};

// The Layers dock's buttons and the Layer menu's items, in one order (#169).
struct LayerOp
{
  const char *button;
  const char *menu;
  std::function<void(Document &)> op;
};
std::vector<LayerOp> layerOps(LayerList *layerList)
{
  return {
    {"+ Raster", "Add &Raster", [](Document &d) { d.addLayer(); }},
    {"+ Vector", "Add &Vector", [](Document &d) { d.addVectorLayer(); }},
    {"+ Group", "Add &Group",
     [layerList](Document &d)
     {
       if (layerList->sel.size() > 1)
         d.wrap(layerList->items(d.current()));
       else
         d.addGroup();
     }},
    {"↑", "&Up", [layerList](Document &d)
     { d.stepItems(layerList->items(d.current()), true); }},
    {"↓", "Dow&n", [layerList](Document &d)
     { d.stepItems(layerList->items(d.current()), false); }},
    {"🗑", "&Delete", [](Document &d) { d.deleteLayer(d.current()); }}};
}

// A row's flag icon (#468): Alpha Lock or Inherit Alpha, grey outline off, white on
// a blue tile on.
QToolButton *flagButton(Canvas *canvas, const Document &d, int i, bool lock)
{
  auto *fb = new QToolButton;
  fb->setCheckable(true);
  fb->setFixedSize(24, 24);
  fb->setToolTip(lock ? "Alpha Lock" : "Inherit Alpha");
  auto paint = [fb, lock](bool on)
  {
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor c = on ? Qt::white : QColor(102, 102, 102);
    p.setPen(QPen(c, 1.5));
    if (lock)
    {
      p.drawRoundedRect(QRectF(1.5, 1.5, 15, 15), 1, 1);
      p.fillRect(QRectF(1.5, 1.5, 4, 4), c);
      p.fillRect(QRectF(9.5, 1.5, 4, 4), c);
      p.drawRoundedRect(QRectF(5, 8, 8, 7), 1, 1);
      p.drawArc(QRectF(6.5, 3.5, 5, 5), 0, 180 * 16);
    }
    else
    {
      QFont f("serif", 1);
      f.setItalic(true);
      f.setPixelSize(14);
      p.setFont(f);
      p.drawText(QPointF(1, 12), "α");
      p.drawLine(QPointF(13, 3), QPointF(13, 14));
      p.drawPolyline(
        QPolygonF({QPointF(10, 11), QPointF(13, 14), QPointF(16, 11)}));
    }
    p.end();
    fb->setIcon(pm);
    fb->setStyleSheet(
      on ? "QToolButton { background: #3a5f8f; border: none; border-radius: 4px; }"
         : "QToolButton { border: none; }");
  };
  fb->setChecked(lock ? d.alphaLock(i) : d.inheritAlpha(i));
  paint(fb->isChecked());
  QObject::connect(
    fb, &QToolButton::toggled,
    [canvas, i, lock, paint](bool on)
    {
      if (lock)
        canvas->doc().setAlphaLock(i, on);
      else
        canvas->doc().setInheritAlpha(i, on);
      paint(on);
      canvas->update();
    });
  return fb;
}

// A row's colour bars: one per enclosing group, then its own if it is a group.
void addGroupBars(QHBoxLayout *h, const Document &d, int i)
{
  static const char *const hues[] = {"#d9534f", "#5aa0ff", "#7fd27f", "#c58af9"};
  std::vector<int> bars; // the enclosing groups, outermost first, then the row's
                         // own if it is a group
  for (int j = i + 1, depth = d.depth(i); j < d.layerCount() && depth > 0; j++)
    if (d.depth(j) < depth)
    {
      bars.insert(bars.begin(), j);
      depth = d.depth(j);
    }
  if (d.isGroup(i))
    bars.push_back(i);
  for (int g : bars)
  {
    auto *bar = new QFrame;
    bar->setFixedWidth(4);
    bar->setStyleSheet(QString("background: %1;").arg(hues[g % 4]));
    h->addWidget(bar);
  }
}

// A row's visibility toggle.
QToolButton *eyeButton(Canvas *canvas, const Document &d, int i)
{
  auto *eye = new QToolButton;
  eye->setAutoRaise(true);
  eye->setCheckable(true);
  eye->setChecked(d.visible(i));
  eye->setText("👁");
  eye->setFixedSize(eye->sizeHint()); // "–" is narrower; keep the row from shifting
  eye->setText(d.visible(i) ? "👁" : "–");
  QObject::connect(
    eye, &QToolButton::toggled,
    [canvas, eye, i](bool on)
    {
      eye->setText(on ? "👁" : "–");
      canvas->doc().setVisible(i, on);
      canvas->update();
    });
  return eye;
}

// One Layers dock row: colour bars, eye, kind icon, thumbnail, name (#169); a group
// row's arrow takes the kind icon's place (#457).
QWidget *layerRow(Canvas *canvas, const Document &d, int i)
{
  const bool group = d.isGroup(i);
  auto *row = new QWidget;
  auto *h = new QHBoxLayout(row);
  h->setContentsMargins(2, 2, 2, 2);
  addGroupBars(h, d, i);
  auto *thumb = new QLabel;
  QPixmap tp(32, 24); // as tall as the flag buttons
  tp.fill(QColor(200, 200, 200)); // so a transparent layer's thumbnail still shows
  QPainter tpp(&tp);
  tpp.setRenderHint(QPainter::SmoothPixmapTransform);
  tpp.drawImage(tp.rect(), group ? d.groupImage(i) : d.image(i));
  thumb->setPixmap(tp);
  h->addWidget(eyeButton(canvas, d, i));
  if (group)
  {
    auto *arrow = new QToolButton;
    arrow->setAutoRaise(true);
    arrow->setText(d.collapsed(i) ? "▸" : "▾");
    QObject::connect(
      arrow, &QToolButton::clicked,
      [canvas, i]
      {
        canvas->doc().setCollapsed(i, !canvas->doc().collapsed(i));
        canvas->onLayersChange();
      });
    h->addWidget(arrow);
  }
  else
    h->addWidget(new QLabel(d.vector(i) ? "✒" : "🖌"));
  h->addWidget(thumb);
  auto *name = new QLabel(d.name(i));
  if (group)
    name->setStyleSheet("font-weight: bold;");
  h->addWidget(name, 1);
  // flag icons at the right end (#468): Alpha Lock (Raster Layer rows; Vector and
  // group rows keep a blank slot) and Inherit Alpha, grey outline off, white on a
  // blue tile on
  for (bool lock : {true, false})
  {
    if (lock && (group || d.vector(i)))
    {
      h->addSpacing(24 + h->spacing());
      continue;
    }
    h->addWidget(flagButton(canvas, d, i, lock));
  }
  return row;
}

// Rebuilds the rows from the Document, then the opacity slider, the tools and the
// title follow.
void refreshLayers(Shell &sh, QSlider *layerOpacity, QLabel *layerOpacityValue)
{
  Canvas *canvas = sh.canvas;
  LayerList *layerList = sh.layerList;
  Document &d = canvas->doc();
  sh.nav->refresh();
  QSignalBlocker b1(layerList), b2(layerOpacity);
  layerList->clear();
  int currentRow = -1;
  for (int i = d.layerCount() - 1; i >= 0; i--)
  { // top item first
    QWidget *row = layerRow(canvas, d, i);
    const bool group = d.isGroup(i);
    auto *it = new QListWidgetItem(layerList);
    it->setData(Qt::UserRole, i);
    it->setData(Qt::UserRole + 1, group);
    it->setSizeHint(row->sizeHint());
    layerList->setItemWidget(it, row);
    if (i == d.current())
      currentRow = layerList->count() - 1;
    if (group && d.collapsed(i)) // a collapsed group's children have no rows
      for (int gd = d.depth(i); i > 0 && d.depth(i - 1) > gd;)
        i--;
  }
  if (layerList->selStep != d.stepId())
    layerList->sel.clear();
  layerList->setCurrentRow(currentRow);
  for (int r = 0; r < layerList->count(); r++)
    if (layerList->sel.count(layerList->item(r)->data(Qt::UserRole).toInt()))
      layerList->item(r)->setSelected(true);
  layerOpacity->setValue(std::lround(d.opacity(d.current()) * 100));
  layerOpacityValue->setText(QString("%1 %").arg(layerOpacity->value()));
  canvas->update();
  if (sh.kindChanged)
    sh.kindChanged();
  if (sh.updateTitle)
    sh.updateTitle();
}

// Ctrl toggles a row, Shift selects from the current row; the clicked row is
// current.
void clickRow(Shell &sh, int i, Qt::KeyboardModifiers m)
{
  Canvas *canvas = sh.canvas;
  LayerList *layerList = sh.layerList;
  Document &d = canvas->doc();
  auto &sel = layerList->sel;
  if (!(m & (Qt::ControlModifier | Qt::ShiftModifier)))
    sel.clear();
  else if (m & Qt::ShiftModifier)
  {
    int a = -1, b = -1;
    for (int r = 0; r < layerList->count(); r++)
    {
      int k = layerList->item(r)->data(Qt::UserRole).toInt();
      if (k == d.current())
        a = r;
      if (k == i)
        b = r;
    }
    sel.clear();
    for (int r = std::min(a < 0 ? b : a, b); r <= std::max(a, b); r++)
      sel.insert(layerList->item(r)->data(Qt::UserRole).toInt());
  }
  else
  {
    if (sel.empty())
      sel.insert(d.current());
    if (!sel.erase(i))
      sel.insert(i);
  }
  d.setCurrent(i);
  layerList->selStep = d.stepId();
  sh.refresh();
}

// A row dropped, clicked or made current (#459, #475).
void wireLayerList(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  LayerList *layerList = sh.layerList;
  layerList->onDrop =
    [canvas, layerList](
      int from, int gap,
      int inside) { // nothing drops below the Background, or into its own block
      Document &d = canvas->doc();
      auto at = [&](int row)
      { return layerList->item(row)->data(Qt::UserRole).toInt(); };
      bool moved;
      auto items =
        layerList->sel.count(from)
          ? layerList->items(from)
          : std::vector<int>{
              from}; // dragging a selected row drags the whole selection (#446)
      if (inside >= 0)
        moved =
          !layerList->sel.count(at(inside)) && at(inside) != from &&
          d.moveItems(
            items, at(inside), d.depth(at(inside)) + 1); // top item of the group
      else
        moved =
          gap < layerList->count() &&
          d.moveItems(
            items, at(gap) + 1, d.depth(at(gap))); // above that row, at its level
      if (moved)
        QTimer::singleShot(0, [canvas] { canvas->onLayersChange(); });
    };
  layerList->onClick = [&sh](int i, Qt::KeyboardModifiers m)
  { clickRow(sh, i, m); };
  QObject::connect(
    layerList, &QListWidget::currentRowChanged,
    [canvas, layerList, &sh](int row)
    {
      if (row < 0)
        return;
      canvas->doc().setCurrent(layerList->item(row)->data(Qt::UserRole).toInt());
      sh.refresh();
    });
}

// Layer > Properties and a row's right-click (#470): edits preview live with no undo
// step; OK keeps them as one step, Cancel puts the layer back. The Background and
// groups have none yet.
void openProperties(Shell &sh, int i)
{
  Canvas *canvas = sh.canvas;
  Document &d = canvas->doc();
  if (i < 1 || d.isGroup(i))
    return;
  const QString name0 = d.name(i);
  const double opacity0 = d.opacity(i);
  const bool lock0 = d.alphaLock(i), inherit0 = d.inheritAlpha(i);
  QDialog dlg(&sh.window);
  dlg.setWindowTitle("Layer Properties");
  auto *form = new QFormLayout(&dlg);
  auto *nameEdit = new QLineEdit(name0);
  auto *opacity = new QSlider(Qt::Horizontal);
  opacity->setRange(0, 100);
  opacity->setValue(std::lround(opacity0 * 100));
  auto *lock = new QCheckBox("Alpha Lock");
  lock->setChecked(lock0);
  lock->setVisible(!d.vector(i));
  auto *inherit = new QCheckBox("Inherit Alpha");
  inherit->setChecked(inherit0);
  auto *buttons =
    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  form->addRow("Name", nameEdit);
  form->addRow("Opacity", opacity);
  form->addRow(lock);
  form->addRow(inherit);
  form->addRow(buttons);
  auto preview = [&]
  {
    d.previewLayer(
      i, nameEdit->text(), opacity->value() / 100.0, lock->isChecked(),
      inherit->isChecked());
    canvas->update();
  };
  QObject::connect(nameEdit, &QLineEdit::textChanged, preview);
  QObject::connect(opacity, &QSlider::valueChanged, preview);
  QObject::connect(lock, &QCheckBox::toggled, preview);
  QObject::connect(inherit, &QCheckBox::toggled, preview);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  const bool ok = dlg.exec() == QDialog::Accepted;
  const QString name1 = nameEdit->text();
  d.previewLayer(i, name0, opacity0, lock0, inherit0); // back, then the one step
  if (ok)
    d.setLayer(
      i, name1, opacity->value() / 100.0, lock->isChecked(), inherit->isChecked());
  sh.refresh();
}

void wireRowMenus(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  LayerList *layerList = sh.layerList;
  // double-click a group row (not its eye or arrow, which take the click) opens or
  // closes it; a filter, because refresh() rebuilds the rows on the first click
  auto *dbl = new DoubleClick;
  dbl->on = [canvas, layerList](QPoint pos)
  {
    auto *it = layerList->itemAt(pos);
    if (!it)
      return false;
    int i = it->data(Qt::UserRole).toInt();
    if (!canvas->doc().isGroup(i))
      return false;
    canvas->doc().setCollapsed(i, !canvas->doc().collapsed(i));
    canvas->onLayersChange();
    return true;
  };
  layerList->viewport()->installEventFilter(dbl);
  layerList->setContextMenuPolicy(
    Qt::CustomContextMenu); // right-click a row: Rasterize, enabled on a Vector
                            // Layer only (#200)
  QObject::connect(
    layerList, &QListWidget::customContextMenuRequested,
    [canvas, layerList, &sh](QPoint pos)
    {
      auto *it = layerList->itemAt(pos);
      if (!it)
        return;
      int i = it->data(Qt::UserRole).toInt();
      QMenu menu;
      auto *rasterize = menu.addAction("Rasterize");
      rasterize->setEnabled(canvas->doc().vector(i));
      auto *props = menu.addAction("Properties...");
      props->setEnabled(i > 0 && !canvas->doc().isGroup(i));
      auto *chosen = menu.exec(layerList->viewport()->mapToGlobal(pos));
      if (chosen == rasterize && canvas->doc().rasterizeLayer(i))
        sh.refresh();
      else if (chosen == props)
        openProperties(sh, i);
    });
}

} // namespace

void buildLayersDock(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  auto *layerList = sh.layerList = new LayerList;
  layerList->setAcceptDrops(true);
  layerList->viewport()->setAcceptDrops(true);
  layerList->setDropIndicatorShown(false);
  layerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  auto [layerOpacity, layerOpacityValue, layerOpacityRow] =
    sliderRow("", 0, 100, "%1 %"); // percent, of the current layer
  sh.refresh = [&sh, layerOpacity, layerOpacityValue]
  { refreshLayers(sh, layerOpacity, layerOpacityValue); };
  canvas->onLayersChange = sh.refresh;
  wireLayerList(sh);
  wireRowMenus(sh);
  QObject::connect(
    layerOpacity, &QSlider::valueChanged,
    [canvas](int v)
    {
      canvas->doc().setOpacity(canvas->doc().current(), v / 100.0);
      canvas->update();
    });
  auto *layerButtons = new QGridLayout;
  int layerOpIndex = 0;
  for (const auto &l : layerOps(layerList))
  {
    auto *btn = new QPushButton(l.button);
    btn->setMinimumWidth(1); // the style's 80px floor would keep the dock about
                             // 450px wide; 0 counts as unset and keeps it
    QObject::connect(
      btn, &QPushButton::clicked,
      [canvas, &sh, op = l.op]
      {
        op(canvas->doc());
        sh.refresh();
      });
    layerButtons->addWidget(
      btn, layerOpIndex / 3, layerOpIndex % 3); // three per row
    layerOpIndex++;
  }
  auto *layerPanel = new QWidget;
  auto *layerCol = new QVBoxLayout(layerPanel);
  layerCol->addWidget(layerList);
  layerCol->addLayout(layerButtons);
  layerCol->addWidget(new QLabel("Opacity"));
  layerCol->addLayout(layerOpacityRow);
  sh.layerDock = new QDockWidget("Layers", &sh.window);
  sh.layerDock->setWidget(layerPanel);
  sh.window.addDockWidget(Qt::RightDockWidgetArea, sh.layerDock);
  sh.refresh();
}

void buildLayerMenu(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  LayerList *layerList = sh.layerList;
  auto *layerMenu = sh.window.menuBar()->addMenu("&Layer");
  for (const auto &l : layerOps(layerList))
  {
    auto *a = layerMenu->addAction(l.menu);
    sh.docActions.push_back(a);
    QObject::connect(
      a, &QAction::triggered,
      [&sh, canvas, op = l.op]
      {
        op(canvas->doc());
        sh.refresh();
      });
  }
  QObject::connect(
    key(sh, layerMenu->addAction("&Group current item"), {QKeySequence("Ctrl+G")}),
    &QAction::triggered,
    [&sh, canvas, layerList]
    {
      canvas->doc().wrap(layerList->items(canvas->doc().current()));
      sh.refresh();
    });
  auto *props = layerMenu->addAction("&Properties...");
  sh.docActions.push_back(props);
  QObject::connect(
    props, &QAction::triggered,
    [&sh, canvas] { openProperties(sh, canvas->doc().current()); });
  sh.docActions.push_back(layerMenu->menuAction());
}
