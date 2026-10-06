#pragma once
// The Node/Knot edit tool's path edits: Spiro and Bezier nodes, Bezier handles,
// Width Knot drags.
#include <cmath>
#include <optional>
#include <set>
#include <vector>
#include <QLineF>
#include "document.h"

// The nodes with those at the selected indexes moved by d (Node tool drag, #140).
inline std::vector<SpiroNode>
moveNodes(std::vector<SpiroNode> nodes, const std::set<int> &selected, QPointF d)
{
  for (int i : selected)
    if (i >= 0 && i < int(nodes.size()))
    {
      nodes[i].x += d.x();
      nodes[i].y += d.y();
    }
  return nodes;
}

// A Width Knot drag moves its point by an eighth of the pointer's motion since the
// press, for fine width/t control (#374).
inline constexpr double kKnotDragScale = 0.125;
inline QPointF knotDragTarget(QPointF knotStart, QPointF press, QPointF pointer)
{
  return knotStart + (pointer - press) * kKnotDragScale;
}

// The node with its out (or in) handle dragged to `to` (#313): a Smooth node turns
// the other handle opposite, keeping its length; a Symmetric one mirrors it; a Cusp
// leaves it.
inline BezierNode moveHandle(BezierNode n, bool out, QPointF to)
{
  QPointF &h = out ? n.out : n.in;
  QPointF &o = out ? n.in : n.out;
  h = to;
  QPointF d = n.p - to;
  double len = std::hypot(d.x(), d.y());
  if (n.type == BezierType::Symmetric)
    o = n.p + d;
  else if (n.type == BezierType::Smooth && len > 0)
    o = n.p + d * (QLineF(n.p, o).length() / len);
  return n;
}

// The nodes without those at the selected indexes (Node tool Delete, #141); nullopt
// if fewer than 2 would remain.
inline std::optional<std::vector<SpiroNode>>
removeNodes(const std::vector<SpiroNode> &nodes, const std::set<int> &selected)
{
  std::vector<SpiroNode> kept;
  for (int i = 0; i < int(nodes.size()); i++)
    if (!selected.count(i))
      kept.push_back(nodes[i]);
  if (kept.size() < 2)
    return std::nullopt;
  return kept;
}

// The nodes with those at the selected indexes set to type t (Tool Options picker,
// #141).
inline std::vector<SpiroNode> retypeNodes(
  std::vector<SpiroNode> nodes, const std::set<int> &selected, SpiroType t)
{
  for (int i : selected)
    if (i >= 0 && i < int(nodes.size()))
      nodes[i].type = t;
  return nodes;
}

// The Bezier path without the nodes at the selected indexes (Node/Knot edit Delete,
// #311): the neighbours join with the handles they have, and a knot past the new
// end moves to it; nullopt if fewer than 2 nodes would remain.
inline std::optional<Document::VPath>
removeBezier(Document::VPath v, const std::set<int> &selected)
{
  std::vector<BezierNode> kept;
  for (int i = 0; i < int(v.bezier.size()); i++)
    if (!selected.count(i))
      kept.push_back(v.bezier[i]);
  if (kept.size() < 2)
    return std::nullopt;
  v.bezier = kept;
  double segs = v.closed ? kept.size() : kept.size() - 1;
  for (auto &k : v.knots)
    k.t = std::min(k.t, segs);
  return v;
}

// The Bezier path split at skeleton parameter t (Ctrl+Shift+click, #312) by de
// Casteljau, so the shape stays: the new Smooth node sits on the curve, the
// neighbours' handles shorten (a Symmetric one becomes Smooth), and each knot keeps
// its point on the curve. nullopt if t is on a node.
inline std::optional<Document::VPath> splitBezier(Document::VPath v, double t)
{
  const int n = int(v.bezier.size());
  const int segs = v.closed ? n : n - 1;
  const int s = std::min(int(t), segs - 1);
  const double u = t - s;
  if (n < 2 || u < 1e-6 || u > 1 - 1e-6)
    return std::nullopt;
  auto lerp = [u](QPointF a, QPointF b) { return a + (b - a) * u; };
  BezierNode &a = v.bezier[s];
  BezierNode &b = v.bezier[(s + 1) % n];
  QPointF q0 = lerp(a.p, a.out), q1 = lerp(a.out, b.in), q2 = lerp(b.in, b.p),
          r0 = lerp(q0, q1), r1 = lerp(q1, q2);
  a.out = q0, b.in = q2;
  for (BezierNode *x : {&a, &b})
    if (x->type == BezierType::Symmetric)
      x->type = BezierType::Smooth;
  v.bezier.insert(
    v.bezier.begin() + s + 1, {lerp(r0, r1), r0, r1, BezierType::Smooth});
  for (auto &k : v.knots)
    if (k.t > s)
      k.t = k.t < s + u   ? s + (k.t - s) / u
            : k.t < s + 1 ? s + 1 + (k.t - s - u) / (1 - u)
                          : k.t + 1;
  return v;
}

// The Bezier nodes with those at the selected indexes set to type t (Node/Knot edit
// picker, #314): the out handle (the in one if out sits on the point) stays, and
// moveHandle turns the other to suit, collinear for Smooth, mirrored for Symmetric.
// A node with both handles on its point (an undragged pen click) gets them pulled
// out along previous -> next, a third of the way to each (#368).
inline std::vector<BezierNode> retypeBezier(
  std::vector<BezierNode> nodes,
  const std::set<int> &selected,
  BezierType t,
  bool closed = false)
{
  const int c = int(nodes.size());
  for (int i : selected)
    if (i >= 0 && i < c)
    {
      auto &n = nodes[i];
      n.type = t;
      if (t != BezierType::Cusp && n.in == n.p && n.out == n.p && c > 1)
      {
        QPointF prev = i > 0 || closed ? nodes[(i + c - 1) % c].p : n.p,
                next = i < c - 1 || closed ? nodes[(i + 1) % c].p : n.p;
        QPointF d = next - prev;
        double len = std::hypot(d.x(), d.y());
        if (len > 0)
          n.out = n.p + d / len * (QLineF(n.p, next).length() / 3),
          n.in = n.p - d / len * (QLineF(prev, n.p).length() / 3);
        if (n.out == n.p)
          n.out = 2 * n.p - n.in; // an open end: mirror the one handle it got
      }
      bool out = n.out != n.p;
      n = moveHandle(n, out, out ? n.out : n.in);
    }
  return nodes;
}
