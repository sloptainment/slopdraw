#include "document.h"

Document::Document(QSize size) : layer_(size, QImage::Format_RGBA8888) {
    layer_.fill(Qt::white);
}

QImage Document::render() const { return layer_; }

void Document::stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples, bool continuing) {
    paintStroke(brush.get(), layer_, samples, continuing);
}
