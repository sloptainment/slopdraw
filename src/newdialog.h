#pragma once
#include <optional>
#include <QPoint>
#include <QSize>

class QWidget;

struct NewSize
{
  QSize size;
  QPoint offset; // forEdit only: where the old picture sits in the new size
};

// File > New's dialog (#380): a preset list, width and height, orientation and a
// preview. The size OK'd, remembered for next time (#385); none if cancelled.
// forEdit is Edit > Resize's mode (#390): titled "Resize", opens on `current` with
// offset 0,0 (not on the remembered size), adds the Offset row (#651), and never
// reads or writes the remembered size.
std::optional<NewSize> askNewSize(QWidget *parent, bool forEdit, QSize current = {});
