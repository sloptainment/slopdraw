#include "references.h"
#include <algorithm>
#include <cmath>
#include <map>

// The original, then smooth halvings down to one pixel, cached by the original's cacheKey.
// ponytail: never evicted; drop entries for keys no longer in any list if memory matters.
static const std::vector<QImage>& pyramid(const QImage& image) {
    static std::map<qint64, std::vector<QImage>> cache;
    auto& levels = cache[image.cacheKey()];
    if (levels.empty()) {
        levels.push_back(image);
        while (levels.back().width() > 1 || levels.back().height() > 1) {
            const QImage& l = levels.back();
            levels.push_back(l.scaled(std::max(1, l.width() / 2), std::max(1, l.height() / 2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
    }
    return levels;
}

void drawReferences(QPainter& p, const std::vector<Reference>& refs, const QTransform& view) {
    p.save();
    for (const Reference& r : refs) {
        if (r.image.isNull()) continue;
        QTransform m = r.transform * view;
        double scale = std::sqrt(std::abs(m.determinant()));  // screen px per image px
        const auto& levels = pyramid(r.image);
        size_t k = 0;
        while (k + 1 < levels.size() && double(levels[k + 1].width()) / r.image.width() >= scale) k++;
        p.setTransform(m);
        p.setClipRect(r.crop);
        p.setOpacity(r.opacity);
        p.setRenderHint(QPainter::SmoothPixmapTransform, scale < 1);
        p.drawImage(QRectF(r.image.rect()), levels[k]);
    }
    p.restore();
}

QTransform placeReference(QSize image, QSize viewSize, const QTransform& view) {
    double zoom = std::sqrt(std::abs(view.determinant()));
    double k = std::min({1.0, viewSize.width() / (image.width() * zoom), viewSize.height() / (image.height() * zoom)});
    QPointF c = view.inverted().map(QRectF(QPointF(), viewSize).center());
    return QTransform::fromTranslate(-image.width() / 2.0, -image.height() / 2.0) * QTransform::fromScale(k, k) * QTransform::fromTranslate(c.x(), c.y());
}

static QPolygonF area(const Reference& r) { return r.transform.map(QPolygonF(r.crop)); }

int referenceAt(const std::vector<Reference>& refs, QPointF p) {
    for (int i = int(refs.size()) - 1; i >= 0; i--)
        if (area(refs[i]).containsPoint(p, Qt::OddEvenFill)) return i;
    return -1;
}

QColor pickColorAt(const Document& doc, QPointF p, bool showReferences) {
    int i = showReferences ? referenceAt(doc.references(), p) : -1;
    if (i < 0) return doc.colorAt({int(std::floor(p.x())), int(std::floor(p.y()))});  // the pixel under the point, not the nearest corner
    const Reference& r = doc.references()[i];
    QPointF q = r.transform.inverted().map(p);
    QColor c = r.image.pixelColor(std::clamp(int(std::floor(q.x())), 0, r.image.width() - 1), std::clamp(int(std::floor(q.y())), 0, r.image.height() - 1));
    c.setAlpha(255);
    return c;
}

std::set<int> referencesIn(const std::vector<Reference>& refs, const QRectF& box) {
    std::set<int> in;
    for (int i = 0; i < int(refs.size()); i++)
        if (area(refs[i]).intersects(QPolygonF(box))) in.insert(i);
    return in;
}

QRectF referencesBounds(const std::vector<Reference>& refs, const std::set<int>& picks) {
    QRectF b;
    for (int i : picks)
        if (i >= 0 && i < int(refs.size())) b |= area(refs[i]).boundingRect();
    return b;
}

std::vector<Reference> transformReferences(std::vector<Reference> refs, const std::set<int>& picks, const QTransform& m) {
    for (int i : picks)
        if (i >= 0 && i < int(refs.size())) refs[i].transform *= m;
    return refs;
}

std::optional<QPoint> cropEdgeAt(const QRectF& crop, QPointF p, double tol) {
    bool inX = p.x() > crop.left() - tol && p.x() < crop.right() + tol, inY = p.y() > crop.top() - tol && p.y() < crop.bottom() + tol;
    int hx = inY && std::abs(p.x() - crop.left()) < tol ? -1 : inY && std::abs(p.x() - crop.right()) < tol ? 1 : 0;
    int hy = inX && std::abs(p.y() - crop.top()) < tol ? -1 : inX && std::abs(p.y() - crop.bottom()) < tol ? 1 : 0;
    if (!hx && !hy) return std::nullopt;
    return QPoint(hx, hy);
}

QRectF cropTo(QRectF crop, QSize image, QPoint h, QPointF p) {
    if (h.x() < 0) crop.setLeft(std::clamp(p.x(), 0.0, crop.right() - 1));
    if (h.x() > 0) crop.setRight(std::clamp(p.x(), crop.left() + 1, double(image.width())));
    if (h.y() < 0) crop.setTop(std::clamp(p.y(), 0.0, crop.bottom() - 1));
    if (h.y() > 0) crop.setBottom(std::clamp(p.y(), crop.top() + 1, double(image.height())));
    return crop;
}
