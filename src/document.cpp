#include "document.h"

Document::Document(QSize size) : size_(size) {}

QImage Document::render() const {
    QImage img(size_, QImage::Format_RGBA8888);
    img.fill(Qt::white);
    return img;
}
