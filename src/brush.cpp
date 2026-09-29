#include "brush.h"
#include <QFile>

std::shared_ptr<MyPaintBrush> loadBrush(const QString& mybPath) {
    QFile f(mybPath);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    std::shared_ptr<MyPaintBrush> b(mypaint_brush_new(), mypaint_brush_unref);
    if (!mypaint_brush_from_string(b.get(), f.readAll().constData())) return nullptr;
    return b;
}
