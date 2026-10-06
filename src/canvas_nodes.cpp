// Canvas input for Node/Knot edit: selecting and dragging nodes, Bezier handles and
// Width Knots.
#include "canvas.h"
#include <QLineF>
#include "nodeedit.h"

void Canvas::retypeSelected(SpiroType t)
{
  editType_ = t;
  if (tool_ != NodeKnotEditTool || bezierSelected() || nodeSels().empty())
    return;
  editSelected(
    [&](Document::VPath &v, const std::set<int> &sel)
    {
      if (v.kind != Document::PathKind::Spiro)
        return true;
      v.nodes = retypeNodes(v.nodes, sel, t);
      return bool(spiroToPath(v.nodes, v.closed));
    });
  update();
}

void Canvas::retypeSelected(BezierType t)
{
  if (!bezierSelected() || nodeSels().empty())
    return;
  editSelected(
    [&](Document::VPath &v, const std::set<int> &sel)
    {
      if (v.kind == Document::PathKind::Bezier)
        v.bezier = retypeBezier(v.bezier, sel, t, v.closed);
      return true;
    });
  update();
}

bool Canvas::bezierSelected() const
{
  if (
    tool_ != NodeKnotEditTool || selected_ < 0 ||
    selected_ >= int(s_.doc.paths(s_.doc.current()).size()))
    return false;
  return s_.doc.paths(s_.doc.current())[selected_].kind ==
         Document::PathKind::Bezier;
}

void Canvas::selectAt(QPointF pos)
{
  selected_ = s_.doc.pathAt(
    s_.doc.current(), s_.view.toDoc(pos), 6 / s_.view.screenRadius(1));
  nodeSel_.clear();
  others_.clear();
  update();
}

void Canvas::selectPress(QPointF pos, Qt::KeyboardModifiers mods)
{
  focusHit(pos);
  int i = knotAt(pos);
  int n = selected_ < 0 ? -1
                        : s_.doc.nodeAt(
                            s_.doc.current(), selected_, s_.view.toDoc(pos),
                            6 / s_.view.screenRadius(1));
  if (i >= 0 && n >= 0)
  { // a knot handle near a node (the t=0 knot by node 0): the nearer one takes the
    // press (#368)
    QPointF d = s_.view.toDoc(pos),
            np(
              s_.doc.nodes(s_.doc.current(), selected_)[n].x,
              s_.doc.nodes(s_.doc.current(), selected_)[n].y);
    if (
      QLineF(np, d).length() <=
      QLineF(s_.doc.knotHandles(s_.doc.current(), selected_)[i], d).length())
      i = -1;
  }
  if (i >= 0)
  {
    knotDrag_ = i;
    knotsBefore_ = s_.doc.knots(s_.doc.current(), selected_);
    knotPress_ = s_.view.toDoc(pos);
    knotStart_ = s_.doc.knotHandles(s_.doc.current(), selected_)[i];
    return;
  }
  if (auto h = handleAt(pos))
  {
    std::tie(handleDrag_, handleOut_) = *h;
    pathBefore_ = s_.doc.paths(s_.doc.current())[selected_];
    return;
  }
  if (n < 0 && (mods & Qt::ShiftModifier))
  { // Shift+click on another path shows its nodes to select too, again hides them
    int p = selected_ < 0 ? -1
                          : s_.doc.pathAt(
                              s_.doc.current(), s_.view.toDoc(pos),
                              6 / s_.view.screenRadius(1));
    if (p >= 0 && p != selected_ && !others_.erase(p))
      others_.try_emplace(p);
    update();
    return;
  }
  if (n < 0)
    return selectAt(pos);
  nodePress(pos, n, mods);
}

