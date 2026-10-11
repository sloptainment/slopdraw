// Vector Layers: paths, nodes, Width Knots, and drawing them (#119).
#include "document.h"
#include <algorithm>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include "powerstroke.h"

namespace
{

// The curve of a path, whose nodes must give one (see spiroToPath). Every caller
// goes through here, so another kind plugs in here alone.
static QPainterPath curveOf(const Document::VPath &v)
{
  if (v.kind == Document::PathKind::Spiro)
    return *spiroToPath(v.nodes, v.closed);
  // one cubic per node-to-node segment, so knot t is a segment index (#252)
  const std::vector<BezierNode> &b = v.bezier;
  QPainterPath path(b[0].p);
  for (size_t i = 1; i < b.size() + v.closed; i++)
    path.cubicTo(b[i - 1].out, b[i % b.size()].in, b[i % b.size()].p);
  return path;
}

// The cubics of curveOf(v). A QPainterPath from spiroToPath is lines and cubics
// (several per Spiro segment); lines get p1=p0, p2=p3.
static std::vector<Cubic> skeletonOf(const Document::VPath &v)
{
  QPainterPath curve = curveOf(v);
  std::vector<Cubic> skeleton;
  QPointF at = curve.elementAt(0);
  for (int i = 1; i < curve.elementCount();)
  {
    if (curve.elementAt(i).type == QPainterPath::CurveToElement)
    {
      skeleton.push_back(
        {at, curve.elementAt(i), curve.elementAt(i + 1), curve.elementAt(i + 2)});
      i += 3;
    }
    else
    {
      skeleton.push_back({at, at, curve.elementAt(i), curve.elementAt(i)});
      i++;
    }
    at = skeleton.back()[3];
  }
  return skeleton;
}

} // namespace

