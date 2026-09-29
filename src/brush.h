#pragma once
#include <libmypaint/mypaint-brush.h>
#include <memory>
#include <QString>

// A libmypaint brush loaded from a .myb file; null if the file is unreadable or not valid brush JSON.
std::shared_ptr<MyPaintBrush> loadBrush(const QString& mybPath);
