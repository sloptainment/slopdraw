#include "newdialog.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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

// The dialog's widgets, made by makeFields and laid out by layOut.
struct Fields
{
  QListWidget *list = presetList();
  SizeBox *ws = new SizeBox, *hs = new SizeBox;
  QToolButton *swap = new QToolButton, *portrait = new QToolButton,
              *landscape = new QToolButton;
  QLabel *preview = new QLabel, *info = new QLabel;
  QDialogButtonBox *btns =
    new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
};

Fields makeFields()
{
  Fields f;
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

void layOut(QDialog &dlg, const Fields &f)
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
  form->addWidget(f.preview, 3, 0, 1, 3);
  form->addWidget(f.info, 4, 0, 1, 3);
  form->setRowStretch(5, 1);
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
  if (good)
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

} // namespace

std::optional<QSize> askNewSize(QWidget *parent)
{ // take B of #380: preset list, size fields, orientation, preview
  QDialog dlg(parent);
  dlg.setWindowTitle("New");
  Fields f = makeFields();
  layOut(dlg, f);
  wireFields(f);
  QObject::connect(f.btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(f.btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QSize last = QSettings()
                 .value("newSize", QSize(1920, 1080))
                 .toSize(); // the last size OK'd (#385)
  f.ws->setValue(last.width());
  f.hs->setValue(last.height());
  sync(f);
  if (dlg.exec() != QDialog::Accepted)
    return std::nullopt;
  QSize sz(f.ws->value(), f.hs->value());
  QSettings().setValue("newSize", sz);
  return sz;
}
