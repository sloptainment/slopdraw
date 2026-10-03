#include "document.h"
#include <algorithm>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include "powerstroke.h"
#include "spiropen.h"
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

// The curve of a path, whose nodes must give one (see spiroToPath). Every caller goes through here, so another kind plugs in here alone.
static QPainterPath curveOf(const Document::VPath& v) {
    if (v.kind == Document::PathKind::Spiro) return *spiroToPath(v.nodes, v.closed);
    // one cubic per node-to-node segment, so knot t is a segment index (#252)
    const std::vector<BezierNode>& b = v.bezier;
    QPainterPath path(b[0].p);
    for (size_t i = 1; i < b.size() + v.closed; i++) path.cubicTo(b[i - 1].out, b[i % b.size()].in, b[i % b.size()].p);
    return path;
}

// The cubics of curveOf(v). A QPainterPath from spiroToPath is lines and cubics (several per Spiro segment); lines get p1=p0, p2=p3.
static std::vector<Cubic> skeletonOf(const Document::VPath& v) {
    QPainterPath curve = curveOf(v);
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

QImage Document::render() const { return compose(0, int(layers_.size()), 0); }

QImage Document::groupImage(int group) const {
    int lo = group;
    while (lo > 0 && layers_[lo - 1].depth > layers_[group].depth) lo--;
    return compose(lo, group, layers_[group].depth + 1);
}

QImage Document::compose(int lo, int hi, int d) const {
    QImage out(layers_[0].image.size(), QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    // walk down the level's items, a group taking the deeper run below it
    std::vector<std::pair<int, int>> items;  // (item, first child)
    for (int i = hi - 1; i >= lo;) {
        int s = i;
        if (layers_[i].group) while (s > lo && layers_[s - 1].depth > d) s--;
        items.push_back({i, s});
        i = s - 1;
    }
    QPainter p(&out);
    for (size_t k = items.size(); k-- > 0;) {
        const Layer& l = layers_[items[k].first];
        if (!l.visible) continue;
        p.setOpacity(l.opacity);
        p.setCompositionMode(k + 1 < items.size() && l.inheritAlpha ? QPainter::CompositionMode_SourceAtop : QPainter::CompositionMode_SourceOver);
        p.drawImage(0, 0, l.group ? compose(items[k].second, items[k].first, d + 1) : l.image);
    }
    return out;
}

// ponytail: composites the whole canvas per call; fine per click, cache if it ever runs per mouse move
QColor Document::colorAt(QPoint p) const { return layers_[0].image.rect().contains(p) ? render().pixelColor(p) : QColor(); }

int Document::addLayer() {
    seal();
    Step s;
    s.kind = Add;
    s.layer = current_ + 1;
    s.to = layers_[current_].depth;
    s.na = "Layer " + QString::number(layers_.size());
    push(s);
    apply(s, false);
    return current_;
}

int Document::addGroup() {
    seal();
    Step s;
    s.kind = Tree;
    s.items = layers_;
    s.to = current_;
    Layer g;
    g.name = "Group " + QString::number(layers_.size());
    g.depth = layers_[current_].depth;
    g.group = true;
    s.newItems = layers_;
    s.newItems.insert(s.newItems.begin() + current_ + 1, g);
    s.layer = current_ + 1;
    push(s);
    apply(s, false);
    return current_;
}

int Document::blockStart(int i) const {
    int lo = i;
    if (layers_[i].group) while (lo > 0 && layers_[lo - 1].depth > layers_[i].depth) lo--;
    return lo;
}

bool Document::wellFormed(const std::vector<Layer>& v) {
    if (v.back().depth != 0) return false;
    for (size_t i = v.size() - 1; i > 0; i--)
        if (v[i - 1].depth < 0 || v[i - 1].depth > v[i].depth + v[i].group) return false;
    return true;
}

bool Document::relocate(int lo, int hi, int at, int depth) {
    std::vector<Layer> v = layers_;
    std::vector<int> ids(v.size());
    for (size_t i = 0; i < ids.size(); i++) ids[i] = int(i);
    std::vector<Layer> block(v.begin() + lo, v.begin() + hi + 1);
    std::vector<int> blockIds(ids.begin() + lo, ids.begin() + hi + 1);
    for (Layer& l : block) l.depth += depth - v[hi].depth;
    v.erase(v.begin() + lo, v.begin() + hi + 1);
    ids.erase(ids.begin() + lo, ids.begin() + hi + 1);
    v.insert(v.begin() + at, block.begin(), block.end());
    ids.insert(ids.begin() + at, blockIds.begin(), blockIds.end());
    if (at < 1 || depth < 0 || !wellFormed(v) || (at == lo && depth == layers_[hi].depth)) return false;
    seal();
    Step s;
    s.kind = Tree;
    s.items = layers_;
    s.newItems = v;
    s.to = current_;
    s.layer = int(std::find(ids.begin(), ids.end(), current_) - ids.begin());  // the current item keeps its identity
    push(s);
    apply(s, false);
    return true;
}

int Document::wrap(int layer) {
    if (layer < 1 || layer >= int(layers_.size())) return -1;
    seal();
    Step s;
    s.kind = Tree;
    s.items = layers_;
    s.to = current_;
    s.newItems = layers_;
    for (int i = blockStart(layer); i <= layer; i++) s.newItems[i].depth++;
    Layer g;
    g.name = "Group " + QString::number(layers_.size());
    g.depth = layers_[layer].depth;
    g.group = true;
    s.newItems.insert(s.newItems.begin() + layer + 1, g);
    s.layer = layer + 1;
    push(s);
    apply(s, false);
    return current_;
}

bool Document::stepItem(int layer, bool up) {
    int n = int(layers_.size());
    if (layer < 1 || layer >= n) return false;
    int lo = blockStart(layer), d = layers_[layer].depth, size = layer - lo + 1;
    // `at` is an index of the list without the block
    if (up) {
        int j = layer + 1;
        if (j >= n) return false;
        int dj = layers_[j].depth;
        if (dj < d) return relocate(lo, layer, j + 1 - size, d - 1);  // j is the group: out above it
        if (dj == d) return relocate(lo, layer, layers_[j].group && !layers_[j].collapsed ? j - size : j + 1 - size, d + (layers_[j].group && !layers_[j].collapsed));  // an empty group, or a layer
        int k = j;
        while (layers_[k].depth > d) k++;  // the group above whose block j ends
        return layers_[k].collapsed ? relocate(lo, layer, k + 1 - size, d) : relocate(lo, layer, lo, d + 1);
    }
    int j = lo - 1;
    int dj = layers_[j].depth;
    if (dj < d) return relocate(lo, layer, lo, d - 1);  // the bottom of its group: out below the group's block
    if (!layers_[j].group) return relocate(lo, layer, j, d);
    return layers_[j].collapsed ? relocate(lo, layer, blockStart(j), d) : relocate(lo, layer, j, d + 1);
}

bool Document::moveItem(int layer, int gap, int depth) {
    if (layer < 1 || layer >= int(layers_.size()) || gap < 1 || gap > int(layers_.size())) return false;
    int lo = blockStart(layer);
    if (gap > lo && gap <= layer) return false;
    return relocate(lo, layer, gap > layer ? gap - (layer - lo + 1) : gap, depth);
}

int Document::addVectorLayer() {
    seal();
    Step s;
    s.kind = Add;
    s.layer = current_ + 1;
    s.to = layers_[current_].depth;
    s.na = "Layer " + QString::number(layers_.size());
    s.vector = true;
    push(s);
    apply(s, false);
    return current_;
}

bool Document::addPath(const std::vector<SpiroNode>& nodes, bool closed, const QColor& color, double width, PaintMode mode, const QColor& fillColor) {
    if (!layers_[current_].vector || !spiroToPath(nodes, closed)) return false;
    seal();
    Step s;
    s.kind = PathAdd;
    s.layer = current_;
    std::vector<WidthKnot> knots{{0, width}};
    if (!closed) knots.push_back({double(skeletonOf({nodes, closed}).size()), width});
    s.paths = {{nodes, closed, color, width, knots}};
    s.paths[0].mode = mode;
    s.paths[0].fillColor = fillColor.isValid() ? fillColor : color;  // no fill color given: the fill takes the stroke color
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

// A Bezier path takes Spiro-style node positions (#310): each node moves to its new point and its handles move with it.
// False, changing nothing, unless there is one position per node.
static bool placeBezier(Document::VPath& v, const std::vector<SpiroNode>& nodes) {
    if (nodes.size() != v.bezier.size()) return false;
    for (size_t i = 0; i < nodes.size(); i++) {
        QPointF d = QPointF(nodes[i].x, nodes[i].y) - v.bezier[i].p;
        v.bezier[i].p += d, v.bezier[i].in += d, v.bezier[i].out += d;
    }
    return true;
}

bool Document::setNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed, const std::vector<WidthKnot>& knots) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size())) return false;
    VPath after = layers_[layer].paths[path];
    if (after.kind == PathKind::Bezier ? !placeBezier(after, nodes) : !spiroToPath(nodes, closed)) return false;
    seal();
    Step s;
    s.kind = Nodes;
    s.layer = layer;
    s.to = path;
    s.paths = {layers_[layer].paths[path], after};
    if (after.kind == PathKind::Spiro) s.paths[1].nodes = nodes;
    s.paths[1].closed = closed;
    if (!knots.empty()) s.paths[1].knots = knots;
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

