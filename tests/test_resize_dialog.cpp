// Resize dialog's chain link (#418), driven through askNewSize on a real (Xvfb) display.
#include "newdialog.h"
#include <cstdio>
#include <cstdlib>
#include <QApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
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
template <class F> static void withDialog(bool forEdit, F body)
{
  QTimer::singleShot(0, [=] {
    QWidget *dlg = QApplication::activeModalWidget();
    body(dlg);
    static_cast<QDialog *>(dlg)->reject();
  });
  QImage pic(400, 300, QImage::Format_RGBA8888);
  askNewSize(nullptr, forEdit, QSize(400, 300), pic);
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
  QCoreApplication::setOrganizationName("mydraw-test");
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
  return failures;
}
