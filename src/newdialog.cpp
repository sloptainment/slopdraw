#include "newdialog.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{

struct SizeBox : QSpinBox
{ // Enter on a bad size does nothing (#383); QSpinBox would restore the old value
  // and let OK fire
  void keyPressEvent(QKeyEvent *e) override
  {
    if (
      (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
      !hasAcceptableInput())
      return;
    QSpinBox::keyPressEvent(e);
  }
  QLineEdit *edit()
  {
    return lineEdit();
  } // its textChanged fires on typed edits; QSpinBox's only on valid text
};

struct Preset
{
  const char *group;
  const char *name;
  int w, h;
};
static const Preset presets[] = {
  // #384, #388; paper sizes at 300 ppi (/r/16)
  {"Screen", "720p", 1280, 720},
  {"Screen", "1080p", 1920, 1080},
  {"Screen", "1440p", 2560, 1440},
  {"Screen", "4K UHD", 3840, 2160},
  {"Paper, 300 ppi", "A5", 1748, 2480},
  {"Paper, 300 ppi", "A4", 2480, 3508},
  {"Paper, 300 ppi", "US Letter", 2550, 3300},
  {"Square", "Square 1024", 1024, 1024},
  {"Square", "Square 2048", 2048, 2048},
  {"Other", "1000 × 1500", 1000, 1500},
  {"Other", "Golden ratio", 1618, 1000},
};

// The presets under their group headers; each row holds its size in Qt::UserRole.
QListWidget *presetList()
{
  auto *list = new QListWidget;
  list->setFixedWidth(220);
  list->setMinimumHeight(
    18 * 15 + 4); // every row shows: 11 presets, 4 group headers
  QString group;
  for (auto &p : presets)
  {
    if (group != p.group)
      (new QListWidgetItem(group = p.group, list))
        ->setFlags(Qt::NoItemFlags); // a header row
    QString size = QString("%1 × %2").arg(p.w).arg(p.h);
    auto *it = new QListWidgetItem(
      size == p.name ? size : QString("%1   %2").arg(p.name, size));
    it->setData(Qt::UserRole, QSize(p.w, p.h));
    list->addItem(it);
  }
  return list;
}

// The preview box; forEdit drags the picture (#416): press, then the pointer's
// distance from the press.
struct Preview : QLabel
{
  QPoint start;
  std::function<void()> pressed;
  std::function<void(QPoint)> dragged;
  void mousePressEvent(QMouseEvent *e) override
  {
    start = e->pos();
    if (pressed)
      pressed();
  }
  void mouseMoveEvent(QMouseEvent *e) override
  {
    if (dragged)
      dragged(e->pos() - start);
  }
};

// Preview pixels per document pixel: the canvas and the picture together fit the
// box, whatever the offset, so a drag never rescales.
double previewScale(QSize sz, QSize old)
{
  return std::min(
    140.0 / std::max(sz.width(), old.width()),
    120.0 / std::max(sz.height(), old.height()));
}

// The dialog's widgets, made by makeFields and laid out by layOut.
struct Fields
{
  QListWidget *list = presetList();
  SizeBox *ws = new SizeBox, *hs = new SizeBox;
  QToolButton *swap = new QToolButton, *portrait = new QToolButton,
              *landscape = new QToolButton;
  Preview *preview = new Preview;
  QLabel *info = new QLabel;
  QDialogButtonBox *btns =
    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QSpinBox *ox = nullptr, *oy = nullptr; // forEdit only: the Offset row
  QPushButton *center = nullptr;
  QCheckBox *scale = nullptr; // forEdit only
  QImage pic; // forEdit only: the document's pixels, and its size
  QSize old;
};

Fields makeFields(bool forEdit)
{
  Fields f;
  if (forEdit)
  {
    f.ox = new QSpinBox;
    f.oy = new QSpinBox;
    f.center = new QPushButton("Center");
    f.scale = new QCheckBox("Scale image");
  }
  for (auto *s : {f.ws, f.hs})
  {
    s->setRange(1, 16384);
    s->setSuffix(" px");
  } // cap #383
  f.swap->setText("⇄");
  f.portrait->setText("Portrait");
  f.portrait->setCheckable(true);
  f.landscape->setText("Landscape");
  f.landscape->setCheckable(true);
  f.preview->setFixedSize(160, 140);
  f.preview->setAlignment(Qt::AlignCenter);
  return f;
}

void layOut(QDialog &dlg, const Fields &f, bool forEdit)
{
  auto *form = new QGridLayout;
  form->addWidget(new QLabel("Width"), 0, 0);
  form->addWidget(f.ws, 0, 1);
  form->addWidget(new QLabel("Height"), 1, 0);
  form->addWidget(f.hs, 1, 1);
  form->addWidget(f.swap, 1, 2);
  auto *orient = new QHBoxLayout;
  orient->addWidget(f.portrait);
  orient->addWidget(f.landscape);
  form->addWidget(new QLabel("Orientation"), 2, 0);
  form->addLayout(orient, 2, 1, 1, 2);
  int row = 3;
  if (forEdit)
  {
    form->addWidget(f.scale, row++, 0, 1, 3);
    auto *offset = new QHBoxLayout;
    offset->addWidget(new QLabel("X"));
    offset->addWidget(f.ox);
    offset->addWidget(new QLabel("Y"));
    offset->addWidget(f.oy);
    offset->addWidget(f.center);
    form->addWidget(new QLabel("Offset"), row, 0);
    form->addLayout(offset, row++, 1, 1, 2);
  }
  form->addWidget(f.preview, row, 0, 1, 3);
  form->addWidget(f.info, row + 1, 0, 1, 3);
  form->setRowStretch(row + 2, 1);
  auto *body = new QHBoxLayout;
  body->addWidget(f.list);
  body->addLayout(form);
  auto *col = new QVBoxLayout(&dlg);
  col->addLayout(body);
  col->addWidget(f.btns);
}

// Fields changed: OK, row highlight, orientation, preview and memory line follow.
void sync(const Fields &f)
{
  bool good = f.ws->hasAcceptableInput() && f.hs->hasAcceptableInput();
  f.btns->button(QDialogButtonBox::Ok)->setEnabled(good);
  QSize sz(f.ws->value(), f.hs->value());
  {
    QSignalBlocker b(f.list);
    f.list->setCurrentRow(-1);
    for (int i = 0; i < f.list->count(); ++i)
      if (good && f.list->item(i)->data(Qt::UserRole).toSize() == sz)
        f.list->setCurrentRow(i);
  }
  f.portrait->setChecked(good && sz.width() < sz.height());
  f.landscape->setChecked(good && sz.width() >= sz.height());
  QPixmap pm(f.preview->size());
  pm.fill(QColor(42, 42, 42));
  if (good && f.ox)
  { // the picture dimmed where it will be cropped, the new canvas outlined
    QPainter p(&pm);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    double k = previewScale(sz, f.old);
    QPointF u(
      80 - std::max(sz.width(), f.old.width()) * k / 2,
      70 - std::max(sz.height(), f.old.height()) * k / 2);
    QPointF c = u + QPointF(std::max(0, -f.ox->value()), std::max(0, -f.oy->value())) * k;
    QRectF canvas(c, QSizeF(sz) * k),
      picture(c + QPointF(f.ox->value(), f.oy->value()) * k, QSizeF(f.old) * k);
    if (f.scale->isChecked())
      picture = canvas; // stretched to fill the frame (#399)
    p.setOpacity(0.3);
    p.drawImage(picture, f.pic);
    p.setOpacity(1);
    p.setClipRect(canvas);
    p.drawImage(picture, f.pic);
    p.setClipping(false);
    p.setPen(Qt::white);
    p.drawRect(canvas.adjusted(0.5, 0.5, -0.5, -0.5));
  }
  else if (good)
  {
    QPainter p(&pm);
    QSize r = sz.scaled(140, 120, Qt::KeepAspectRatio).expandedTo({2, 2});
    p.fillRect(
      QRect(QPoint(80 - r.width() / 2, 70 - r.height() / 2), r), Qt::white);
  }
  f.preview->setPixmap(pm);
  f.info->setText(
    good ? QString("%1 × %2 px · Background layer %3 MB")
             .arg(sz.width())
             .arg(sz.height())
             .arg(sz.width() * double(sz.height()) * 4 / 1048576, 0, 'f', 1)
         : ""); // RGBA8888 (Document::Document, src/document.cpp)
}

// Swap, Portrait and Landscape flip width and height.
void wireOrientation(const Fields &f)
{
  auto flip = [=]
  {
    int w = f.ws->value();
    f.ws->setValue(f.hs->value());
    f.hs->setValue(w);
  };
  QObject::connect(f.swap, &QToolButton::clicked, flip);
  QObject::connect(
    f.portrait, &QToolButton::clicked,
    [=]
    {
      if (f.ws->value() >= f.hs->value())
        flip();
      sync(f);
    });
  QObject::connect(
    f.landscape, &QToolButton::clicked,
    [=]
    {
      if (f.ws->value() < f.hs->value())
        flip();
      sync(f);
    });
}

void wireFields(const Fields &f)
{
  for (auto *s : {f.ws, f.hs})
  { // typing reaches the lineEdit; setValue (a row, swap) only valueChanged
    QObject::connect(s->edit(), &QLineEdit::textChanged, [=] { sync(f); });
    QObject::connect(s, &QSpinBox::valueChanged, [=] { sync(f); });
  }
  QObject::connect(
    f.list, &QListWidget::currentRowChanged,
    [=](int row)
    {
      QSize sz = row < 0 ? QSize() : f.list->item(row)->data(Qt::UserRole).toSize();
      if (sz.isValid())
      {
        f.ws->setValue(sz.width());
        f.hs->setValue(sz.height());
      }
    });
  wireOrientation(f);
}

// Each offset axis runs between 0 and the size change (#393); Center sets half of it
// (#397).
void wireOffset(const Fields &f, QSize old)
{
  auto clamp = [=]
  {
    int dx = f.ws->value() - old.width(), dy = f.hs->value() - old.height();
    f.ox->setRange(std::min(0, dx), std::max(0, dx));
    f.oy->setRange(std::min(0, dy), std::max(0, dy));
  };
  for (auto *s : {f.ws, f.hs})
    QObject::connect(s, &QSpinBox::valueChanged, clamp);
  for (auto *s : {f.ox, f.oy})
    QObject::connect(s, &QSpinBox::valueChanged, [=] { sync(f); });
  auto base = std::make_shared<QPoint>(); // the offset at the press (#416)
  f.preview->setCursor(Qt::SizeAllCursor);
  f.preview->pressed = [=] { *base = QPoint(f.ox->value(), f.oy->value()); };
  f.preview->dragged = [=](QPoint d)
  { // the spin boxes clamp, as typing does
    if (f.scale->isChecked())
      return;
    double k = previewScale(QSize(f.ws->value(), f.hs->value()), old);
    f.ox->setValue(base->x() + qRound(d.x() / k));
    f.oy->setValue(base->y() + qRound(d.y() / k));
  };
  QObject::connect(
    f.center, &QPushButton::clicked,
    [=]
    {
      f.ox->setValue((f.ws->value() - old.width()) / 2);
      f.oy->setValue((f.hs->value() - old.height()) / 2);
    });
  QObject::connect(
    f.scale, &QCheckBox::toggled,
    [=](bool on)
    {
      if (on)
      {
        f.ox->setValue(0);
        f.oy->setValue(0);
      }
      for (QWidget *w : QList<QWidget *>{f.ox, f.oy, f.center})
        w->setEnabled(!on);
      f.preview->setCursor(on ? Qt::ArrowCursor : Qt::SizeAllCursor);
      sync(f);
      QSettings().setValue("scaleImage", on);
    });
  f.scale->setChecked(QSettings().value("scaleImage", false).toBool()); // remembered (#402)
  clamp();
}

} // namespace

std::optional<NewSize> askNewSize(
  QWidget *parent, bool forEdit, QSize current, const QImage &picture)
{ // take B of #380: preset list, size fields, orientation, preview
  QDialog dlg(parent);
  dlg.setWindowTitle(forEdit ? "Resize" : "New");
  Fields f = makeFields(forEdit);
  layOut(dlg, f, forEdit);
  f.pic = picture;
  f.old = current;
  wireFields(f);
  if (forEdit)
    wireOffset(f, current);
  QObject::connect(f.btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(f.btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QSize last =
    forEdit ? current
            : QSettings()
                .value("newSize", QSize(1920, 1080))
                .toSize(); // the last size OK'd (#385)
  f.ws->setValue(last.width());
  f.hs->setValue(last.height());
  sync(f);
  if (forEdit)
    f.ws->setFocus(); // else the list takes focus and selects its first preset
  if (dlg.exec() != QDialog::Accepted)
    return std::nullopt;
  QSize sz(f.ws->value(), f.hs->value());
  if (forEdit)
    return NewSize{sz, QPoint(f.ox->value(), f.oy->value()), f.scale->isChecked()};
  QSettings().setValue("newSize", sz);
  return NewSize{sz, {}};
}