void Document::addReference(const Reference& r) {
    auto refs = refs_;
    refs.push_back(r);
    setReferences(refs);
}

void Document::setReferences(const std::vector<Reference>& refs) {
    seal();
    Step s;
    s.kind = References;
    s.refs = refs_;
    s.newRefs = refs;
    push(s);
    apply(s, false);
}

int Document::pathAt(int layer, QPointF p, double tol) const {
    if (!layers_[layer].vector) return -1;
    for (int i = int(layers_[layer].paths.size()); i-- > 0;) {
        const VPath& v = layers_[layer].paths[i];
        QPainterPath curve = curveOf(v);
        QPainterPathStroker stroker;
        stroker.setWidth(2 * tol);
        if (stroker.createStroke(curve).contains(p) || outline(v).contains(p) || (v.mode != PaintMode::Stroke && curve.contains(p))) return i;
    }
    return -1;
}

std::vector<int> Document::pathsIn(int layer, const QRectF& box) const {
    std::vector<int> hit;
    if (!layers_[layer].vector) return hit;
    for (int i = 0; i < int(layers_[layer].paths.size()); i++) {
        const VPath& v = layers_[layer].paths[i];
        if (outline(v).intersects(box) || (v.mode != PaintMode::Stroke && curveOf(v).intersects(box))) hit.push_back(i);
    }
    return hit;
}

