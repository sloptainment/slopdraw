#include "document.h"
#include <algorithm>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include "powerstroke.h"
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

// The cubics of the Spiro curve through nodes, which must give one (see spiroToPath). A QPainterPath from spiroToPath is lines and
// cubics (several per Spiro segment); lines get p1=p0, p2=p3.
std::vector<Cubic> skeletonOf(const std::vector<SpiroNode>& nodes, bool closed) {
    QPainterPath curve = *spiroToPath(nodes, closed);
    std::vector<Cubic> skeleton;
    QPointF at = curve.elementAt(0);
    for (int i = 1; i < curve.elementCount();) {
        if (curve.elementAt(i).type == QPainterPath::CurveToElement) {
            skeleton.push_back({at, curve.elementAt(i), curve.elementAt(i + 1), curve.elementAt(i + 2)});
            i += 3;
        } else {
            skeleton.push_back({at, at, curve.elementAt(i), curve.elementAt(i)});
            i++;
        }
        at = skeleton.back()[3];
    }
    return skeleton;
}

}  // namespace

Document::Document(QSize size, qsizetype undoBudget) : budget_(undoBudget) {
    layers_.push_back({true, 1, QImage(size, QImage::Format_RGBA8888), "Background"});
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
    seal();
    Step s;
    s.kind = Add;
    s.layer = int(layers_.size());
    s.na = "Layer " + QString::number(s.layer);
    push(s);
    apply(s, false);
    return current_;
}

int Document::addVectorLayer() {
    seal();
    Step s;
    s.kind = Add;
    s.layer = int(layers_.size());
    s.na = "Layer " + QString::number(s.layer);
    s.vector = true;
    push(s);
    apply(s, false);
    return current_;
}

bool Document::addPath(const std::vector<SpiroNode>& nodes, bool closed, const QColor& color, double width) {
    if (!layers_[current_].vector || !spiroToPath(nodes, closed)) return false;
    seal();
    Step s;
    s.kind = PathAdd;
    s.layer = current_;
    std::vector<WidthKnot> knots{{0, width}};
    if (!closed) knots.push_back({double(skeletonOf(nodes, closed).size()), width});
    s.paths = {{nodes, closed, color, width, knots}};
    push(s);
    apply(s, false);
    return true;
}

bool Document::setKnots(int layer, int path, const std::vector<WidthKnot>& knots) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size()) || knots.empty()) return false;
    seal();
    Step s;
    s.kind = Knots;
    s.layer = layer;
    s.to = path;
    s.ka = layers_[layer].paths[path].knots;
    s.kb = knots;
    push(s);
    apply(s, false);
    return true;
}

bool Document::setNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size()) || !spiroToPath(nodes, closed)) return false;
    seal();
    Step s;
    s.kind = Nodes;
    s.layer = layer;
    s.to = path;
    s.paths = {layers_[layer].paths[path], layers_[layer].paths[path]};
    s.paths[1].nodes = nodes;
    s.paths[1].closed = closed;
    push(s);
    apply(s, false);
    return true;
}

bool Document::rasterizeLayer(int layer) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector) return false;
    seal();
    Step s;
    s.kind = Rasterize;
    s.layer = layer;
    s.paths = layers_[layer].paths;
    push(s);
    apply(s, false);
    return true;
}

bool Document::setPaths(int layer, const std::vector<VPath>& paths) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector) return false;
    seal();
    Step s;
    s.kind = Paths;
    s.layer = layer;
    s.paths = layers_[layer].paths;
    s.newPaths = paths;
    push(s);
    apply(s, false);
    return true;
}

int Document::pathAt(int layer, QPointF p, double tol) const {
    if (!layers_[layer].vector) return -1;
    for (int i = int(layers_[layer].paths.size()); i-- > 0;) {
        const VPath& v = layers_[layer].paths[i];
        QPainterPath curve = *spiroToPath(v.nodes, v.closed);
        QPainterPathStroker stroker;
        stroker.setWidth(2 * tol);
        if (stroker.createStroke(curve).contains(p) || outline(v).contains(p) || (v.closed && curve.contains(p))) return i;
    }
    return -1;
}

