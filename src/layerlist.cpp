#include "layerlist.h"
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>

void LayerList::mousePressEvent(QMouseEvent *e)
{
  auto *it = itemAt(e->pos());
  pressed_ =
    it && e->button() == Qt::LeftButton ? it->data(Qt::UserRole).toInt() : -1;
  pressPos_ = e->pos();
}

void LayerList::mouseMoveEvent(QMouseEvent *e)
{
  if (
    pressed_ < 0 || !(e->buttons() & Qt::LeftButton) ||
    (e->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance())
    return;
  from_ = pressed_;
  pressed_ = -1;
  auto *drag = new QDrag(this);
  drag->setMimeData(new QMimeData);
  drag->exec(Qt::MoveAction);
}

void LayerList::mouseReleaseEvent(QMouseEvent *e)
{
  if (pressed_ >= 0 && itemAt(e->pos()))
    onClick(itemAt(e->pos())->data(Qt::UserRole).toInt(), e->modifiers());
  pressed_ = -1;
}

void LayerList::aim(QPoint pos)
{
  auto *it = itemAt(pos);
  gap_ = it ? row(it) : count();
  inside_ = -1;
  if (!it)
    return;
  QRect r = visualItemRect(it);
  if (
    it->data(Qt::UserRole + 1).toBool() && pos.y() > r.top() + r.height() / 4 &&
    pos.y() < r.bottom() - r.height() / 4)
    inside_ = gap_;
  else
    gap_ += pos.y() > r.center().y();
}

void LayerList::dragMoveEvent(QDragMoveEvent *e)
{
  aim(e->position().toPoint());
  e->accept();
  viewport()->update();
}

void LayerList::dropEvent(QDropEvent *e)
{
  aim(e->position().toPoint());
  e->setDropAction(Qt::IgnoreAction);
  e->accept();
  int g = gap_, in = inside_;
  gap_ = inside_ = -1;
  onDrop(from_, g, in);
}

void LayerList::paintEvent(QPaintEvent *e)
{
  QListWidget::paintEvent(e);
  if (gap_ < 0 && inside_ < 0)
    return;
  QPainter p(viewport());
  if (inside_ >= 0)
  {
    p.setPen(QPen(QColor("#7fd27f"), 2, Qt::DashLine));
    p.drawRect(visualItemRect(item(inside_)).adjusted(1, 1, -1, -1));
  }
  else if (count())
  {
    QRect r = visualItemRect(item(std::min(gap_, count() - 1)));
    p.fillRect(
      0, (gap_ < count() ? r.top() : r.bottom()) - 1, viewport()->width(), 3,
      QColor("#7fd27f"));
  }
}
