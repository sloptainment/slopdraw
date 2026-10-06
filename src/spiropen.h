#pragma once
#include <cmath>
#include <vector>
#include "document.h"

// The Tool Options picker's rows (#53), in display order.
inline constexpr struct
{
  const char *name;
  SpiroType type;
} kNodeTypes[] = {
  {"Corner", SpiroType::Corner},
  {"G4", SpiroType::G4},
  {"G2", SpiroType::G2},
  {"Left", SpiroType::Left},
  {"Right", SpiroType::Right}};

// The Spiro Pen's path in progress (#28, /r/6): clicks place nodes of the current
// type, finish turns them into one path on the current Vector Layer. The Canvas
// decides what counts as a click and which key finishes.
struct SpiroPen
{
  SpiroType type =
    SpiroType::G2;     // what Inkscape's Spiro mode makes for a smooth node
  bool bezier = false; // the Tool Options toggle (#308): Bezier clicks place
                       // corners, finish makes a Bezier path
  std::vector<SpiroNode> nodes; // in Bezier, all Corners: Spiro draws them as the
                                // same straight segments, so the preview is shared

  std::vector<QPointF> outs; // Bezier: each node's out handle, parallel to nodes;
                             // the in handle mirrors it (#309)
  bool closing = false; // Bezier: a press on the first node is down, so drag pulls
                        // node 0's handle and release closes (#368)

  void click(QPointF p, bool corner = false)
  {
    nodes.push_back({p.x(), p.y(), corner || bezier ? SpiroType::Corner : type});
    outs.push_back(p);
  } // Shift+click places a Corner (#274)
  // True when p is within tol of the first node: clicking there closes the path.
  bool onFirst(QPointF p, double tol) const
  {
    return !nodes.empty() &&
           std::hypot(p.x() - nodes[0].x, p.y() - nodes[0].y) <= tol;
  }
  void backspace()
  {
    if (!nodes.empty())
      nodes.pop_back(), outs.pop_back();
  }
  void cancel()
  {
    nodes.clear();
    outs.clear();
    closing = false;
  }
  // Bezier: the last node's out handle follows the pointer (press-drag, #309);
  // while closing, the first node's (#368).
  void drag(QPointF p)
  {
    if (bezier && !outs.empty())
      (closing ? outs.front() : outs.back()) = p;
  }
  // The nodes as Bezier nodes: dragged ones Symmetric, the rest Cusps with handles
  // on their points.
  std::vector<BezierNode> bezierNodes() const
  {
    std::vector<BezierNode> b;
    for (size_t i = 0; i < nodes.size(); i++)
    {
      QPointF p(nodes[i].x, nodes[i].y);
      b.push_back(
        {p, 2 * p - outs[i], outs[i],
         outs[i] == p ? BezierType::Cusp : BezierType::Symmetric});
    }
    return b;
  }
  // One undo step. On false (not a Vector Layer, or no curve) the nodes stay, so
  // the user can carry on or cancel.
  bool finish(
    Document &doc,
    bool closed,
    const QColor &color,
    double width,
    Document::PaintMode mode = Document::PaintMode::Stroke,
    const QColor &fillColor = {})
  {
    if (bezier)
    {
      if (nodes.size() < 2 || !doc.vector(doc.current()))
        return false;
      Document::VPath v{{},           closed,
                        color,        width,
                        {{0, width}}, Document::PathKind::Bezier,
                        mode,         fillColor.isValid() ? fillColor : color};
      if (!closed)
        v.knots.push_back(
          {double(nodes.size() - 1), width}); // one cubic per segment (#306)
      v.bezier = bezierNodes();
      auto paths = doc.paths(doc.current());
      paths.push_back(v);
      if (!doc.setPaths(doc.current(), paths))
        return false;
    }
    else if (!doc.addPath(nodes, closed, color, width, mode, fillColor))
      return false;
    cancel();
    return true;
  }
};
