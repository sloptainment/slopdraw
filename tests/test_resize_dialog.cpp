// Resize dialog's chain link (#418), driven through askNewSize on a real (Xvfb) display.
#include "newdialog.h"
#include <cstdio>
#include <cstdlib>
#include <QApplication>
#include <QDialog>
#include <optional>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void type(QSpinBox *s, const QString &text)
{
  s->setFocus();
  s->selectAll();
  for (QChar c : text)
  {
    QKeyEvent e(QEvent::KeyPress, Qt::Key_0 + c.digitValue(), Qt::NoModifier, QString(c));
    QApplication::sendEvent(s->findChild<QLineEdit *>(), &e);
  }
}

// Runs `body` once the modal dialog is up, then closes it.
template <class F> static std::optional<NewSize> withDialog(bool forEdit, F body, bool ok = false)
{
  QTimer::singleShot(0, [=] {
    QWidget *dlg = QApplication::activeModalWidget();
    body(dlg);
    ok ? static_cast<QDialog *>(dlg)->accept() : static_cast<QDialog *>(dlg)->reject();
  });
  QImage pic(400, 300, QImage::Format_RGBA8888);
  return askNewSize(nullptr, forEdit, QSize(400, 300), pic);
}

static void pickA4(QWidget *dlg)
{
  auto *list = dlg->findChild<QListWidget *>();
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole).toSize() == QSize(2480, 3508))
      list->setCurrentRow(i);
}

int main(int argc, char **argv)
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("slopdraw-test");
  QCoreApplication::setApplicationName("test_resize_dialog");
  QSettings().clear();

  withDialog(false, [](QWidget *dlg) { CHECK(!dlg->findChild<QToolButton *>("chain")); });
  withDialog(true, [](QWidget *dlg) { CHECK(dlg->findChild<QToolButton *>("chain")); });

  withDialog(true, [](QWidget *dlg) {
    auto *ws = dlg->findChild<QSpinBox *>(), *hs = dlg->findChildren<QSpinBox *>()[1];
    dlg->findChild<QToolButton *>("chain")->setChecked(true);
    type(ws, "800");
    CHECK(hs->value() == 600);
  });

  // linked state survives a restart: the previous dialog left it linked
  withDialog(true, [](QWidget *dlg) {
    CHECK(dlg->findChild<QToolButton *>("chain")->isChecked());
    auto *ws = dlg->findChild<QSpinBox *>(), *hs = dlg->findChildren<QSpinBox *>()[1];
    pickA4(dlg);
    CHECK(ws->value() == 2480 && hs->value() == 3508);
    type(ws, "1240");
    CHECK(hs->value() == 1754);
  });

  // Integer / Float-point radio box (#578): Integer on first launch, the last OK'd pick after, none in Resize
  withDialog(true, [](QWidget *dlg) { CHECK(!dlg->findChild<QRadioButton *>()); });
  withDialog(false, [](QWidget *dlg) {
    CHECK(dlg->findChild<QRadioButton *>("integer")->isChecked());
    CHECK(!dlg->findChild<QRadioButton *>("floatPoint")->isChecked());
    CHECK(dlg->findChild<QRadioButton *>("integer")->text() == "Integer");
    CHECK(dlg->findChild<QRadioButton *>("floatPoint")->text() == "Float-point");
    bool a = false, b = false;
    for (auto *l : dlg->findChildren<QLabel *>()) {
      a |= l->text() == "Faster, but may clip levels. Good for most usage";
      b |= l->text() == "Dynamic range artwork, but may be a little slower.";
    }
    CHECK(a && b);
  });
  auto ok = withDialog(false, [](QWidget *dlg) { dlg->findChild<QRadioButton *>("floatPoint")->setChecked(true); }, true);
  CHECK(ok && ok->floatPoint);
  withDialog(false, [](QWidget *dlg) { CHECK(dlg->findChild<QRadioButton *>("floatPoint")->isChecked()); });
  return failures;
}