std::vector<int> Document::pathsIn(int layer, const QRectF& box) const {
    std::vector<int> hit;
    if (!layers_[layer].vector) return hit;
    for (int i = 0; i < int(layers_[layer].paths.size()); i++) {
        const VPath& v = layers_[layer].paths[i];
        if (outline(v).intersects(box) || (v.closed && spiroToPath(v.nodes, v.closed)->intersects(box))) hit.push_back(i);
    }
    return hit;
}

std::vector<SpiroNode> Document::nodes(int layer, int path) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return {};
    return layers_[layer].paths[path].nodes;
}

int Document::nodeAt(int layer, int path, QPointF p, double tol) const {
    int best = -1;
    double bestD = tol;
    auto ns = nodes(layer, path);
    for (int i = 0; i < int(ns.size()); i++) {
        double d = QLineF(p, QPointF(ns[i].x, ns[i].y)).length();
        if (d <= bestD) best = i, bestD = d;
    }
    return best;
}

// The skeleton point at path time t.
static QPointF skeletonAt(const std::vector<Cubic>& skeleton, double t) {
    int i = std::clamp(int(t), 0, int(skeleton.size()) - 1);
    double u = t - i, w = 1 - u;
    const Cubic& c = skeleton[i];
    return c[0] * (w * w * w) + c[1] * (3 * w * w * u) + c[2] * (3 * w * u * u) + c[3] * (u * u * u);
}

// The unit left normal (-ty, tx) at path time t, from nearby points, since a line's end control points coincide with its ends.
static QPointF leftNormal(const std::vector<Cubic>& skeleton, double t) {
    double e = 1e-3;
    QPointF d = skeletonAt(skeleton, std::min<double>(t + e, skeleton.size())) - skeletonAt(skeleton, std::max(t - e, 0.0));
    return QPointF(-d.y(), d.x()) / std::hypot(d.x(), d.y());
}

std::vector<QPointF> Document::knotHandles(int layer, int path) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return {};
    const VPath& v = layers_[layer].paths[path];
    std::vector<Cubic> skeleton = skeletonOf(v.nodes, v.closed);
    std::vector<QPointF> out;
    for (const WidthKnot& k : v.knots) out.push_back(skeletonAt(skeleton, k.t) + leftNormal(skeleton, k.t) * (k.width / 2));
    return out;
}

std::vector<WidthKnot> Document::knots(int layer, int path) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return {};
    return layers_[layer].paths[path].knots;
}

// ponytail: samples each cubic then ternary-searches around the best sample; a far-off pointer between two lobes may pick the other one
std::optional<WidthKnot> Document::projectKnot(int layer, int path, QPointF p) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return std::nullopt;
    const VPath& v = layers_[layer].paths[path];
    std::vector<Cubic> skeleton = skeletonOf(v.nodes, v.closed);
    auto dist2 = [&](double t) { QPointF d = skeletonAt(skeleton, t) - p; return d.x() * d.x() + d.y() * d.y(); };
    const int steps = 32;
    double n = skeleton.size(), best = 0;
    for (int i = 1; i <= steps * int(n); i++)
        if (dist2(i / double(steps)) < dist2(best)) best = i / double(steps);
    double lo = std::max(best - 1.0 / steps, 0.0), hi = std::min(best + 1.0 / steps, n);
    for (int i = 0; i < 40; i++) {
        double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
        if (dist2(a) < dist2(b)) hi = b;
        else lo = a;
    }
    double t = (lo + hi) / 2;
    QPointF d = p - skeletonAt(skeleton, t), nrm = leftNormal(skeleton, t);
    return WidthKnot{t, 2 * (d.x() * nrm.x() + d.y() * nrm.y())};
}

bool Document::addKnot(int layer, int path, QPointF p) {
    auto k = projectKnot(layer, path, p);
    if (!k || !layers_[layer].vector) return false;
    const VPath& v = layers_[layer].paths[path];
    auto knots = v.knots;
    k->width = widthAt(knots, k->t, int(skeletonOf(v.nodes, v.closed).size()), v.closed);
    knots.insert(std::upper_bound(knots.begin(), knots.end(), *k, [](auto& a, auto& b) { return a.t < b.t; }), *k);
    return setKnots(layer, path, knots);
}

