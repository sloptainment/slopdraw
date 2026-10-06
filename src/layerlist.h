#pragma once
#include <functional>
#include <set>
#include <vector>
#include <QDragEnterEvent>
#include <QListWidget>

// Layers dock: the list runs top item first; each row holds its Document index in
// Qt::UserRole. refresh() rebuilds it from the Document. Drag a row (#459, #460):
// the pointer is over a gap between rows (a green bar) or the middle of a group row
// (a dashed outline, "inside"). Qt moves nothing: the drop is handed over as the
// dragged item's Document index, the gap row and the inside row (-1 for none);
// refresh() rebuilds the rows.
struct LayerList : QListWidget
{
  std::function<void(int, int, int)> onDrop;
  std::function<void(int, Qt::KeyboardModifiers)> onClick;
  std::set<int> sel; // the Document indexes selected with Ctrl/Shift (#475); empty
                     // means the current item alone. Dropped by refresh() once the
                     // Document has taken another step
  int selStep = 0;
  std::vector<int> items(int current) const
  {
    return sel.empty() ? std::vector<int>{current}
                       : std::vector<int>(sel.begin(), sel.end());
  }

protected:
  // A press that made the row current would rebuild the rows under the mouse and
  // lose the drag; so the row becomes current on release, and the drag starts here.
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void dragEnterEvent(QDragEnterEvent *e) override { e->accept(); }
  void dragMoveEvent(QDragMoveEvent *e) override;
  void dragLeaveEvent(QDragLeaveEvent *) override
  {
    gap_ = inside_ = -1;
    viewport()->update();
  }
  void dropEvent(QDropEvent *e) override;
  void paintEvent(QPaintEvent *e) override;

private:
  void aim(QPoint pos); // sets gap_ and inside_ for the pointer at pos
  int gap_ = -1, inside_ = -1, pressed_ = -1, from_ = -1;
  QPoint pressPos_;
};
