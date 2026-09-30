#include "document.h"
#include <algorithm>
#include <QPainter>
#include <QPainterPath>
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

Document::Document(QSize size, qsizetype undoBudget) : budget_(undoBudget) {
    layers_.push_back({true, 1, QImage(size, QImage::Format_RGBA8888)});
    layers_[0].image.fill(Qt::white);
}

QImage Document::render() const {
    QImage out(layers_[0].image.size(), QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    QPainter p(&out);
    for (const Layer& l : layers_) {
        if (!l.visible) continue;
        p.setOpacity(l.opacity);
        p.drawImage(0, 0, l.image);
    }
    return out;
}

// ponytail: composites the whole canvas per call; fine per click, cache if it ever runs per mouse move
QColor Document::colorAt(QPoint p) const { return layers_[0].image.rect().contains(p) ? render().pixelColor(p) : QColor(); }

int Document::addLayer() {
    Layer l;
    l.image = QImage(layers_[0].image.size(), QImage::Format_RGBA8888);
    l.image.fill(Qt::transparent);
    seal();
    layers_.push_back(std::move(l));
    current_ = int(layers_.size()) - 1;
    return current_;
}

void Document::setCurrent(int layer) {
    seal();
    current_ = layer;
}

void Document::setVisible(int layer, bool visible) { layers_[layer].visible = visible; }

void Document::setOpacity(int layer, double opacity) { layers_[layer].opacity = opacity; }

void Document::stroke(const std::shared_ptr<MyPaintBrush>& brush, const std::vector<PenSample>& samples, bool continuing, bool eraser) {
    if (!continuing || strokeStart_.isNull()) {
        seal();
        history_.resize(applied_);  // a new edit discards the redo branch
        strokeStart_ = layers_[current_].image;
    }
    paintStroke(brush.get(), layers_[current_].image, samples, continuing, eraser);
}

void Document::fill(const QColor& color, const std::vector<QPointF>& points, bool eraser) {
    seal();
    history_.resize(applied_);  // a new edit discards the redo branch
    strokeStart_ = layers_[current_].image;
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addPolygon(QPolygonF(QVector<QPointF>(points.begin(), points.end())));
    path.closeSubpath();
    QPainter p(&layers_[current_].image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.fillPath(path, eraser ? Qt::white : color);
    p.end();
    seal();  // its own step, so a live stroke after it starts fresh
}

void Document::seal() {
    if (strokeStart_.isNull()) return;
    // ponytail: full-layer diff once per stroke; upgrade to dirty rects from the dab callback if it shows up in a profile
    QRect rect = diffRect(strokeStart_, layers_[current_].image);
    if (!rect.isNull()) {
        history_.push_back({current_, rect, strokeStart_.copy(rect), layers_[current_].image.copy(rect)});
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
    paste(layers_[s.layer].image, s.before,s.rect.topLeft());
    return true;
}

bool Document::redo() {
    seal();
    if (applied_ == history_.size()) return false;
    const Step& s = history_[applied_++];
    paste(layers_[s.layer].image, s.after,s.rect.topLeft());
    return true;
}
