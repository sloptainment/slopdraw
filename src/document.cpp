#include "document.h"
#include <algorithm>
#include <cstring>

namespace {

// Smallest rect holding every pixel where a and b (same size, RGBA8888) differ; null if they are equal.
QRect diffRect(const QImage& a, const QImage& b) {
    int x0 = a.width(), x1 = -1, y0 = -1, y1 = -1;
    for (int y = 0; y < a.height(); y++) {
        auto* p = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        auto* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); x++) {
            if (p[x] == q[x]) continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            if (y0 < 0) y0 = y;
            y1 = y;
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

void paste(QImage& dst, const QImage& src, QPoint at) {
    for (int y = 0; y < src.height(); y++)
        std::memcpy(dst.scanLine(at.y() + y) + 4 * at.x(), src.constScanLine(y), 4 * size_t(src.width()));
}

}  // namespace

Document::Document(QSize size, qsizetype undoBudget) : layer_(size, QImage::Format_RGBA8888), budget_(undoBudget) {
    layer_.fill(Qt::white);
}

QImage Document::render() const { return layer_; }

// ponytail: one layer, so composite == layer_; composite the stack here when #44 lands
QColor Document::colorAt(QPoint p) const { return layer_.rect().contains(p) ? layer_.pixelColor(p) : QColor(); }

void Document::stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples, bool continuing, bool eraser) {
    if (!continuing || strokeStart_.isNull()) {
        seal();
        history_.resize(applied_);  // a new edit discards the redo branch
        strokeStart_ = layer_;
    }
    paintStroke(brush.get(), layer_, samples, continuing, eraser);
}

void Document::seal() {
    if (strokeStart_.isNull()) return;
    // ponytail: full-layer diff once per stroke; upgrade to dirty rects from the dab callback if it shows up in a profile
    QRect rect = diffRect(strokeStart_, layer_);
    if (!rect.isNull()) {
        history_.push_back({rect, strokeStart_.copy(rect), layer_.copy(rect)});
        applied_++;
    }
    strokeStart_ = QImage();
    auto bytes = [&] {
        qsizetype n = 0;
        for (const Step& s : history_) n += s.before.sizeInBytes() + s.after.sizeInBytes();
        return n;
    };
    while (history_.size() > 1 && bytes() > budget_) {
        history_.erase(history_.begin());
        applied_--;
    }
}

bool Document::undo() {
    seal();
    if (applied_ == 0) return false;
    const Step& s = history_[--applied_];
    paste(layer_, s.before, s.rect.topLeft());
    return true;
}

bool Document::redo() {
    seal();
    if (applied_ == history_.size()) return false;
    const Step& s = history_[applied_++];
    paste(layer_, s.after, s.rect.topLeft());
    return true;
}