int Document::addVectorLayer()
{
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

bool Document::addPath(
  const std::vector<SpiroNode> &nodes,
  bool closed,
  const QColor &color,
  double width,
  PaintMode mode,
  const QColor &fillColor)
{
  if (!layers_[current_].vector || !spiroToPath(nodes, closed))
    return false;
  seal();
  Step s;
  s.kind = PathAdd;
  s.layer = current_;
  std::vector<WidthKnot> knots{{0, width}};
  if (!closed)
    knots.push_back({double(skeletonOf({nodes, closed}).size()), width});
  s.paths = {{nodes, closed, color, width, knots}};
  s.paths[0].mode = mode;
  s.paths[0].fillColor =
    fillColor.isValid()
      ? fillColor
      : color; // no fill color given: the fill takes the stroke color
  push(s);
  apply(s, false);
  return true;
}

bool Document::setKnots(int layer, int path, const std::vector<WidthKnot> &knots)
{
  if (
    layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector ||
    path < 0 || path >= int(layers_[layer].paths.size()) || knots.empty())
    return false;
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

// A Bezier path takes Spiro-style node positions (#310): each node moves to its new
// point and its handles move with it. False, changing nothing, unless there is one
// position per node.
static bool placeBezier(Document::VPath &v, const std::vector<SpiroNode> &nodes)
{
  if (nodes.size() != v.bezier.size())
    return false;
  for (size_t i = 0; i < nodes.size(); i++)
  {
    QPointF d = QPointF(nodes[i].x, nodes[i].y) - v.bezier[i].p;
    v.bezier[i].p += d, v.bezier[i].in += d, v.bezier[i].out += d;
  }
  return true;
}

bool Document::setNodes(
  int layer,
  int path,
  const std::vector<SpiroNode> &nodes,
  bool closed,
  const std::vector<WidthKnot> &knots)
{
  if (
    layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector ||
    path < 0 || path >= int(layers_[layer].paths.size()))
    return false;
  VPath after = layers_[layer].paths[path];
  if (
    after.kind == PathKind::Bezier ? !placeBezier(after, nodes)
                                   : !spiroToPath(nodes, closed))
    return false;
  seal();
  Step s;
  s.kind = Nodes;
  s.layer = layer;
  s.to = path;
  s.paths = {layers_[layer].paths[path], after};
  if (after.kind == PathKind::Spiro)
    s.paths[1].nodes = nodes;
  s.paths[1].closed = closed;
  if (!knots.empty())
    s.paths[1].knots = knots;
  push(s);
  apply(s, false);
  return true;
}

bool Document::rasterizeLayer(int layer)
{
  if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector)
    return false;
  seal();
  Step s;
  s.kind = Rasterize;
  s.layer = layer;
  s.paths = layers_[layer].paths;
  push(s);
  apply(s, false);
  return true;
}

bool Document::setPaths(int layer, const std::vector<VPath> &paths)
{
  if (layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector)
    return false;
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

int Document::pathAt(int layer, QPointF p, double tol) const
{
  if (!layers_[layer].vector)
    return -1;
  for (int i = int(layers_[layer].paths.size()); i-- > 0;)
  {
    const VPath &v = layers_[layer].paths[i];
    QPainterPath curve = curveOf(v);
    QPainterPathStroker stroker;
    stroker.setWidth(2 * tol);
    if (
      stroker.createStroke(curve).contains(p) || outline(v).contains(p) ||
      (v.mode != PaintMode::Stroke && curve.contains(p)))
      return i;
  }
  return -1;
}

std::vector<int> Document::pathsIn(int layer, const QPainterPath &area) const
{
  std::vector<int> hit;
  if (!layers_[layer].vector)
    return hit;
  for (int i = 0; i < int(layers_[layer].paths.size()); i++)
  {
    const VPath &v = layers_[layer].paths[i];
    if (
      outline(v).intersects(area) ||
      (v.mode != PaintMode::Stroke && curveOf(v).intersects(area)))
      hit.push_back(i);
  }
  return hit;
}

std::vector<SpiroNode> Document::nodes(int layer, int path) const
{
  if (
    layer < 0 || layer >= int(layers_.size()) || path < 0 ||
    path >= int(layers_[layer].paths.size()))
    return {};
  const VPath &v = layers_[layer].paths[path];
  if (v.kind == PathKind::Spiro)
    return v.nodes;
  std::vector<SpiroNode> ns; // a Bezier path's points, for the Node drag (#310)
  for (const auto &b : v.bezier)
    ns.push_back({b.p.x(), b.p.y(), SpiroType::Corner});
  return ns;
}

int Document::nodeAt(int layer, int path, QPointF p, double tol) const
{
  int best = -1;
  double bestD = tol;
  auto ns = nodes(layer, path);
  for (int i = 0; i < int(ns.size()); i++)
  {
    double d = QLineF(p, QPointF(ns[i].x, ns[i].y)).length();
    if (d <= bestD)
      best = i, bestD = d;
  }
  return best;
}

// The skeleton point at path time t.
static QPointF skeletonAt(const std::vector<Cubic> &skeleton, double t)
{
  int i = std::clamp(int(t), 0, int(skeleton.size()) - 1);
  double u = t - i, w = 1 - u;
  const Cubic &c = skeleton[i];
  return c[0] * (w * w * w) + c[1] * (3 * w * w * u) + c[2] * (3 * w * u * u) +
         c[3] * (u * u * u);
}

// The unit left normal (-ty, tx) at path time t, from nearby points, since a line's
// end control points coincide with its ends.
static QPointF leftNormal(const std::vector<Cubic> &skeleton, double t)
{
  double e = 1e-3;
  QPointF d = skeletonAt(skeleton, std::min<double>(t + e, skeleton.size())) -
              skeletonAt(skeleton, std::max(t - e, 0.0));
  return QPointF(-d.y(), d.x()) / std::hypot(d.x(), d.y());
}

std::vector<QPointF> Document::knotHandles(int layer, int path) const
{
  if (
    layer < 0 || layer >= int(layers_.size()) || path < 0 ||
    path >= int(layers_[layer].paths.size()))
    return {};
  const VPath &v = layers_[layer].paths[path];
  std::vector<Cubic> skeleton = skeletonOf(v);
  std::vector<QPointF> out;
  for (const WidthKnot &k : v.knots)
    out.push_back(
      skeletonAt(skeleton, k.t) + leftNormal(skeleton, k.t) * (k.width / 2));
  return out;
}

std::vector<WidthKnot> Document::knots(int layer, int path) const
{
  if (
    layer < 0 || layer >= int(layers_.size()) || path < 0 ||
    path >= int(layers_[layer].paths.size()))
    return {};
  return layers_[layer].paths[path].knots;
}

// ponytail: samples each cubic then ternary-searches around the best sample; a
// far-off pointer between two lobes may pick the other one
static double projectT(const std::vector<Cubic> &skeleton, QPointF p)
{
  auto dist2 = [&](double t)
  {
    QPointF d = skeletonAt(skeleton, t) - p;
    return d.x() * d.x() + d.y() * d.y();
  };
  const int steps = 32;
  double n = skeleton.size(), best = 0;
  for (int i = 1; i <= steps * int(n); i++)
    if (dist2(i / double(steps)) < dist2(best))
      best = i / double(steps);
  double lo = std::max(best - 1.0 / steps, 0.0),
         hi = std::min(best + 1.0 / steps, n);
  for (int i = 0; i < 40; i++)
  {
    double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
    if (dist2(a) < dist2(b))
      hi = b;
    else
      lo = a;
  }
  return (lo + hi) / 2;
}

std::optional<WidthKnot> Document::projectKnot(int layer, int path, QPointF p) const
{
  if (
    layer < 0 || layer >= int(layers_.size()) || path < 0 ||
    path >= int(layers_[layer].paths.size()))
    return std::nullopt;
  const VPath &v = layers_[layer].paths[path];
  std::vector<Cubic> skeleton = skeletonOf(v);
  double t = projectT(skeleton, p);
  QPointF d = p - skeletonAt(skeleton, t), nrm = leftNormal(skeleton, t);
  return WidthKnot{t, 2 * (d.x() * nrm.x() + d.y() * nrm.y())};
}

bool Document::addKnot(int layer, int path, QPointF p)
{
  auto k = projectKnot(layer, path, p);
  if (!k || !layers_[layer].vector)
    return false;
  const VPath &v = layers_[layer].paths[path];
  auto knots = v.knots;
  k->width = widthAt(knots, k->t, int(skeletonOf(v).size()), v.closed);
  knots.insert(
    std::upper_bound(
      knots.begin(), knots.end(), *k, [](auto &a, auto &b) { return a.t < b.t; }),
    *k);
  return setKnots(layer, path, knots);
}

// A knot's t counts cubics, and Spiro splits a sharp segment into several, so the
// segment under p is found by where the skeleton passes through each node.
// Re-solving reshapes and recounts the cubics, so each knot is put back at the
// point it was on.
bool Document::addNode(int layer, int path, QPointF p, SpiroType type)
{
  auto k = projectKnot(layer, path, p);
  if (!k)
    return false;
  const VPath &v = layers_[layer].paths[path];
  std::vector<Cubic> skeleton = skeletonOf(v);
  std::vector<int> at{0}; // at[j]: the cubic index where node j sits
  for (int c = 0; c < int(skeleton.size()) && at.size() < v.nodes.size(); c++)
    if (
      QLineF(skeleton[c][3], QPointF(v.nodes[at.size()].x, v.nodes[at.size()].y))
        .length() < 1e-6)
      at.push_back(c + 1);
  int s = std::min(
    int(std::upper_bound(at.begin(), at.end(), k->t) - at.begin()) - 1,
    int(v.nodes.size()) - (v.closed ? 1 : 2));
  QPointF pt = skeletonAt(skeleton, k->t);
  for (const SpiroNode &n : v.nodes)
    if (QLineF(pt, QPointF(n.x, n.y)).length() < 1e-3)
      return false; // on a node: Spiro would take two coincident nodes
  auto nodes = v.nodes;
  nodes.insert(nodes.begin() + s + 1, {pt.x(), pt.y(), type});
  if (!spiroToPath(nodes, v.closed))
    return false;
  std::vector<Cubic> fresh = skeletonOf({nodes, v.closed});
  auto knots = v.knots;
  for (WidthKnot &w : knots)
    w.t = w.t <= 0                 ? 0
          : w.t >= skeleton.size() ? double(fresh.size())
                                   : projectT(fresh, skeletonAt(skeleton, w.t));
  return setNodes(layer, path, nodes, v.closed, knots);
}

void Document::previewKnots(
  int layer, int path, const std::vector<WidthKnot> &knots)
{
  if (
    layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector ||
    path < 0 || path >= int(layers_[layer].paths.size()))
    return;
  layers_[layer].paths[path].knots = knots;
  rasterize(layers_[layer]);
}

void Document::previewNodes(
  int layer, int path, const std::vector<SpiroNode> &nodes, bool closed)
{
  if (
    layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector ||
    path < 0 || path >= int(layers_[layer].paths.size()))
    return;
  VPath &v = layers_[layer].paths[path];
  if (
    v.kind == PathKind::Bezier ? !placeBezier(v, nodes)
                               : !spiroToPath(nodes, closed))
    return;
  if (v.kind == PathKind::Spiro)
    v.nodes = nodes;
  layers_[layer].paths[path].closed = closed;
  rasterize(layers_[layer]);
}

void Document::previewPath(int layer, int path, const VPath &v)
{
  if (
    layer < 0 || layer >= int(layers_.size()) || !layers_[layer].vector ||
    path < 0 || path >= int(layers_[layer].paths.size()))
    return;
  layers_[layer].paths[path] = v;
  rasterize(layers_[layer]);
}

void Document::rasterize(Layer &l)
{
  l.image.fill(Qt::transparent);
  QPainter p(&l.image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  for (const VPath &v : l.paths)
  {
    if (v.mode != PaintMode::Stroke)
    {
      // QPainterPath fills an open subpath as if closed by a straight line (#258)
      QPainterPath inside = curveOf(v);
      inside.setFillRule(Qt::WindingFill);
      p.fillPath(inside, srgbToLinear(v.fillColor));
    }
    if (v.mode != PaintMode::Fill)
      p.fillPath(outline(v), srgbToLinear(v.color));
  }
}

QPainterPath Document::curve(const VPath &v)
{
  return curveOf(v);
}

Document::VPath Document::toBezier(const VPath &v)
{
  std::vector<Cubic> sk = skeletonOf(v);
  size_t n = sk.size() + !v.closed;
  VPath b = v;
  b.kind = PathKind::Bezier;
  b.nodes.clear();
  b.bezier.clear();
  for (size_t i = 0; i < n; i++)
  {
    bool first = i == 0 && !v.closed, last = i == sk.size();
    const Cubic &next = sk[i % sk.size()];
    const Cubic &prev = sk[(i + sk.size() - 1) % sk.size()];
    QPointF p = last ? prev[3] : next[0];
    BezierNode node{
      p, last || !first ? prev[2] : p, first || !last ? next[1] : p,
      BezierType::Cusp};
    if (first)
      node.in = p;
    if (last)
      node.out = p;
    // a handle sitting on its point (a line) points along the segment instead
    QPointF a = p - (node.in == p ? prev[0] : node.in),
            c = (node.out == p ? next[3] : node.out) - p;
    if (
      !first && !last &&
      std::abs(a.x() * c.y() - a.y() * c.x()) <
        1e-9 * std::hypot(a.x(), a.y()) * std::hypot(c.x(), c.y()) &&
      QPointF::dotProduct(a, c) > 0)
      node.type = BezierType::Smooth;
    b.bezier.push_back(node);
  }
  return b;
}

// Memoized: the Transform box and picked outlines ask on every paint and pointer
// move, while the paths rarely change.
QPainterPath Document::outline(const VPath &v)
{
  struct Entry
  {
    VPath v;
    QPainterPath outline;
  };
  thread_local std::vector<Entry> cache; // ponytail: last 32 shapes, linear scan; a
                                         // keyed map if far more get picked at once
  auto same = [&](const VPath &c)
  {
    return c.kind == v.kind && c.closed == v.closed && c.nodes == v.nodes &&
           c.bezier == v.bezier && c.knots == v.knots;
  };
  for (auto &e : cache)
    if (same(e.v))
      return e.outline;
  if (cache.size() >= 32)
    cache.erase(cache.begin());
  cache.push_back({v, expandPowerStroke(skeletonOf(v), v.closed, v.knots)});
  return cache.back().outline;
}

void Document::restorePaths(int layer, const std::vector<VPath> &paths)
{
  layers_[layer].vector = true;
  layers_[layer].paths = paths;
  rasterize(layers_[layer]);
  history_.clear();
  applied_ = 0;
}