std::vector<SpiroNode> Document::nodes(int layer, int path) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return {};
    const VPath& v = layers_[layer].paths[path];
    if (v.kind == PathKind::Spiro) return v.nodes;
    std::vector<SpiroNode> ns;  // a Bezier path's points, for the Node drag (#310)
    for (const auto& b : v.bezier) ns.push_back({b.p.x(), b.p.y(), SpiroType::Corner});
    return ns;
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
    std::vector<Cubic> skeleton = skeletonOf(v);
    std::vector<QPointF> out;
    for (const WidthKnot& k : v.knots) out.push_back(skeletonAt(skeleton, k.t) + leftNormal(skeleton, k.t) * (k.width / 2));
    return out;
}

std::vector<WidthKnot> Document::knots(int layer, int path) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return {};
    return layers_[layer].paths[path].knots;
}

// ponytail: samples each cubic then ternary-searches around the best sample; a far-off pointer between two lobes may pick the other one
static double projectT(const std::vector<Cubic>& skeleton, QPointF p) {
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
    return (lo + hi) / 2;
}

std::optional<WidthKnot> Document::projectKnot(int layer, int path, QPointF p) const {
    if (layer < 0 || layer >= int(layers_.size()) || path < 0 || path >= int(layers_[layer].paths.size())) return std::nullopt;
    const VPath& v = layers_[layer].paths[path];
    std::vector<Cubic> skeleton = skeletonOf(v);
    double t = projectT(skeleton, p);
    QPointF d = p - skeletonAt(skeleton, t), nrm = leftNormal(skeleton, t);
    return WidthKnot{t, 2 * (d.x() * nrm.x() + d.y() * nrm.y())};
}

bool Document::addKnot(int layer, int path, QPointF p) {
    auto k = projectKnot(layer, path, p);
    if (!k || !layers_[layer].vector) return false;
    const VPath& v = layers_[layer].paths[path];
    auto knots = v.knots;
    k->width = widthAt(knots, k->t, int(skeletonOf(v).size()), v.closed);
    knots.insert(std::upper_bound(knots.begin(), knots.end(), *k, [](auto& a, auto& b) { return a.t < b.t; }), *k);
    return setKnots(layer, path, knots);
}