void Canvas::nodePress(QPointF pos, int n, Qt::KeyboardModifiers mods)
{
  if (mods & Qt::ShiftModifier)
  {
    if (!nodeSel_.erase(n))
      nodeSel_.insert(n);
  }
  else
  { // a press on a selected node keeps the selection, so all selected nodes drag
    // together
    if (!nodeSel_.count(n))
    {
      nodeSel_ = {n};
      for (auto &kv : others_)
        kv.second.clear();
    }
    nodeDrag_ = true;
    nodeStart_ = s_.view.toDoc(pos);
    nodesBefore_.clear();
    for (auto &[i, sel] : nodeSels())
      nodesBefore_[i] = {
        s_.doc.nodes(s_.doc.current(), i),
        s_.doc.paths(s_.doc.current())[i]
          .closed}; // nodes(), so a Bezier path's points (#310)
  }
  update();
}

void Canvas::makePrimary(int p)
{
  if (p == selected_)
    return;
  others_[selected_] = std::move(nodeSel_);
  nodeSel_ = std::move(others_[p]);
  others_.erase(p);
  selected_ = p;
}

void Canvas::focusHit(QPointF pos)
{
  if (selected_ < 0)
    return;
  auto hit = [&]
  {
    return knotAt(pos) >= 0 || handleAt(pos) ||
           s_.doc.nodeAt(
             s_.doc.current(), selected_, s_.view.toDoc(pos),
             6 / s_.view.screenRadius(1)) >= 0;
  };
  if (hit())
    return;
  int home = selected_;
  for (auto &[p, _] : std::map(others_))
  {
    makePrimary(p);
    if (hit())
      return;
  }
  makePrimary(home);
}

std::map<int, std::set<int>> Canvas::nodeSels() const
{
  auto m = others_;
  if (selected_ >= 0)
    m[selected_] = nodeSel_;
  std::erase_if(m, [](auto &kv) { return kv.second.empty(); });
  return m;
}

bool Canvas::editSelected(
  const std::function<bool(Document::VPath &, const std::set<int> &)> &f)
{
  auto paths = s_.doc.paths(s_.doc.current());
  for (auto &[i, sel] : nodeSels())
    if (!f(paths[i], sel))
      return false;
  return s_.doc.setPaths(s_.doc.current(), paths);
}

void Canvas::deleteSelectedNodes()
{
  bool ok = editSelected(
    [&](Document::VPath &v, const std::set<int> &sel)
    {
      if (v.kind == Document::PathKind::Bezier)
      {
        auto kept = removeBezier(v, sel);
        if (kept)
          v = *kept;
        return bool(kept);
      }
      auto kept = removeNodes(v.nodes, sel);
      if (kept)
        v.nodes = *kept;
      return kept && spiroToPath(v.nodes, v.closed);
    });
  if (!ok)
    return;
  nodeSel_.clear();
  for (auto &kv : others_)
    kv.second.clear();
}

void Canvas::dragNodesTo(QPointF pos)
{
  preview_ = [=, this]
  {
    auto sels = nodeSels();
    for (auto &[i, was] : nodesBefore_)
      s_.doc.previewNodes(
        s_.doc.current(), i,
        moveNodes(was.nodes, sels[i], s_.view.toDoc(pos) - nodeStart_), was.closed);
  };
  update();
}

void Canvas::endNodeDrag()
{
  if (!nodeDrag_)
    return;
  flushPreview();
  auto after = s_.doc.paths(s_.doc.current());
  bool moved = false;
  for (auto &[i, was] : nodesBefore_)
  {
    auto now = s_.doc.nodes(s_.doc.current(), i);
    moved |= !std::equal(
      now.begin(), now.end(), was.nodes.begin(),
      [](auto &a, auto &b) { return a.x == b.x && a.y == b.y; });
    s_.doc.previewNodes(s_.doc.current(), i, was.nodes, was.closed);
  }
  if (moved)
    s_.doc.setPaths(s_.doc.current(), after);
  nodeDrag_ = false;
  update();
}

std::optional<std::pair<int, bool>> Canvas::handleAt(QPointF pos)
{
  if (tool_ != NodeKnotEditTool || selected_ < 0)
    return std::nullopt;
  auto &b = s_.doc.paths(s_.doc.current())[selected_].bezier;
  for (int i : nodeSel_)
    for (bool out : {false, true})
    {
      if (i >= int(b.size()))
        break;
      QPointF h = out ? b[i].out : b[i].in;
      if (
        h != b[i].p &&
        QLineF(h, s_.view.toDoc(pos)).length() * s_.view.screenRadius(1) <= 6)
        return std::pair{i, out};
    }
  return std::nullopt;
}