void Document::previewKnots(int layer, int path, const std::vector<WidthKnot>& knots) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size())) return;
    layers_[layer].paths[path].knots = knots;
    rasterize(layers_[layer]);
}

void Document::previewNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size()) || !spiroToPath(nodes, closed)) return;
    layers_[layer].paths[path].nodes = nodes;
    layers_[layer].paths[path].closed = closed;
    rasterize(layers_[layer]);
}

void Document::rasterize(Layer& l) {
    l.image.fill(Qt::transparent);
    QPainter p(&l.image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    for (const VPath& v : l.paths) {
        p.fillPath(outline(v), v.color);
    }
}

QPainterPath Document::outline(const VPath& v) {
    return expandPowerStroke(skeletonOf(v.nodes, v.closed), v.closed, v.knots);
}

void Document::restorePaths(int layer, const std::vector<VPath>& paths) {
    layers_[layer].vector = true;
    layers_[layer].paths = paths;
    rasterize(layers_[layer]);
    history_.clear();
    applied_ = 0;
}

void Document::restoreLayer(int layer, const QImage& image, const QString& name, bool visible, double opacity) {
    layers_[layer] = {visible, opacity, image.convertToFormat(QImage::Format_RGBA8888), name};
    history_.clear();
    applied_ = 0;
    strokeStart_ = QImage();
}

void Document::setCurrent(int layer) {
    seal();
    current_ = layer;
}

void Document::setVisible(int layer, bool visible) {
    if (layers_[layer].visible == visible) return;
    seal();
    Step s;
    s.kind = Visible;
    s.layer = layer;
    s.a = !visible;
    s.b = visible;
    push(s);
    apply(s, false);
}

void Document::setName(int layer, const QString& name) {
    if (layers_[layer].name == name) return;
    seal();
    Step s;
    s.kind = Name;
    s.layer = layer;
    s.na = layers_[layer].name;
    s.nb = name;
    push(s);
    apply(s, false);
}

void Document::setOpacity(int layer, double opacity) {
    if (layers_[layer].opacity == opacity) return;
    seal();
    Step s;
    s.kind = Opacity;
    s.layer = layer;
    s.a = layers_[layer].opacity;
    s.b = opacity;
    push(s);
    apply(s, false);
}

bool Document::deleteLayer(int layer) {
    if (layer < 1 || layer >= int(layers_.size())) return false;
    seal();
    Step s;
    s.kind = Delete;
    s.layer = layer;
    s.before = layers_[layer].image;
    s.a = layers_[layer].opacity;
    s.b = layers_[layer].visible;
    s.na = layers_[layer].name;
    s.vector = layers_[layer].vector;
    s.paths = layers_[layer].paths;
    push(s);
    apply(s, false);
    return true;
}

bool Document::moveLayer(int from, int to) {
    int n = int(layers_.size());
    if (from < 1 || to < 1 || from >= n || to >= n || from == to) return false;
    seal();
    Step s;
    s.kind = Move;
    s.layer = from;
    s.to = to;
    push(s);
    apply(s, false);
    return true;
}

void Document::applyMove(int from, int to) {
    Layer l = std::move(layers_[from]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, std::move(l));
    if (current_ == from) current_ = to;  // the current layer keeps its identity
    else if (from < current_ && current_ <= to) current_--;
    else if (to <= current_ && current_ < from) current_++;
}

void Document::apply(const Step& s, bool reverse) {
    auto insert = [&](Layer l) {
        layers_.insert(layers_.begin() + s.layer, std::move(l));
        current_ = s.layer;
    };
    auto remove = [&] {
        layers_.erase(layers_.begin() + s.layer);
        if (current_ > s.layer || current_ >= int(layers_.size())) current_--;
    };
    switch (s.kind) {
    case Pixels: paste(layers_[s.layer].image, reverse ? s.before : s.after, s.rect.topLeft()); break;
    case Add:
        if (reverse) { remove(); break; }
        insert({true, 1, QImage(layers_[0].image.size(), QImage::Format_RGBA8888), s.na});
        layers_[s.layer].image.fill(Qt::transparent);
        layers_[s.layer].vector = s.vector;
        break;
    case Delete:
        if (reverse) insert({bool(s.b), s.a, s.before, s.na, s.vector, s.paths});
        else remove();
        break;
    case Move: reverse ? applyMove(s.to, s.layer) : applyMove(s.layer, s.to); break;
    case Visible: layers_[s.layer].visible = reverse ? s.a : s.b; break;
    case Opacity: layers_[s.layer].opacity = reverse ? s.a : s.b; break;
    case Name: layers_[s.layer].name = reverse ? s.na : s.nb; break;
    case PathAdd:
        reverse ? layers_[s.layer].paths.pop_back() : layers_[s.layer].paths.push_back(s.paths[0]);
        rasterize(layers_[s.layer]);
        break;
    case Knots:
        layers_[s.layer].paths[s.to].knots = reverse ? s.ka : s.kb;
        rasterize(layers_[s.layer]);
        break;
    case Nodes:
        layers_[s.layer].paths[s.to] = s.paths[reverse ? 0 : 1];
        rasterize(layers_[s.layer]);
        break;
    case Paths:
        layers_[s.layer].paths = reverse ? s.paths : s.newPaths;
        rasterize(layers_[s.layer]);
        break;
    case Rasterize:  // the image already shows the paths, so only the kind and the paths change
        layers_[s.layer].vector = reverse;
        layers_[s.layer].paths = reverse ? s.paths : std::vector<VPath>{};
        break;
    }
}

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
    p.fillPath(path, eraser ? QColor(255, 255, 255, color.alpha()) : color);
    p.end();
    seal();  // its own step, so a live stroke after it starts fresh
}