// A knot's t counts cubics, and Spiro splits a sharp segment into several, so the segment under p is found by where the skeleton passes
// through each node. Re-solving reshapes and recounts the cubics, so each knot is put back at the point it was on.
bool Document::addNode(int layer, int path, QPointF p, SpiroType type) {
    auto k = projectKnot(layer, path, p);
    if (!k) return false;
    const VPath& v = layers_[layer].paths[path];
    std::vector<Cubic> skeleton = skeletonOf(v);
    std::vector<int> at{0};  // at[j]: the cubic index where node j sits
    for (int c = 0; c < int(skeleton.size()) && at.size() < v.nodes.size(); c++)
        if (QLineF(skeleton[c][3], QPointF(v.nodes[at.size()].x, v.nodes[at.size()].y)).length() < 1e-6) at.push_back(c + 1);
    int s = std::min(int(std::upper_bound(at.begin(), at.end(), k->t) - at.begin()) - 1, int(v.nodes.size()) - (v.closed ? 1 : 2));
    QPointF pt = skeletonAt(skeleton, k->t);
    for (const SpiroNode& n : v.nodes)
        if (QLineF(pt, QPointF(n.x, n.y)).length() < 1e-3) return false;  // on a node: Spiro would take two coincident nodes
    auto nodes = v.nodes;
    nodes.insert(nodes.begin() + s + 1, {pt.x(), pt.y(), type});
    if (!spiroToPath(nodes, v.closed)) return false;
    std::vector<Cubic> fresh = skeletonOf({nodes, v.closed});
    auto knots = v.knots;
    for (WidthKnot& w : knots)
        w.t = w.t <= 0 ? 0 : w.t >= skeleton.size() ? double(fresh.size()) : projectT(fresh, skeletonAt(skeleton, w.t));
    return setNodes(layer, path, nodes, v.closed, knots);
}

void Document::previewKnots(int layer, int path, const std::vector<WidthKnot>& knots) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size())) return;
    layers_[layer].paths[path].knots = knots;
    rasterize(layers_[layer]);
}

void Document::previewNodes(int layer, int path, const std::vector<SpiroNode>& nodes, bool closed) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size())) return;
    VPath& v = layers_[layer].paths[path];
    if (v.kind == PathKind::Bezier ? !placeBezier(v, nodes) : !spiroToPath(nodes, closed)) return;
    if (v.kind == PathKind::Spiro) v.nodes = nodes;
    layers_[layer].paths[path].closed = closed;
    rasterize(layers_[layer]);
}

void Document::previewPath(int layer, int path, const VPath& v) {
    if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector || path < 0 || path >= int(layers_[layer].paths.size())) return;
    layers_[layer].paths[path] = v;
    rasterize(layers_[layer]);
}

