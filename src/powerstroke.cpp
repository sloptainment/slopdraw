#include "powerstroke.h"
#include <algorithm>
#include <cmath>

static const double JOHAN_BETA = 0.2;  // Inkscape's CubicBezierJohan: horizontal handles, ease in-out between knots

static bool isLine(const Cubic& c) {
    return std::hypot(c[1].x() - c[0].x(), c[1].y() - c[0].y()) < 1e-9 && std::hypot(c[3].x() - c[2].x(), c[3].y() - c[2].y()) < 1e-9;
}

static QPointF at(const Cubic& c, double u) {
    double v = 1 - u, a = v * v * v, b = 3 * v * v * u, d = 3 * v * u * u, e = u * u * u;
    return c[0] * a + c[1] * b + c[2] * d + c[3] * e;
}

// The (t, half-width) points interpolated: knots plus the zero-width ends (open) or wrapped neighbours (closed).
static std::vector<QPointF> controlPoints(std::vector<WidthKnot> knots, int n, bool closed) {
    std::vector<QPointF> pts;
    if (knots.empty()) return pts;
    std::stable_sort(knots.begin(), knots.end(), [](auto& a, auto& b) { return a.t < b.t; });
    for (auto& k : knots) pts.push_back({k.t, k.width / 2});
    QPointF first = pts.front(), last = pts.back();
    if (closed) {
        pts.insert(pts.begin(), {last.x() - n, last.y()});
        pts.push_back({first.x() + n, first.y()});
    } else {
        pts.insert(pts.begin(), {0, 0});
        pts.push_back({double(n), 0});
    }
    return pts;
}

std::vector<Cubic> widthSpans(std::vector<WidthKnot> knots, int n, bool closed) {
    auto pts = controlPoints(std::move(knots), n, closed);
    std::vector<Cubic> out;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        QPointF a = pts[i], b = pts[i + 1], h(JOHAN_BETA * (b.x() - a.x()), 0);
        out.push_back({a, a + h, b - h, b});
    }
    return out;
}

double offsetAt(const std::vector<Cubic>& spans, double t) {
    std::vector<Cubic> sp;
    for (auto& s : spans) if (s[3].x() > s[0].x() + 1e-12) sp.push_back(s);  // zero-extent spans add nothing
    if (sp.empty()) return spans.empty() ? 0 : std::abs(spans[0][0].y());
    if (t <= sp.front()[0].x()) return std::abs(sp.front()[0].y());
    if (t >= sp.back()[3].x()) return std::abs(sp.back()[3].y());
    double best = -1;
    for (auto& s : sp) {
        if (isLine(s)) {
            if (t >= s[0].x() && t <= s[3].x()) best = std::max(best, std::abs(s[0].y() + (s[3].y() - s[0].y()) * (t - s[0].x()) / (s[3].x() - s[0].x())));
            continue;
        }
        const int N = 32;
        double prev = s[0].x();
        for (int i = 1; i <= N; i++) {  // every crossing of x(u) = t in a 32-step scan, refined by bisection
            double u = double(i) / N, x = at(s, u).x();
            if ((prev < t) != (x < t) || x == t) {
                double lo = double(i - 1) / N, hi = u;
                bool rising = prev < x;
                for (int k = 0; k < 30; k++) {
                    double m = (lo + hi) / 2;
                    if ((at(s, m).x() < t) == rising) lo = m; else hi = m;
                }
                best = std::max(best, std::abs(at(s, (lo + hi) / 2).y()));
            }
            prev = x;
        }
    }
    return best >= 0 ? best : std::abs(sp.back()[3].y());
}

double widthAt(const std::vector<WidthKnot>& knots, double t, int n, bool closed) {
    return 2 * offsetAt(widthSpans(knots, n, closed), t);
}
