#pragma once
#include <optional>
#include <QImage>
#include <QPoint>
#include <QSize>

class QWidget;

struct NewSize
{
  QSize size;
  QPoint offset; // forEdit only: where the old picture sits in the new size
  bool scale = false; // forEdit only: stretch the picture to the new size (#659)
  bool floatPoint = false; // New only: FP16 layers instead of RGBA64 (#450)
};

// File > New's dialog (#380): a preset list, width and height, orientation and a
// preview. The size OK'd, remembered for next time (#385); none if cancelled.
// forEdit is Edit > Resize's mode (#390): titled "Resize", opens on `current` with
// offset 0,0 (not on the remembered size), adds the Offset row (#651), and never
// reads or writes the remembered size. Its "Scale image" checkbox is remembered
// (#402); checked, the offset is 0,0 and greyed. `picture` is the document (#416): the preview
// shows it and dragging it sets the offset. New also has the Integer / Float-point
// radio box (#450), opening on the last pick OK'd (#452).
std::optional<NewSize> askNewSize(
  QWidget *parent, bool forEdit, QSize current = {}, const QImage &picture = {});