void Document::rasterize(Layer& l) {
    l.image.fill(Qt::transparent);
    QPainter p(&l.image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    for (const VPath& v : l.paths) {
        if (v.mode != PaintMode::Stroke) {
            // QPainterPath fills an open subpath as if closed by a straight line (#258)
            QPainterPath inside = curveOf(v);
            inside.setFillRule(Qt::WindingFill);
            p.fillPath(inside, v.fillColor);
        }
        if (v.mode != PaintMode::Fill) p.fillPath(outline(v), v.color);
    }
}

QPainterPath Document::curve(const VPath& v) { return curveOf(v); }

Document::VPath Document::toBezier(const VPath& v) {
    std::vector<Cubic> sk = skeletonOf(v);
    size_t n = sk.size() + !v.closed;
    VPath b = v;
    b.kind = PathKind::Bezier;
    b.nodes.clear();
    b.bezier.clear();
    for (size_t i = 0; i < n; i++) {
        bool first = i == 0 && !v.closed, last = i == sk.size();
        const Cubic& next = sk[i % sk.size()];
        const Cubic& prev = sk[(i + sk.size() - 1) % sk.size()];
        QPointF p = last ? prev[3] : next[0];
        BezierNode node{p, last || !first ? prev[2] : p, first || !last ? next[1] : p, BezierType::Cusp};
        if (first) node.in = p;
        if (last) node.out = p;
        // a handle sitting on its point (a line) points along the segment instead
        QPointF a = p - (node.in == p ? prev[0] : node.in), c = (node.out == p ? next[3] : node.out) - p;
        if (!first && !last && std::abs(a.x() * c.y() - a.y() * c.x()) < 1e-9 * std::hypot(a.x(), a.y()) * std::hypot(c.x(), c.y()) && QPointF::dotProduct(a, c) > 0)
            node.type = BezierType::Smooth;
        b.bezier.push_back(node);
    }
    return b;
}

// Memoized: the Transform box and picked outlines ask on every paint and pointer move, while the paths rarely change.
QPainterPath Document::outline(const VPath& v) {
    struct Entry { VPath v; QPainterPath outline; };
    thread_local std::vector<Entry> cache;  // ponytail: last 32 shapes, linear scan; a keyed map if far more get picked at once
    auto same = [&](const VPath& c) { return c.kind == v.kind && c.closed == v.closed && c.nodes == v.nodes && c.bezier == v.bezier && c.knots == v.knots; };
    for (auto& e : cache) if (same(e.v)) return e.outline;
    if (cache.size() >= 32) cache.erase(cache.begin());
    cache.push_back({v, expandPowerStroke(skeletonOf(v), v.closed, v.knots)});
    return cache.back().outline;
}

void Document::restorePaths(int layer, const std::vector<VPath>& paths) {
    layers_[layer].vector = true;
    layers_[layer].paths = paths;
    rasterize(layers_[layer]);
    history_.clear();
    applied_ = 0;
}

void Document::restoreLayer(int layer, const QImage& image, const QString& name, bool visible, double opacity, bool alphaLock, bool inheritAlpha, int depth, bool group) {
    layers_[layer] = {visible, opacity, group ? QImage() : image.convertToFormat(QImage::Format_RGBA8888), name};
    layers_[layer].depth = depth;
    layers_[layer].group = group;
    layers_[layer].alphaLock = alphaLock;
    layers_[layer].inheritAlpha = inheritAlpha;
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

bool Document::setAlphaLock(int layer, bool on) {
    if (layers_[layer].vector || layers_[layer].group) return false;
    if (layers_[layer].alphaLock == on) return true;
    seal();
    Step s;
    s.kind = AlphaLock;
    s.layer = layer;
    s.a = !on;
    s.b = on;
    push(s);
    apply(s, false);
    return true;
}

void Document::setInheritAlpha(int layer, bool on) {
    if (layers_[layer].inheritAlpha == on) return;
    seal();
    Step s;
    s.kind = InheritAlpha;
    s.layer = layer;
    s.a = !on;
    s.b = on;
    push(s);
    apply(s, false);
}

bool Document::deleteLayer(int layer) {
    if (layer < 1 || layer >= int(layers_.size())) return false;
    seal();
    Step s;
    if (layers_[layer].group) {  // the block goes with it
        int lo = layer;
        while (layers_[lo - 1].depth > layers_[layer].depth) lo--;
        s.kind = Tree;
        s.items = layers_;
        s.to = current_;
        s.newItems = layers_;
        s.newItems.erase(s.newItems.begin() + lo, s.newItems.begin() + layer + 1);
        s.layer = lo - 1;
        push(s);
        apply(s, false);
        return true;
    }
    s.kind = Delete;
    s.layer = layer;
    s.before = layers_[layer].image;
    s.a = layers_[layer].opacity;
    s.b = layers_[layer].visible;
    s.na = layers_[layer].name;
    s.vector = layers_[layer].vector;
    s.paths = layers_[layer].paths;
    s.to = layers_[layer].alphaLock | layers_[layer].inheritAlpha << 1 | layers_[layer].depth << 2;
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
        if (reverse) {
            bool onIt = current_ == s.layer;
            remove();
            if (onIt) current_ = s.layer - 1;  // back to the item below it
            break;
        }
        insert({true, 1, QImage(layers_[0].image.size(), QImage::Format_RGBA8888), s.na});
        layers_[s.layer].depth = s.to;
        layers_[s.layer].image.fill(Qt::transparent);
        if (!s.after.isNull()) layers_[s.layer].image = s.after;  // a paste into a new layer (#504)
        layers_[s.layer].vector = s.vector;
        break;
    case Delete:
        if (reverse) insert({bool(s.b), s.a, s.before, s.na, s.vector, s.paths, bool(s.to & 1), bool(s.to & 2), s.to >> 2});
        else remove();
        break;
    case Move: reverse ? applyMove(s.to, s.layer) : applyMove(s.layer, s.to); break;
    case Tree:
        layers_ = reverse ? s.items : s.newItems;
        current_ = reverse ? s.to : s.layer;
        break;
    case Visible: layers_[s.layer].visible = reverse ? s.a : s.b; break;
    case Opacity: layers_[s.layer].opacity = reverse ? s.a : s.b; break;
    case AlphaLock: layers_[s.layer].alphaLock = reverse ? s.a : s.b; break;
    case InheritAlpha: layers_[s.layer].inheritAlpha = reverse ? s.a : s.b; break;
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
    case References: refs_ = reverse ? s.refs : s.newRefs; break;
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
    paintStroke(brush.get(), layers_[current_].image, samples, continuing, eraser, layers_[current_].alphaLock);
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
    if (layers_[current_].alphaLock) p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    else if (eraser) p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.fillPath(path, eraser ? QColor(255, 255, 255, color.alpha()) : color);
    p.end();
    seal();  // its own step, so a live stroke after it starts fresh
}

bool Document::bucketFill(const QPoint& at, const QColor& color, int threshold, int expand, bool antialiased, bool sampleMerged, bool eraser) {
    Layer& l = layers_[current_];
    if (l.vector || l.group || !l.image.rect().contains(at)) return false;
    seal();
    history_.resize(applied_);  // a new edit discards the redo branch
    strokeStart_ = l.image;
    const QImage src = (sampleMerged ? render() : l.image).convertToFormat(QImage::Format_ARGB32);  // render() skips hidden layers and references
    const QRgb target = src.pixel(at);
    auto diff = [&](int x, int y) {
        QRgb c = src.pixel(x, y);
        return std::max({std::abs(qRed(c) - qRed(target)), std::abs(qGreen(c) - qGreen(target)), std::abs(qBlue(c) - qBlue(target)), std::abs(qAlpha(c) - qAlpha(target))});
    };
    const int w = src.width(), h = src.height();
    auto at_ = [&](int x, int y) { return size_t(y) * w + x; };
    std::vector<float> cover(size_t(w) * h);  // 0 outside the region
    std::vector<bool> seen(cover.size());
    auto visit = [&](QPoint q) {
        seen[at_(q.x(), q.y())] = true;
        cover[at_(q.x(), q.y())] = antialiased ? 1.f - 0.5f * diff(q.x(), q.y()) / std::max(threshold, 1) : 1.f;  // ponytail: GIMP's ramp halved so halfway to Threshold is partial
    };
    std::vector<QPoint> todo = {at};
    visit(at);
    while (!todo.empty()) {
        QPoint p = todo.back();
        todo.pop_back();
        for (QPoint q : {p + QPoint(1, 0), p + QPoint(-1, 0), p + QPoint(0, 1), p + QPoint(0, -1)})
            if (src.rect().contains(q) && !seen[at_(q.x(), q.y())] && diff(q.x(), q.y()) <= threshold) {
                visit(q);
                todo.push_back(q);
            }
    }
    // one pixel per pass: dilate sets any neighbour of the region to full coverage, erode drops region pixels that touch outside it
    for (int i = 0; i < std::abs(expand); ++i) {
        std::vector<float> next = cover;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (QPoint d : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
                    int nx = x + d.x(), ny = y + d.y();
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;  // the document edge neither grows nor erodes
                    if (expand > 0 && cover[at_(nx, ny)] > 0) next[at_(x, y)] = std::max(next[at_(x, y)], 1.f);
                    if (expand < 0 && cover[at_(x, y)] > 0 && cover[at_(nx, ny)] == 0) next[at_(x, y)] = 0;
                }
        cover = next;
    }
    QImage mask(src.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QColor paint = eraser ? QColor(255, 255, 255, color.alpha()) : color;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (cover[at_(x, y)] > 0) {
                QColor c = paint;
                c.setAlphaF(paint.alphaF() * cover[at_(x, y)]);
                mask.setPixel(x, y, qPremultiply(c.rgba()));
            }
    QPainter p(&l.image);
    if (l.alphaLock) p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    else if (eraser) p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawImage(0, 0, mask);
    p.end();
    seal();  // its own step, so a live stroke after it starts fresh
    return true;
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

bool Document::floatGroup(int group, const QTransform& m, bool scaleWidths) {
    if (group < 1 || group >= int(layers_.size()) || !layers_[group].group || blockStart(group) == group) return false;
    if (groupStart_.empty()) {
        seal();
        history_.resize(applied_);  // a new edit discards the redo branch
        groupStart_ = layers_;
        pasting_ = true;  // not a step yet: stepId must not seal it
    }
    layers_ = groupStart_;
    for (int i = blockStart(group); i < group; i++) {
        Layer& l = layers_[i];
        if (l.group) continue;
        if (l.vector) {
            for (VPath& v : l.paths) v = transformNodes(v, m, scaleWidths);
            rasterize(l);
        } else {
            QImage out(l.image.size(), l.image.format());
            out.fill(Qt::transparent);
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setTransform(m);
            p.drawImage(0, 0, l.image);
            l.image = out;
        }
    }
    return true;
}

bool Document::transformGroup(int group, const QTransform& m, bool scaleWidths) {
    if (!floatGroup(group, m, scaleWidths)) return false;
    seal();
    return true;
}

QRectF Document::groupBounds(int group) const {
    QRectF box;
    if (group < 1 || group >= int(layers_.size()) || !layers_[group].group) return box;
    for (int i = blockStart(group); i < group; i++) {
        const Layer& l = layers_[i];
        if (l.group) continue;
        if (l.vector) {
            for (const VPath& v : l.paths) box |= outline(v).boundingRect();
            continue;
        }
        QRect r;
        for (int y = 0; y < l.image.height(); y++) {
            auto* p = reinterpret_cast<const QRgb*>(l.image.constScanLine(y));
            for (int x = 0; x < l.image.width(); x++)
                if (qAlpha(p[x])) r |= QRect(x, y, 1, 1);
        }
        box |= QRectF(r);
    }
    return box;
}

void Document::cancelFloat() {
    if (!groupStart_.empty()) {
        layers_ = groupStart_;
        groupStart_.clear();
        pasting_ = false;
        return;
    }
    if (strokeStart_.isNull()) return;
    layers_[current_].image = strokeStart_;
    strokeStart_ = QImage();
    pasting_ = false;
    if (floatFrom_ < 0) return;
    layers_.pop_back();
    current_ = floatFrom_;
    floatFrom_ = -1;
}

bool Document::floatImage(const QImage& img, const QTransform& m, bool newLayer) {
    if (!pasting_) {
        if (!newLayer && layers_[current_].vector) return false;
        seal();  // a stroke still in progress is its own step
        pasting_ = true;
        history_.resize(applied_);  // a new edit discards the redo branch
        if (newLayer) {
            floatFrom_ = current_;
            layers_.push_back({true, 1, QImage(layers_[0].image.size(), QImage::Format_RGBA8888), "Layer " + QString::number(layers_.size())});
            layers_.back().image.fill(Qt::transparent);
            current_ = int(layers_.size()) - 1;
        }
        strokeStart_ = layers_[current_].image;
    }
    layers_[current_].image = strokeStart_;
    QPainter p(&layers_[current_].image);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(m);
    p.drawImage(0, 0, img);
    return true;
}

bool Document::pasteImage(const QImage& img, const QTransform& m, bool newLayer) {
    if (!floatImage(img, m, newLayer)) return false;
    seal();
    return true;
}

void Document::seal() {
    if (!groupStart_.empty()) {
        Step s;
        s.kind = Tree;
        s.items = std::move(groupStart_);
        s.newItems = layers_;
        s.to = s.layer = current_;
        groupStart_.clear();
        pasting_ = false;
        push(s);
        return;
    }
    if (strokeStart_.isNull()) return;
    pasting_ = false;
    if (floatFrom_ >= 0) {  // a paste into a layer it added: the layer, pixels and all, is one Add step
        Step s;
        s.kind = Add;
        s.layer = current_;
        s.na = layers_[current_].name;
        s.after = layers_[current_].image;
        push(s);
        strokeStart_ = QImage();
        floatFrom_ = -1;
        return;
    }
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
    s.id = ++lastId_;
    history_.push_back(std::move(s));
    applied_++;
    auto bytes = [&] {
        qsizetype n = 0;
        for (const Step& s : history_) n += s.before.sizeInBytes() + s.after.sizeInBytes();
        return n;
    };
    while (history_.size() > 1 && bytes() > budget_) {
        baseId_ = history_.front().id;
        history_.erase(history_.begin());
        applied_--;
    }
}

int Document::stepId() {
    if (!pasting_) seal();  // a floating paste is not a step yet: the window title asks on every Layers refresh (#508)
    return applied_ ? history_[applied_ - 1].id : baseId_;
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