bool Document::clearRect(const QRect& rect) {
    if (layers_[current_].vector) return false;
    seal();
    history_.resize(applied_);  // a new edit discards the redo branch
    strokeStart_ = layers_[current_].image;
    QPainter p(&layers_[current_].image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect, current_ == 0 ? QColor(Qt::white) : QColor(Qt::transparent));
    p.end();
    seal();  // its own step; a rect that changed nothing pushes none
    return true;
}

bool Document::floatRect(const QRect& rect, const QTransform& m) {
    if (layers_[current_].vector) return false;
    if (strokeStart_.isNull()) {
        seal();
        history_.resize(applied_);  // a new edit discards the redo branch
        strokeStart_ = layers_[current_].image;
    }
    QImage lifted = strokeStart_.copy(rect);
    layers_[current_].image = strokeStart_;
    QPainter p(&layers_[current_].image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect, current_ == 0 ? QColor(Qt::white) : QColor(Qt::transparent));
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(m);
    p.drawImage(rect.topLeft(), lifted);
    return true;
}

bool Document::transformRect(const QRect& rect, const QTransform& m) {
    if (!floatRect(rect, m)) return false;
    seal();
    return true;
}

void Document::cancelFloat() {
    if (strokeStart_.isNull()) return;
    layers_[current_].image = strokeStart_;
    strokeStart_ = QImage();
}

void Document::seal() {
    if (strokeStart_.isNull()) return;
    // ponytail: full-layer diff once per stroke; upgrade to dirty rects from the dab callback if it shows up in a profile
    QRect rect = diffRect(strokeStart_, layers_[current_].image);
    if (!rect.isNull()) {
        Step s;
        s.layer = current_;
        s.rect = rect;
        s.before = strokeStart_.copy(rect);
        s.after = layers_[current_].image.copy(rect);
        push(s);
    }
    strokeStart_ = QImage();
}

void Document::push(Step s) {
    history_.resize(applied_);  // a new edit discards the redo branch
    history_.push_back(std::move(s));
    applied_++;
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
    apply(history_[--applied_], true);
    return true;
}

bool Document::redo() {
    seal();
    if (applied_ == history_.size()) return false;
    apply(history_[applied_++], false);
    return true;
}
