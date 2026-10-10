#include "document.h"
#include <algorithm>
#include <cstring>
#include <QColorSpace>

Document::Document(QSize size, qsizetype undoBudget, bool floatPoint)
  : budget_(undoBudget), floatPoint_(floatPoint)
{
  layers_.push_back({true, 1, layerImage(size), "Background"});
  layers_[0].image.fill(Qt::white);
}

QImage Document::layerImage(QSize size) const
{
  QImage out(
    size, floatPoint_ ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA64);
  out.setColorSpace(QColorSpace::SRgbLinear);
  out.fill(Qt::transparent);
  return out;
}

// A picture with no profile is read as sRGB, one with a profile from that profile.
QImage Document::layerImage(const QImage &from) const
{
  QImage out = from.convertToFormat(
    floatPoint_ ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA64);
  if (!out.colorSpace().isValid())
    out.setColorSpace(QColorSpace::SRgb);
  return out.convertedToColorSpace(QColorSpace::SRgbLinear);
}

namespace
{

// src scaled by opacity over dst (or, atop, only where dst has alpha), both
// RGBA64 or both FP16 with straight alpha, in float so opacity is not cut to 1/255.
void blend(QImage &dst, const QImage &src, float opacity, bool atop)
{
  const bool fp = dst.format() == QImage::Format_RGBA16FPx4;
  for (int y = 0; y < dst.height(); y++)
  {
    auto *d = reinterpret_cast<quint16 *>(dst.scanLine(y));
    auto *s = reinterpret_cast<quint16 *>(const_cast<uchar *>(src.constScanLine(y)));
    for (int x = 0; x < dst.width(); x++, d += 4, s += 4)
    {
      const Px dp{d, fp}, sp{s, fp};
      if (sp.get(3) <= 0)
        continue;
      if (!atop && opacity == 1 && sp.get(3) == 1)
      { // opaque over anything is the source
        std::memcpy(d, s, 8);
        continue;
      }
      const float sa = sp.get(3) * opacity, da = dp.get(3);
      const float oa = atop ? da : sa + da * (1 - sa);
      if (sa <= 0 || oa <= 0)
        continue;
      for (int c = 0; c < 3; c++)
        dp.set(
          c, atop ? sp.get(c) * sa + dp.get(c) * (1 - sa)
                  : (sp.get(c) * sa + dp.get(c) * da * (1 - sa)) / oa);
      dp.set(3, oa);
    }
  }
}

} // namespace

QImage Document::render() const
{
  return compose(0, int(layers_.size()), 0);
}

QImage Document::groupImage(int group) const
{
  int lo = group;
  while (lo > 0 && layers_[lo - 1].depth > layers_[group].depth)
    lo--;
  return compose(lo, group, layers_[group].depth + 1);
}

QImage Document::compose(int lo, int hi, int d) const
{
  QImage out = layerImage(layers_[0].image.size());
  // walk down the level's items, a group taking the deeper run below it
  std::vector<std::pair<int, int>> items; // (item, first child)
  for (int i = hi - 1; i >= lo;)
  {
    int s = i;
    if (layers_[i].group)
      while (s > lo && layers_[s - 1].depth > d)
        s--;
    items.push_back({i, s});
    i = s - 1;
  }
  for (size_t k = items.size(); k-- > 0;)
  {
    const Layer &l = layers_[items[k].first];
    if (!l.visible)
      continue;
    blend(
      out, l.group ? compose(items[k].second, items[k].first, d + 1) : l.image,
      l.opacity, k + 1 < items.size() && l.inheritAlpha);
  }
  return out;
}

// ponytail: composites the whole canvas per call; fine per click, cache if it ever
// runs per mouse move
QColor Document::colorAt(QPoint p) const
{
  return layers_[0].image.rect().contains(p)
           ? QColor::fromRgba(linearToSrgb(render().pixelColor(p)).rgba())
           : QColor();
}

