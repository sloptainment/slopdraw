#include <QApplication>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabBar>
#include <QVBoxLayout>
#include "shell.h"

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  app.setOrganizationName(
    "mydraw"); // QSettings keys the saved window state on these
  app.setApplicationName("mydraw");

  Shell sh;
  Canvas *canvas = sh.canvas = new Canvas;
  sh.tabBar = new QTabBar;
  auto *central = new QWidget;
  auto *centralCol = new QVBoxLayout(central);
  centralCol->setContentsMargins(0, 0, 0, 0);
  centralCol->setSpacing(0);
  centralCol->addWidget(sh.tabBar);
  centralCol->addWidget(canvas);
  sh.window.setCentralWidget(central);
  canvas->onHint = [&sh](const QString &h)
  { sh.window.statusBar()->showMessage(h); };

  // The first-launch layout (#172): Tools above Brushes on the left; Navigation,
  // Color, Layers, Tool Options stacked on the right.
  buildNavigationDock(sh);
  buildColorDock(sh);
  buildBrushesDock(sh);
  buildLayersDock(sh);
  buildToolOptionsDock(sh);
  buildToolsDock(sh);

  buildFileMenu(sh);
  buildEditMenu(sh);
  buildViewMenu(sh);
  buildLayerMenu(sh);
  buildToolsMenu(sh);
  auto *helpMenu = sh.window.menuBar()->addMenu("&Help");
  QObject::connect(
    helpMenu->addAction("About Qt"), &QAction::triggered, &QApplication::aboutQt);

  sh.kindChanged(); // the menus exist now: the no-document state applies to them
                    // too
  sh.window.resize(1024, 768);
  sh.window.show();
  for (const QString &path : app.arguments().mid(1))
    sh.openPath(path); // mydraw a.ora b.ora: a tab each
  return app.exec();
}
