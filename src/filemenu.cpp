// The File menu and the document tabs: New, Open, Save, Import Reference Image, Export, Close, Quit (#241, #277).
#include "shell.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QVBoxLayout>
#include <QActionGroup>
#include "newdialog.h"
#include "ora.h"

namespace {

// "name[*] - mydraw"; Qt shows the [*] as * while the window is modified (#278)
void updateTitle(Shell& sh) {
    Canvas* canvas = sh.canvas;
    if (!canvas->hasDoc()) { sh.window.setWindowTitle("mydraw"); sh.window.setWindowModified(false); return; }
    QString name = canvas->session().path.isEmpty() ? QString("Untitled %1").arg(canvas->session().untitled) : QFileInfo(canvas->session().path).fileName();
    bool dirty = canvas->doc().stepId() != canvas->session().saved;
    sh.window.setWindowTitle(name + "[*] - mydraw");
    sh.window.setWindowModified(dirty);
    sh.tabBar->setTabText(sh.active, dirty ? name + "*" : name);
}

// New and Open: a new tab, made active
void openTab(Shell& sh, Document doc, QString path) {
    if (sh.active >= 0) sh.tabs[sh.active] = sh.canvas->takeSession();
    sh.tabs.emplace_back();
    sh.active = int(sh.tabs.size()) - 1;
    { QSignalBlocker b(sh.tabBar); sh.tabBar->setCurrentIndex(sh.tabBar->addTab({})); }
    sh.canvas->setDocument(std::move(doc), path);
    if (path.isEmpty()) sh.canvas->session().untitled = ++sh.untitledCount;
    sh.updateTitle();
}

// A dirty tab asks Save / Discard / Cancel (#262); the last close returns to the no-document start (#157)
void closeTab(Shell& sh, int i) {
    Canvas* canvas = sh.canvas;
    sh.tabBar->setCurrentIndex(i);  // a background tab is shown before it is asked about
    if (canvas->doc().stepId() != canvas->session().saved) {
        auto b = QMessageBox::question(&sh.window, "Close", sh.tabBar->tabText(i).chopped(1) + " has unsaved changes.", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (b == QMessageBox::Cancel || (b == QMessageBox::Save && !sh.saveActive())) return;
    }
    bool wasActive = i == sh.active;
    if (wasActive) canvas->takeSession();  // ends a lifted piece or unfinished path; the session itself is dropped
    sh.tabs.erase(sh.tabs.begin() + i);
    { QSignalBlocker b(sh.tabBar); sh.tabBar->removeTab(i); }
    sh.active = sh.tabBar->currentIndex();
    if (sh.active < 0) canvas->setDocument(Document(QSize()));
    else if (wasActive) canvas->setSession(std::move(sh.tabs[sh.active]));  // the bar's new current tab moves into the Canvas
    sh.updateTitle();
}

bool saveTo(Shell& sh, QString path) {
    if (path.isEmpty()) return false;
    if (!path.endsWith(".ora")) path += ".ora";
    if (!writeOra(sh.canvas->doc(), path)) { QMessageBox::warning(&sh.window, "Save", "Could not write " + path); return false; }
    sh.canvas->session().path = path;
    sh.canvas->session().saved = sh.canvas->doc().stepId();
    sh.updateTitle();
    return true;
}

// Dirty tabs are listed as a checklist, all checked: the default button saves the checked and discards the rest (#324)
bool canClose(Shell& sh) {
    auto session = [&sh](int i) -> Session& { return i == sh.active ? sh.canvas->session() : sh.tabs[i]; };  // the active slot of tabs[] is empty
    std::vector<int> dirty;
    for (int i = 0; i < int(sh.tabs.size()); ++i) if (session(i).doc.stepId() != session(i).saved) dirty.push_back(i);
    if (dirty.empty()) return true;
    QDialog dlg(&sh.window);
    dlg.setWindowTitle("Quit");
    auto* col = new QVBoxLayout(&dlg);
    col->addWidget(new QLabel("Save changes before quitting?\nUnchecked documents are closed without saving."));
    auto* list = new QListWidget;
    for (int i : dirty) {
        QString path = session(i).path;
        auto* it = new QListWidgetItem(sh.tabBar->tabText(i).chopped(1) + "\n" + (path.isEmpty() ? "never saved: will ask where to save" : path), list);
        it->setCheckState(Qt::Checked);
    }
    col->addWidget(list);
    auto* btns = new QDialogButtonBox;
    auto* go = btns->addButton("Save All", QDialogButtonBox::AcceptRole);
    auto* discard = btns->addButton("Discard All", QDialogButtonBox::DestructiveRole);  // quits saving nothing, whatever is checked (#271 story 15)
    btns->addButton(QDialogButtonBox::Cancel);
    go->setDefault(true);
    bool discardAll = false;
    QObject::connect(discard, &QPushButton::clicked, &dlg, [&] { discardAll = true; dlg.accept(); });
    col->addWidget(btns);
    auto checked = [&] { std::vector<int> c; for (int r = 0; r < list->count(); ++r) if (list->item(r)->checkState() == Qt::Checked) c.push_back(dirty[r]); return c; };
    QObject::connect(list, &QListWidget::itemChanged, [&] {
        int n = int(checked().size()), m = int(dirty.size()) - n;
        go->setText(!n ? "Discard All" : m ? QString("Save %1, Discard %2").arg(n).arg(m) : "Save All");
    });
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return false;
    if (discardAll) return true;
    for (int i : checked()) {
        sh.tabBar->setCurrentIndex(i);  // the tab being saved is shown, and its Save path dialog asks over it
        if (!sh.saveActive()) return false;  // a cancelled path dialog stops the quit
    }
    return true;
}

}  // namespace

void buildFileMenu(Shell& sh) {
    Canvas* canvas = sh.canvas;
    auto* fileMenu = sh.window.menuBar()->addMenu("&File");
    sh.updateTitle = [&sh] { updateTitle(sh); };
    sh.openTab = [&sh](Document doc, QString path) { openTab(sh, std::move(doc), path); };
    QObject::connect(sh.tabBar, &QTabBar::currentChanged, [&sh](int i) {
        if (i < 0 || i == sh.active) return;
        sh.tabs[sh.active] = sh.canvas->takeSession();
        sh.active = i;
        sh.canvas->setSession(std::move(sh.tabs[i]));
        sh.updateTitle();
    });
    sh.tabBar->setTabsClosable(true);
    QObject::connect(sh.tabBar, &QTabBar::tabCloseRequested, [&sh](int i) { closeTab(sh, i); });
    auto* fileNew = fileMenu->addAction("&New...");
    fileNew->setShortcut(QKeySequence::New);
    QObject::connect(fileNew, &QAction::triggered, [&sh] { if (auto sz = askNewSize(&sh.window)) sh.openTab(Document(*sz), {}); });

    sh.openPath = [&sh](QString path) {  // Open and the command line: a tab per file
        auto doc = readOra(path);
        if (!doc) { QMessageBox::warning(&sh.window, "Open", "Could not read " + path); return; }
        sh.openTab(std::move(*doc), path);
    };
    auto* fileOpen = fileMenu->addAction("&Open...");
    fileOpen->setShortcut(QKeySequence::Open);
    QObject::connect(fileOpen, &QAction::triggered, [&sh] {
        QString path = QFileDialog::getOpenFileName(&sh.window, "Open", {}, "OpenRaster (*.ora)");
        if (!path.isEmpty()) sh.openPath(path);
    });
    auto* fileSave = fileMenu->addAction("&Save");
    sh.docActions.push_back(fileSave);
    fileSave->setShortcut(QKeySequence::Save);
    sh.saveActive = [&sh, canvas] { return saveTo(sh, !canvas->session().path.isEmpty() ? canvas->session().path : QFileDialog::getSaveFileName(&sh.window, "Save", {}, "OpenRaster (*.ora)")); };
    QObject::connect(fileSave, &QAction::triggered, [&sh] { sh.saveActive(); });
    auto* fileSaveAs = fileMenu->addAction("Save &As...");
    sh.docActions.push_back(fileSaveAs);
    fileSaveAs->setShortcut(QKeySequence("Ctrl+Shift+S"));
    QObject::connect(fileSaveAs, &QAction::triggered, [&sh, canvas] {
        saveTo(sh, QFileDialog::getSaveFileName(&sh.window, "Save As", canvas->session().path, "OpenRaster (*.ora)"));
    });
    auto* fileImportRef = fileMenu->addAction("Import &Reference Image...");
    sh.docActions.push_back(fileImportRef);
    QObject::connect(fileImportRef, &QAction::triggered, [&sh, canvas] {
        QString path = QFileDialog::getOpenFileName(&sh.window, "Import Reference Image", {}, "PNG (*.png)");
        if (path.isEmpty()) return;
        QImage img(path);
        if (img.isNull()) return void(QMessageBox::warning(&sh.window, "Import Reference Image", "Could not read " + path));
        sh.toolGroup->actions()[ReferenceTool]->trigger();
        canvas->importReference(img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    });
    auto* fileExport = fileMenu->addAction("&Export PNG...");
    sh.docActions.push_back(fileExport);
    QObject::connect(fileExport, &QAction::triggered, [&sh, canvas] {
        QString path = QFileDialog::getSaveFileName(&sh.window, "Export PNG", {}, "PNG (*.png)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".png")) path += ".png";
        if (!exportPng(canvas->doc(), path)) QMessageBox::warning(&sh.window, "Export PNG", "Could not write " + path);
    });

    auto* fileClose = fileMenu->addAction("&Close");
    sh.docActions.push_back(fileClose);
    fileClose->setShortcut(QKeySequence::Close);
    QObject::connect(fileClose, &QAction::triggered, [&sh] { closeTab(sh, sh.active); });
    auto* fileQuit = fileMenu->addAction("&Quit");
    fileQuit->setShortcut(QKeySequence::Quit);
    QObject::connect(fileQuit, &QAction::triggered, &sh.window, &QWidget::close);
    sh.window.canClose = [&sh] { return canClose(sh); };
}
