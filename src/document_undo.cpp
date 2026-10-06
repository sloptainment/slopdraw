// The undo history: steps, applying them either way, sealing a stroke, the budget (#27).
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
        s.to = layers_[current_].depth;
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
        for (const Step& s : history_) {
            n += s.before.sizeInBytes() + s.after.sizeInBytes();
            for (const Layer& l : s.items) n += l.image.sizeInBytes();  // Tree (#628)
            for (const Layer& l : s.newItems) n += l.image.sizeInBytes();
        }
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