int Document::addLayer()
{
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

int Document::addGroup()
{
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

int Document::blockStart(int i) const
{
  int lo = i;
  if (layers_[i].group)
    while (lo > 0 && layers_[lo - 1].depth > layers_[i].depth)
      lo--;
  return lo;
}

bool Document::wellFormed(const std::vector<Layer> &v)
{
  if (v.back().depth != 0)
    return false;
  for (size_t i = v.size() - 1; i > 0; i--)
    if (v[i - 1].depth < 0 || v[i - 1].depth > v[i].depth + v[i].group)
      return false;
  return true;
}

std::vector<std::pair<int, int>>
Document::blocksOf(const std::vector<int> &items) const
{
  std::vector<int> sorted = items;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::vector<std::pair<int, int>> out;
  if (sorted.empty() || sorted.front() < 1 || sorted.back() >= int(layers_.size()))
    return out;
  int floor = int(layers_.size());
  for (auto i = sorted.rbegin(); i != sorted.rend(); ++i)
    if (*i < floor)
    {
      out.insert(out.begin(), {blockStart(*i), *i});
      floor = blockStart(*i);
    }
  return out;
}

bool Document::relocate(
  const std::vector<std::pair<int, int>> &blocks, int at, int depth, bool group)
{
  std::vector<Layer> v = layers_, block;
  std::vector<int> ids(v.size()), blockIds;
  std::iota(ids.begin(), ids.end(), 0);
  for (auto [lo, hi] : blocks)
    for (int i = lo; i <= hi; i++)
    {
      block.push_back(layers_[i]);
      block.back().depth += depth + group - layers_[hi].depth;
      blockIds.push_back(i);
    }
  for (auto b = blocks.rbegin(); b != blocks.rend(); ++b)
  {
    v.erase(v.begin() + b->first, v.begin() + b->second + 1);
    ids.erase(ids.begin() + b->first, ids.begin() + b->second + 1);
  }
  if (group)
  {
    Layer g;
    g.name = "Group " + QString::number(layers_.size());
    g.depth = depth;
    g.group = true;
    block.push_back(g);
    blockIds.push_back(-1);
  }
  v.insert(v.begin() + at, block.begin(), block.end());
  ids.insert(ids.begin() + at, blockIds.begin(), blockIds.end());
  bool same = !group;
  for (size_t i = 0; same && i < v.size(); i++)
    same = ids[i] == int(i) && v[i].depth == layers_[i].depth;
  if (at < 1 || depth < 0 || !wellFormed(v) || same)
    return false;
  seal();
  Step s;
  s.kind = Tree;
  s.items = layers_;
  s.newItems = v;
  s.to = current_;
  s.layer = group ? at + int(block.size()) - 1
                  : int(
                      std::find(ids.begin(), ids.end(), current_) -
                      ids.begin()); // the current item keeps its identity
  push(s);
  apply(s, false);
  moved_ = ids;
  return true;
}

int Document::wrap(const std::vector<int> &items)
{
  auto blocks = blocksOf(items);
  if (blocks.empty())
    return -1;
  int total = 0;
  for (auto [lo, hi] : blocks)
    total += hi - lo + 1;
  int top = blocks.back().second;
  return relocate(blocks, top + 1 - total, layers_[top].depth, true) ? current_
                                                                     : -1;
}

bool Document::stepItem(int layer, bool up)
{
  int n = int(layers_.size());
  if (layer < 1 || layer >= n)
    return false;
  int lo = blockStart(layer), d = layers_[layer].depth, size = layer - lo + 1;
  // `at` is an index of the list without the block
  if (up)
  {
    int j = layer + 1;
    if (j >= n)
      return false;
    int dj = layers_[j].depth;
    if (dj < d)
      return relocate(
        lo, layer, j + 1 - size, d - 1); // j is the group: out above it
    if (dj == d)
      return relocate(
        lo, layer,
        layers_[j].group && !layers_[j].collapsed ? j - size : j + 1 - size,
        d + (layers_[j].group &&
             !layers_[j].collapsed)); // an empty group, or a layer
    int k = j;
    while (layers_[k].depth > d)
      k++; // the group above whose block j ends
    return layers_[k].collapsed ? relocate(lo, layer, k + 1 - size, d)
                                : relocate(lo, layer, lo, d + 1);
  }
  int j = lo - 1;
  int dj = layers_[j].depth;
  if (dj < d)
    return relocate(
      lo, layer, lo, d - 1); // the bottom of its group: out below the group's block
  if (!layers_[j].group)
    return relocate(lo, layer, j, d);
  return layers_[j].collapsed ? relocate(lo, layer, blockStart(j), d)
                              : relocate(lo, layer, j, d + 1);
}

bool Document::stepItems(const std::vector<int> &items, bool up)
{
  auto blocks = blocksOf(items);
  if (blocks.empty())
    return false;
  std::vector<int> tops; // the stepping order: the ones ahead go first
  for (auto b : blocks)
    tops.insert(up ? tops.begin() : tops.end(), b.second);
  seal();
  size_t base = applied_;
  for (size_t k = 0; k < tops.size(); k++)
  {
    if (!stepItem(tops[k], up))
    {
      while (applied_ > base)
        undo();
      history_.resize(applied_);
      return false;
    }
    std::vector<int> now(moved_.size());
    for (size_t p = 0; p < moved_.size(); p++)
      now[moved_[p]] = int(p);
    for (size_t j = k + 1; j < tops.size(); j++)
      tops[j] = now[tops[j]];
  }
  if (applied_ - base > 1)
  { // one Tree step from the first's before to the last's after
    history_[base].newItems = history_[applied_ - 1].newItems;
    history_[base].layer = history_[applied_ - 1].layer;
    applied_ = base + 1;
    history_.resize(applied_);
  }
  return true;
}

bool Document::moveItems(const std::vector<int> &items, int gap, int depth)
{
  auto blocks = blocksOf(items);
  if (blocks.empty() || gap < 1 || gap > int(layers_.size()))
    return false;
  int below = 0;
  for (auto [lo, hi] : blocks)
  {
    if (gap > lo && gap <= hi)
      return false;
    if (hi < gap)
      below += hi - lo + 1;
  }
  return relocate(blocks, gap - below, depth);
}

void Document::addReference(const Reference &r)
{
  auto refs = refs_;
  refs.push_back(r);
  setReferences(refs);
}

void Document::setReferences(const std::vector<Reference> &refs)
{
  seal();
  Step s;
  s.kind = References;
  s.refs = refs_;
  s.newRefs = refs;
  push(s);
  apply(s, false);
}

void Document::restoreLayer(
  int layer,
  const QImage &image,
  const QString &name,
  bool visible,
  double opacity,
  bool alphaLock,
  bool inheritAlpha,
  int depth,
  bool group)
{
  layers_[layer] = {
    visible, opacity,
    group ? QImage() : layerImage(image), name};
  layers_[layer].depth = depth;
  layers_[layer].group = group;
  layers_[layer].alphaLock = alphaLock;
  layers_[layer].inheritAlpha = inheritAlpha;
  history_.clear();
  applied_ = 0;
  strokeStart_ = QImage();
}

void Document::setCurrent(int layer)
{
  seal();
  current_ = layer;
}

void Document::setVisible(int layer, bool visible)
{
  if (layers_[layer].visible == visible)
    return;
  seal();
  Step s;
  s.kind = Visible;
  s.layer = layer;
  s.a = !visible;
  s.b = visible;
  push(s);
  apply(s, false);
}

void Document::setName(int layer, const QString &name)
{
  if (layers_[layer].name == name)
    return;
  seal();
  Step s;
  s.kind = Name;
  s.layer = layer;
  s.na = layers_[layer].name;
  s.nb = name;
  push(s);
  apply(s, false);
}

void Document::setOpacity(int layer, double opacity)
{
  if (layers_[layer].opacity == opacity)
    return;
  seal();
  Step s;
  s.kind = Opacity;
  s.layer = layer;
  s.a = layers_[layer].opacity;
  s.b = opacity;
  push(s);
  apply(s, false);
}

bool Document::setAlphaLock(int layer, bool on)
{
  if (layers_[layer].vector || layers_[layer].group)
    return false;
  if (layers_[layer].alphaLock == on)
    return true;
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

void Document::setInheritAlpha(int layer, bool on)
{
  if (layers_[layer].inheritAlpha == on)
    return;
  seal();
  Step s;
  s.kind = InheritAlpha;
  s.layer = layer;
  s.a = !on;
  s.b = on;
  push(s);
  apply(s, false);
}

bool Document::deleteLayer(int layer)
{
  if (layer < 1 || layer >= int(layers_.size()))
    return false;
  seal();
  Step s;
  if (layers_[layer].group)
  { // the block goes with it
    int lo = layer;
    while (layers_[lo - 1].depth > layers_[layer].depth)
      lo--;
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
  s.to = layers_[layer].alphaLock | layers_[layer].inheritAlpha << 1 |
         layers_[layer].depth << 2;
  push(s);
  apply(s, false);
  return true;
}

bool Document::editLayers(
  const std::vector<int> &layers, const std::function<bool(Layer &)> &change,
  bool withGroups)
{
  auto v = layers_;
  bool any = false;
  for (int i : layers)
    if (i > 0 && i < int(v.size()) && (withGroups || !v[i].group))
      any |= change(v[i]);
  if (!any)
    return false;
  seal();
  Step s;
  s.kind = Tree;
  s.items = layers_;
  s.newItems = v;
  s.to = s.layer = current_;
  push(s);
  apply(s, false);
  return true;
}

bool Document::setVisible(const std::vector<int> &layers, bool visible)
{
  return editLayers(
    layers, [&](Layer &l) { return std::exchange(l.visible, visible) != visible; });
}

bool Document::setOpacity(const std::vector<int> &layers, double opacity)
{
  return editLayers(
    layers, [&](Layer &l) { return std::exchange(l.opacity, opacity) != opacity; });
}

bool Document::setAlphaLock(const std::vector<int> &layers, bool on)
{
  return editLayers(
    layers,
    [&](Layer &l) { return !l.vector && std::exchange(l.alphaLock, on) != on; });
}

bool Document::setInheritAlpha(const std::vector<int> &layers, bool on)
{
  return editLayers(
    layers, [&](Layer &l) { return std::exchange(l.inheritAlpha, on) != on; });
}

void Document::previewLayer(
  int layer, const QString &name, double opacity, bool alphaLock, bool inheritAlpha)
{
  Layer &l = layers_[layer];
  l.name = name;
  l.opacity = opacity;
  l.alphaLock = alphaLock && !l.vector && !l.group;
  l.inheritAlpha = inheritAlpha;
}

bool Document::setLayer(
  int layer, const QString &name, double opacity, bool alphaLock, bool inheritAlpha)
{
  return editLayers(
    {layer},
    [&](Layer &l)
    {
      Layer was = l;
      l.name = name;
      l.opacity = opacity;
      l.alphaLock = alphaLock && !l.vector && !l.group;
      l.inheritAlpha = inheritAlpha;
      return l.name != was.name || l.opacity != was.opacity ||
             l.alphaLock != was.alphaLock || l.inheritAlpha != was.inheritAlpha;
    },
    true);
}

bool Document::rasterizeLayers(const std::vector<int> &layers)
{
  return editLayers(
    layers,
    [](Layer &l)
    {
      bool v = std::exchange(l.vector, false);
      if (v)
        l.paths.clear();
      return v;
    });
}

bool Document::deleteLayers(const std::vector<int> &layers)
{
  std::vector<int> gone;
  for (int i : layers)
    if (i > 0 && i < int(layers_.size()) && !layers_[i].group)
      gone.push_back(i);
  std::sort(gone.begin(), gone.end());
  gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
  if (gone.empty())
    return false;
  seal();
  Step s;
  s.kind = Tree;
  s.items = s.newItems = layers_;
  for (auto it = gone.rbegin(); it != gone.rend(); ++it)
    s.newItems.erase(s.newItems.begin() + *it);
  if (!wellFormed(s.newItems))
    return false;
  s.to = current_;
  s.layer = std::max(
    0,
    current_ - int(std::count_if(
                 gone.begin(), gone.end(), [&](int i) { return i <= current_; })));
  push(s);
  apply(s, false);
  return true;
}

bool Document::moveLayer(int from, int to)
{
  int n = int(layers_.size());
  if (from < 1 || to < 1 || from >= n || to >= n || from == to)
    return false;
  seal();
  Step s;
  s.kind = Move;
  s.layer = from;
  s.to = to;
  push(s);
  apply(s, false);
  return true;
}

void Document::applyMove(int from, int to)
{
  Layer l = std::move(layers_[from]);
  layers_.erase(layers_.begin() + from);
  layers_.insert(layers_.begin() + to, std::move(l));
  if (current_ == from)
    current_ = to; // the current layer keeps its identity
  else if (from < current_ && current_ <= to)
    current_--;
  else if (to <= current_ && current_ < from)
    current_++;
}