void Canvas::dragHandleTo(QPointF pos)
{
  preview_ = [=, this]
  {
    auto v = pathBefore_;
    v.bezier[handleDrag_] =
      moveHandle(v.bezier[handleDrag_], handleOut_, s_.view.toDoc(pos));
    s_.doc.previewPath(s_.doc.current(), selected_, v);
  };
  update();
}

void Canvas::endHandleDrag()
{
  if (handleDrag_ < 0)
    return;
  flushPreview();
  auto paths = s_.doc.paths(s_.doc.current());
  s_.doc.previewPath(s_.doc.current(), selected_, pathBefore_);
  auto &a = paths[selected_].bezier[handleDrag_];
  auto &w = pathBefore_.bezier[handleDrag_];
  if (a.in != w.in || a.out != w.out)
    s_.doc.setPaths(s_.doc.current(), paths);
  handleDrag_ = -1;
  update();
}

int Canvas::knotAt(QPointF pos)
{
  if (tool_ != NodeKnotEditTool || selected_ < 0)
    return -1;
  auto handles = s_.doc.knotHandles(s_.doc.current(), selected_);
  for (int i = 0; i < int(handles.size()); i++)
    if (
      QLineF(handles[i], s_.view.toDoc(pos)).length() * s_.view.screenRadius(1) <=
      6)
      return i;
  return -1;
}

bool Canvas::editKnot(QPointF pos, Qt::KeyboardModifiers mods)
{
  int i = knotAt(pos);
  if (i < 0)
  {
    if (
      tool_ != NodeKnotEditTool || selected_ < 0 || (mods & Qt::AltModifier) ||
      s_.doc.pathAt(
        s_.doc.current(), s_.view.toDoc(pos), 6 / s_.view.screenRadius(1)) !=
        selected_)
      return false;
    if (mods & Qt::ShiftModifier)
    {
      if (bezierSelected())
      {
        auto k =
          s_.doc.projectKnot(s_.doc.current(), selected_, s_.view.toDoc(pos));
        auto paths = s_.doc.paths(s_.doc.current());
        if (auto split = k ? splitBezier(paths[selected_], k->t) : std::nullopt)
        {
          paths[selected_] = *split;
          s_.doc.setPaths(s_.doc.current(), paths);
          nodeSel_.clear(); // indexes after the new node moved
        }
      }
      else
        s_.doc.addNode(s_.doc.current(), selected_, s_.view.toDoc(pos), editType_);
    }
    else
      s_.doc.addKnot(s_.doc.current(), selected_, s_.view.toDoc(pos));
    update();
    return true;
  }
  auto k = s_.doc.knots(s_.doc.current(), selected_);
  s_.doc.setKnots(
    s_.doc.current(), selected_,
    (mods & Qt::AltModifier) ? deleteKnot(k, i) : duplicateKnot(k, i));
  update();
  return true;
}

void Canvas::dragKnotTo(QPointF pos)
{
  preview_ = [=, this]
  {
    auto k = s_.doc.projectKnot(
      s_.doc.current(), selected_,
      knotDragTarget(knotStart_, knotPress_, s_.view.toDoc(pos)));
    if (!k)
      return;
    auto knots = knotsBefore_;
    knots[knotDrag_] = *k;
    s_.doc.previewKnots(s_.doc.current(), selected_, knots);
  };
  update();
}

void Canvas::endKnotDrag()
{
  if (knotDrag_ < 0)
    return;
  flushPreview();
  auto after = s_.doc.knots(s_.doc.current(), selected_);
  s_.doc.previewKnots(s_.doc.current(), selected_, knotsBefore_);
  if (
    after.size() == knotsBefore_.size() &&
    !std::equal(
      after.begin(), after.end(), knotsBefore_.begin(),
      [](auto &a, auto &b) { return a.t == b.t && a.width == b.width; }))
    s_.doc.setKnots(s_.doc.current(), selected_, after);
  knotDrag_ = -1;
  update();
}
