#pragma once
// The main window around the Canvas: what main() builds and the docks and menus
// share. Each build* adds one dock or menu; main() calls them in the order the
// first-launch layout and the menu bar expect (#172).
#include <functional>
#include <vector>
#include <QList>
#include <QMainWindow>
#include "canvas.h"

class ColorPicker;
class LayerList;
class Navigator;
class QAction;
class QActionGroup;
class QCheckBox;
class QDockWidget;
class QHBoxLayout;
class QKeySequence;
class QLabel;
class QSlider;
class QTabBar;

// The window saves its dock state in closeEvent: by aboutToQuit the floating docks
// are already hidden and would be saved closed.
struct Window : QMainWindow
{
  std::function<bool()>
    canClose; // false keeps the window open (the quit dialog was cancelled, #285)
  std::function<void()> onClose;
  void closeEvent(QCloseEvent *e) override;
};

struct Shell
{
  Window window;
  Canvas *canvas = nullptr;
  // Tab bar over the canvas (#241): the Canvas holds the active tab's Session,
  // tabs[] parks the rest; the active slot is empty.
  QTabBar *tabBar = nullptr;
  std::vector<Session> tabs;
  int active = -1;
  int untitledCount = 0; // N of the last "Untitled N", never reused (#282)
  std::vector<QAction *> docActions; // enabled only while a document is open
                                     // (#173); filled as the menus are built
  std::vector<QAction *>
    notOnGroup; // also disabled while a group is current: Cut and Copy (#501)
  std::function<void()> refresh; // rebuilds the Layers dock from the Document; runs
                                 // on every current-layer change
  std::function<void()> kindChanged; // set with the Tools dock; refresh() runs it
  std::function<void()> updateTitle; // set with the File menu; refresh() runs it
                                     // after every edit, undo and redo
  std::function<void(QString)>
    openPath;                          // Open and the command line: a tab per file
  std::function<void()> onEraseChange; // the Erase action switched mode: Tool
                                       // Options re-reads the mode's sliders
  QActionGroup *toolGroup = nullptr;
  LayerList *layerList = nullptr;
  ColorPicker *picker = nullptr;
  Navigator *nav = nullptr;
  QCheckBox *eraseBox = nullptr;
  QDockWidget *navDock = nullptr, *colorDock = nullptr, *brushDock = nullptr,
              *layerDock = nullptr, *optionsDock = nullptr, *toolDock = nullptr;
};

// A "Label [slider] readout" row (#621 row 6): the readout shows fmt ("%1 px", "%1
// %", "%1") of the value and follows the slider. An empty label leaves it out, for
// a caption above the row.
struct SliderRow
{
  QSlider *slider;
  QLabel *value;
  QHBoxLayout *row;
};
SliderRow sliderRow(const QString &label, int lo, int hi, const char *fmt);

// A window-wide action, so its keys work whichever dock has focus; it needs a
// document.
QAction *key(Shell &sh, QAction *a, QList<QKeySequence> keys);

void buildNavigationDock(Shell &sh);
void buildColorDock(Shell &sh);
void buildBrushesDock(Shell &sh);
void buildLayersDock(Shell &sh);
void buildToolOptionsDock(Shell &sh);
void buildToolsDock(Shell &sh);
void buildFileMenu(Shell &sh);
void buildEditMenu(Shell &sh);
void buildViewMenu(Shell &sh);
void buildLayerMenu(Shell &sh);
void buildToolsMenu(Shell &sh);
void buildHelpMenu(Shell &sh);
