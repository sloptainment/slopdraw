// The Tools dock (#154): one checkable action per tool, exclusive, its key a window-wide shortcut so it works whichever dock has focus.
#include "shell.h"
#include <memory>
#include <QActionGroup>
#include <QDockWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// What the layer-kind rule remembers between switches.
struct ToolMemory {
    int lastTool[2] = {BrushTool, SpiroPenTool};  // last tool used on a Raster, a Vector Layer (#161); Select fits both, so it is no one's last
    bool inGroup = false;
};

}  // namespace

void buildToolsDock(Shell& sh) {
    Canvas* canvas = sh.canvas;
    auto* toolPanel = new QWidget;
    auto* toolCol = new QVBoxLayout(toolPanel);
    auto* toolGroup = sh.toolGroup = new QActionGroup(&sh.window);
    const struct { const char* text; const char* key; } kTools[] = {
        {"\U0001F58C Brush", "B"}, // Brush (B)
        {"\u2B2E Fill", "F"}, // Lasso Fill (F)
        {"\u2712 Pen", "P"}, // Spiro Pen (P)
        {"\u2316 Node", "N"}, // Node/Knot edit (N)
        {"\u25AD Select", "S"}, // Select (S)
        {"\u2B1A Box", ""}, // Box Select
        {"\U0001F5BC Reference", "R"}, // Reference (R, #535)
        {"\U0001FAA3 Bucket Fill", "G"} // Bucket Fill (G, #607)
    };
    auto memory = std::make_shared<ToolMemory>();
    for (int i = 0; i < 8; i++) {
        auto* a = toolGroup->addAction(kTools[i].text);
        a->setCheckable(true);
        a->setChecked(i == 0);
        a->setShortcut(QKeySequence(kTools[i].key));
        sh.window.addAction(a);
        QObject::connect(a, &QAction::triggered, [canvas, i, memory] { canvas->setTool(i); if (i != SelectTool && i != ReferenceTool) memory->lastTool[toolFitsKind(true, i)] = i; });
        auto* b = new QToolButton;
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolCol->addWidget(b);
    }
    toolCol->addStretch();
    // Only the tools that fit the current layer's kind are enabled, so their keys do nothing on the other kind (#170); a switch that disables the tool picks the last one used on the new kind.
    sh.kindChanged = [&sh, canvas, toolGroup, memory] {
        bool has = canvas->hasDoc(), vec = canvas->doc().vector(canvas->doc().current()), grp = has && canvas->doc().isGroup(canvas->doc().current());
        canvas->setEnabled(has);  // a disabled widget gets no pointer or key events
        for (auto* w : {sh.colorDock->widget(), sh.layerDock->widget(), sh.optionsDock->widget(), sh.navDock->widget()}) w->setEnabled(has);
        for (auto* a : sh.docActions) a->setEnabled(has);
        for (auto* a : sh.notOnGroup) a->setEnabled(has && !grp);
        auto tools = toolGroup->actions();
        for (int i = 0; i < 8; i++) tools[i]->setEnabled(has && toolFitsKind(vec, i, grp));
        sh.brushDock->widget()->setEnabled(has && !vec);
        int now = toolForLayer(vec, tools.indexOf(toolGroup->checkedAction()), memory->lastTool[0], memory->lastTool[1], grp);
        if (memory->inGroup && !grp) now = memory->lastTool[vec];  // leaving a group restores the kind's last tool (#161)
        memory->inGroup = grp;
        if (!tools[now]->isChecked()) tools[now]->trigger();
    };
    sh.kindChanged();
    sh.toolDock = new QDockWidget("Tools", &sh.window);
    sh.toolDock->setWidget(toolPanel);
    sh.window.addDockWidget(Qt::LeftDockWidgetArea, sh.toolDock);
    sh.window.addDockWidget(Qt::LeftDockWidgetArea, sh.brushDock);  // layout B: Brushes below Tools, bottom-left
}
