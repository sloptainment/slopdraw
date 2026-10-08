#pragma once
#include <optional>
#include <QSize>

class QWidget;

// File > New's dialog (#380): a preset list, width and height, orientation and a
// preview. The size OK'd, remembered for next time (#385); none if cancelled.
// forEdit is Edit > Resize's mode (#390); nothing differs yet.
std::optional<QSize> askNewSize(QWidget *parent, bool forEdit);
