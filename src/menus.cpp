// The Edit, View and Tools menus; their keys are window-wide (#168).
#include "shell.h"
#include "newdialog.h"
#include <memory>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QWidgetAction>
#include <qapplication.h>
#include <qdialog.h>

QAction *key(Shell &sh, QAction *a, QList<QKeySequence> keys)
{
  a->setShortcuts(keys);
  sh.window.addAction(a);
  sh.docActions.push_back(a);
  return a;
}

void Window::closeEvent(QCloseEvent *e)
{
  if (canClose && !canClose())
  {
    e->ignore();
    return;
  }
  if (onClose)
    onClose();
  QMainWindow::closeEvent(e);
}

namespace
{

enum class PasteTo
{
  Cancel,
  Same,
  New,
  Reference
};

// Paste on a Raster Layer asks (#509, look: #511); a Vector Layer or group has no
// Same layer to choose, so it goes to a new Raster Layer without asking (#564,
// #625, #626).
PasteTo askPasteTo(Canvas *canvas, const QImage &img)
{
  Document &d = canvas->doc();
  bool raster = !d.vector(d.current()) && !d.isGroup(d.current());
  if (!raster)
    return PasteTo::New; // no Same layer to choose: a new Raster Layer, no menu
                         // (#626)
  QMenu menu;
  auto *head = new QWidgetAction(&menu);
  auto *box = new QWidget;
  auto *row = new QHBoxLayout(box);
  auto *thumb = new QLabel;
  thumb->setPixmap(QPixmap::fromImage(
    img.scaled(48, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
  row->addWidget(thumb);
  row->addWidget(
    new QLabel(QString("<b>Paste image</b><br>%1 × %2 px")
                 .arg(img.width())
                 .arg(img.height())),
    1);
  head->setDefaultWidget(box);
  menu.addAction(head);
  menu.addSeparator();
  auto *same =
    raster
      ? menu.addAction(QString("&Same layer: into “%1”").arg(d.name(d.current())))
      : nullptr;
  auto *fresh = menu.addAction("&New layer");
  auto *ref = menu.addAction("&Reference image");
  menu.addAction("Cancel");
  menu.setActiveAction(raster ? same : fresh); // Enter takes it
  auto *chosen = menu.exec(canvas->pasteMenuPoint());
  if (chosen == ref)
    return PasteTo::Reference;
  if (chosen && chosen == same)
    return PasteTo::Same;
  return chosen && chosen == fresh ? PasteTo::New : PasteTo::Cancel;
}

// Paste, Paste as Reference Image, and the tool Paste switched away from coming
// back.
void addPaste(Shell &sh, QMenu *editMenu)
{
  Canvas *canvas = sh.canvas;
  QActionGroup *toolGroup = sh.toolGroup;
  auto pasteReference =
    [canvas,
     toolGroup](const QImage &img) { // as an import is placed (#547, #524): the
                                     // view centre, scaled down to fit, picked
      toolGroup->actions()[ReferenceTool]->trigger();
      canvas->importReference(
        img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    };
  auto beforePaste =
    std::make_shared<QAction *>(nullptr); // the tool Paste switched away from;
                                          // Enter or a click outside brings it back
  QObject::connect(
    key(sh, editMenu->addAction("&Paste"), {QKeySequence("Ctrl+V")}),
    &QAction::triggered,
    [canvas, toolGroup, beforePaste, pasteReference]
    {
      QImage img = QGuiApplication::clipboard()->image();
      if (img.isNull())
        return; // text or nothing on the clipboard: no paste
      QPointF at = canvas->pastePoint();
      PasteTo to = askPasteTo(canvas, img);
      if (to == PasteTo::Reference)
        return pasteReference(img);
      if (to == PasteTo::Cancel)
        return; // Cancel, Esc or a click outside
      *beforePaste = toolGroup->checkedAction();
      toolGroup->actions()[SelectTool]->trigger();
      canvas->paste(
        img.convertToFormat(QImage::Format_RGBA8888), at, to == PasteTo::New);
    });
  QObject::connect(
    key(
      sh, editMenu->addAction("Paste as Re&ference Image"),
      {QKeySequence("Ctrl+Shift+V")}),
    &QAction::triggered,
    [pasteReference]
    {
      QImage img = QGuiApplication::clipboard()->image();
      if (!img.isNull())
        pasteReference(img); // any layer kind, no menu (#549)
    });
  canvas->onPasteEnd = [beforePaste]
  {
    if (*beforePaste)
      (*beforePaste)->trigger();
    *beforePaste = nullptr;
  };
}

} // namespace

void buildEditMenu(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  auto *editMenu = sh.window.menuBar()->addMenu("&Edit");
  QObject::connect(
    key(sh, editMenu->addAction("&Undo"), {QKeySequence("Ctrl+Z")}),
    &QAction::triggered, [canvas] { canvas->undo(false); });
  QObject::connect(
    key(sh, editMenu->addAction("&Redo"), {QKeySequence("Ctrl+Shift+Z")}),
    &QAction::triggered, [canvas] { canvas->undo(true); });
  editMenu->addSeparator();
  QObject::connect(
    key(sh, editMenu->addAction("Cu&t"), {QKeySequence("Ctrl+X")}),
    &QAction::triggered, [canvas] { canvas->copy(true); });
  QObject::connect(
    key(sh, editMenu->addAction("&Copy"), {QKeySequence("Ctrl+C")}),
    &QAction::triggered, [canvas] { canvas->copy(false); });
  sh.notOnGroup = {editMenu->actions().end()[-2], editMenu->actions().end()[-1]};
  addPaste(sh, editMenu);
  editMenu->addSeparator();
  QObject::connect(
    editMenu->addAction("Re&size..."), &QAction::triggered,
    [&sh, canvas]
    {
      auto r = askNewSize(
        &sh.window, true, canvas->doc().image(0).size(), canvas->doc().render());
      if (r)
        canvas->resize(r->size, r->offset, r->scale);
    });
  auto *crop = editMenu->addAction("Crop to Selecti&on");
  QObject::connect(crop, &QAction::triggered, [canvas] { canvas->cropToBox(); });
  QObject::connect(
    editMenu, &QMenu::aboutToShow,
    [canvas, crop] { crop->setEnabled(canvas->hasBox()); });
  sh.docActions.push_back(editMenu->menuAction());
}

void buildViewMenu(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  auto *viewMenu = sh.window.menuBar()->addMenu("&View");
  QObject::connect(
    key(sh, viewMenu->addAction("&Mirror"), {QKeySequence("M")}),
    &QAction::triggered, [canvas] { canvas->mirror(); });
  auto *showRefs =
    key(sh, viewMenu->addAction("Show &References"), {QKeySequence("Shift+R")});
  showRefs->setCheckable(true);
  showRefs->setChecked(true);
  QObject::connect(
    showRefs, &QAction::toggled,
    [canvas](bool on) { canvas->setShowReferences(on); });
  viewMenu->addSeparator();
  const QList<QDockWidget *> docks = {sh.toolDock,  sh.navDock,   sh.colorDock,
                                      sh.brushDock, sh.layerDock, sh.optionsDock};
  for (auto *dock : docks)
    viewMenu->addAction(dock->toggleViewAction());

  // Tab hides every visible dock and remembers them; the next Tab shows those again
  // (#52).
  auto tabHidden = std::make_shared<QList<QDockWidget *>>();
  auto tabDocks = [docks, tabHidden]
  {
    if (tabHidden->isEmpty())
    {
      for (auto *d : docks)
        if (d->isVisible())
        {
          *tabHidden << d;
          d->hide();
        }
    }
    else
    {
      for (auto *d : *tabHidden)
        d->show();
      tabHidden->clear();
    }
  };
  viewMenu->addSeparator();
  QObject::connect(
    key(sh, viewMenu->addAction("&Hide/show docks"), {QKeySequence("Tab")}),
    &QAction::triggered, tabDocks);

  // The dock arrangement is saved on quit and restored on launch; with nothing
  // saved the layout above stands (#56).
  for (auto *d : docks)
    d->setObjectName(
      d->windowTitle()); // QMainWindow::saveState needs each dock named
  sh.window.restoreState(QSettings().value("windowState").toByteArray());
  sh.window.onClose = [&sh, tabHidden, tabDocks]
  {
    if (!tabHidden->isEmpty())
      tabDocks(); // quit from a Tab-hidden state must not save every dock as closed
    QSettings().setValue("windowState", sh.window.saveState());
  };
}

void buildToolsMenu(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  QCheckBox *eraseBox = sh.eraseBox;
  auto *toolsMenu = sh.window.menuBar()->addMenu("&Tools");
  toolsMenu->addActions(sh.toolGroup->actions());
  toolsMenu->addSeparator();
  auto *erase = key(sh, toolsMenu->addAction("&Erase mode"), {QKeySequence("E")});
  erase->setCheckable(true);
  QObject::connect(
    erase, &QAction::toggled,
    [&sh, canvas, eraseBox, erase](bool on)
    {
      canvas->setErase(on);
      sh.onEraseChange();
      eraseBox->setChecked(canvas->erase());
      erase->setChecked(canvas->erase());
    });
  auto showOptionsFor = canvas->onToolChange;
  canvas->onToolChange = [=](int tool)
  {
    showOptionsFor(tool);
    erase->setChecked(canvas->erase());
  }; // the new tool's own Erase
  QObject::connect(
    eraseBox, &QCheckBox::toggled, erase,
    &QAction::setChecked); // each side's setChecked with the same value emits
                           // nothing, so they settle
  QObject::connect(
    key(
      sh, toolsMenu->addAction("Finish path &open"),
      {QKeySequence("Return"), QKeySequence("Enter")}),
    &QAction::triggered, [canvas] { canvas->finishPath(false); });
  QObject::connect(
    key(
      sh, toolsMenu->addAction("Finish path &closed"),
      {QKeySequence("Shift+Return"), QKeySequence("Shift+Enter")}),
    &QAction::triggered, [canvas] { canvas->finishPath(true); });
  sh.docActions.push_back(toolsMenu->menuAction());
}

void buildHelpMenu(Shell &sh)
{
  auto *helpMenu = sh.window.menuBar()->addMenu("&Help");
  QObject::connect(
    helpMenu->addAction("About"), &QAction::triggered,
    [&sh]()
    {
      QDialog dialog(&sh.window);
      dialog.setWindowTitle("About");

      auto *layout = new QVBoxLayout(&dialog);

      // 1. Text description
      auto *textLabel =
        new QLabel("slopdraw v0.1", &dialog);
      textLabel->setAlignment(Qt::AlignCenter);

      auto *imageLabel = new QLabel(&dialog);
      QPixmap pixmap(":/badge.png");
      imageLabel->setPixmap(
        pixmap.scaled(88, 31, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      imageLabel->setAlignment(Qt::AlignCenter);

      layout->addWidget(textLabel);
      layout->addWidget(imageLabel);
      dialog.exec();
    });
  QObject::connect(
    helpMenu->addAction("About Qt"), &QAction::triggered, &QApplication::aboutQt);
}