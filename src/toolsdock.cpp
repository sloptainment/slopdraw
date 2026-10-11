// The Tools dock (#154): one checkable action per tool, exclusive, its key a
// window-wide shortcut so it works whichever dock has focus.
#include "shell.h"
#include <memory>
#include <QActionGroup>
#include <QDockWidget>
#include <QGridLayout>
#include <QToolButton>

namespace
{

// What the layer-kind rule remembers between switches.
struct ToolMemory
{
  int lastTool[2] = {
    BrushTool, SpiroPenTool}; // last tool used on a Raster, a Vector Layer (#161);
                              // Select fits both, so it is no one's last
  bool inGroup = false;
};

// Only the tools that fit the current layer's kind are enabled, so their keys do
// nothing on the other kind (#170); a switch that disables the tool picks the
// last one used on the new kind.
void kindChanged(Shell &sh, ToolMemory &memory)
{
  Canvas *canvas = sh.canvas;
  QActionGroup *toolGroup = sh.toolGroup;
  bool has = canvas->hasDoc(), vec = canvas->doc().vector(canvas->doc().current()),
       grp = has && canvas->doc().isGroup(canvas->doc().current());
  canvas->setEnabled(has); // a disabled widget gets no pointer or key events
  for (auto *w :
       {sh.colorDock->widget(), sh.layerDock->widget(), sh.optionsDock->widget(),
        sh.navDock->widget()})
    w->setEnabled(has);
  for (auto *a : sh.docActions)
    a->setEnabled(has);
  for (auto *a : sh.notOnGroup)
    a->setEnabled(has && !grp);
  auto tools = toolGroup->actions();
  for (int i = 0; i < 8; i++)
    tools[i]->setEnabled(has && toolFitsKind(vec, i, grp));
  sh.brushDock->widget()->setEnabled(has && !vec);
  int now = toolForLayer(
    vec, tools.indexOf(toolGroup->checkedAction()), memory.lastTool[0],
    memory.lastTool[1], grp);
  if (memory.inGroup && !grp)
    now =
      memory.lastTool[vec]; // leaving a group restores the kind's last tool (#161)
  memory.inGroup = grp;
  if (!tools[now]->isChecked())
    tools[now]->trigger();
}

// The tool buttons in as many columns as the panel's width holds.
struct ToolPanel : QWidget
{
  QGridLayout *grid = new QGridLayout(this);
  std::vector<QToolButton *> buttons;
  int cols = 0;
  void flow(int n)
  {
    if (n == cols)
      return;
    cols = n;
    for (size_t i = 0; i < buttons.size(); i++)
    {
      grid->removeWidget(buttons[i]);
      grid->addWidget(buttons[i], i / n, i % n);
    }
    grid->setRowStretch(buttons.size(), 1); // pushes the rows to the top
  }
  void resizeEvent(QResizeEvent *e) override
  {
    QWidget::resizeEvent(e);
    int w = 1;
    for (auto *b : buttons)
      w = std::max(w, b->sizeHint().width());
    flow(std::max(1, width() / w));
  }
};

} // namespace

void buildToolsDock(Shell &sh)
{
  Canvas *canvas = sh.canvas;
  auto *toolPanel = new ToolPanel;
  auto *toolGroup = sh.toolGroup = new QActionGroup(&sh.window);
  const struct
  {
    const char *text;
    const char *key;
  } kTools[] = {
    {"\U0001F58C Brush", "B"},      // Brush (B)
    {"\u2B2E Fill", "F"},           // Lasso Fill (F)
    {"\u2712 Pen", "P"},            // Spiro Pen (P)
    {"\u2316 Node", "N"},           // Node/Knot edit (N)
    {"\u25AD Select", "S"},         // Select (S)
    {"\U0001F5BC Reference", "R"},  // Reference (R, #535)
    {"\U0001FAA3 Bucket Fill", "G"}, // Bucket Fill (G, #607)
    {"\u2702 Crop", "C"}            // Crop (C, #420)
  };
  auto memory = std::make_shared<ToolMemory>();
  for (int i = 0; i < 8; i++)
  {
    auto *a = toolGroup->addAction(kTools[i].text);
    a->setCheckable(true);
    a->setChecked(i == 0);
    a->setShortcut(QKeySequence(kTools[i].key));
    sh.window.addAction(a);
    QObject::connect(
      a, &QAction::triggered,
      [canvas, i, memory]
      {
        canvas->setTool(i);
        if (i != SelectTool && i != ReferenceTool && i != CropTool)
          memory->lastTool[toolFitsKind(true, i)] = i;
      });
    auto *b = new QToolButton;
    b->setDefaultAction(a);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolPanel->buttons.push_back(b);
  }
  toolPanel->flow(1);
  sh.kindChanged = [&sh, memory] { kindChanged(sh, *memory); };
  sh.kindChanged();
  sh.toolDock = new QDockWidget("Tools", &sh.window);
  sh.toolDock->setWidget(toolPanel);
  sh.window.addDockWidget(Qt::LeftDockWidgetArea, sh.toolDock);
  sh.window.addDockWidget(
    Qt::LeftDockWidgetArea,
    sh.brushDock); // layout B: Brushes below Tools, bottom-left
}
