#include "powerstroke.h"
#include <algorithm>
#include <cmath>
#include <functional>

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

static const double EPS = 1e-9, MAX_TURN = M_PI / 6, MAX_DT = 1.0 / 8;  // a piece turns < 30 degrees and spans <= 1/8 segment, so its width is near linear
static const int MAX_DEPTH = 8;

static double len(QPointF p) { return std::hypot(p.x(), p.y()); }
static double cross(QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); }
static QPointF unit(QPointF p) { return p / len(p); }
static QPointF normal(QPointF p) { QPointF u = unit(p); return {-u.y(), u.x()}; }  // left normal, y up

// First and second derivative of a cubic at u.
static std::pair<QPointF, QPointF> derivs(const Cubic& c, double u) {
    double v = 1 - u;
    return {3 * (v * v * (c[1] - c[0]) + 2 * v * u * (c[2] - c[1]) + u * u * (c[3] - c[2])), 6 * (v * (c[2] - 2 * c[1] + c[0]) + u * (c[3] - 2 * c[2] + c[1]))};
}

static double curvatureAt(const Cubic& c, double u) {
    auto [d1, d2] = derivs(c, u);
    double speed = len(d1);
    return speed < EPS ? 0 : cross(d1, d2) / (speed * speed * speed);
}

// Total turning of the control polygon.
static double turn(const Cubic& c) {
    std::vector<QPointF> legs;
    for (int i = 0; i < 3; i++) if (len(c[i + 1] - c[i]) > EPS) legs.push_back(c[i + 1] - c[i]);
    double t = 0;
    for (size_t i = 1; i < legs.size(); i++) t += std::abs(std::atan2(cross(legs[i - 1], legs[i]), QPointF::dotProduct(legs[i - 1], legs[i])));
    return t;
}

// Offset radius r is below the radius of curvature at five samples (else the inner offset cusps).
static bool safe(const Cubic& c, double r) {
    for (double u : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        auto [d1, d2] = derivs(c, u);
        double v = len(d1);
        if (v >= EPS && std::abs(cross(d1, d2)) / (v * v * v) * r > 1) return false;
    }
    return true;
}

// Unit tangent at the start (u=0) or end (u=1), skipping coincident control points.
static QPointF tangent(const Cubic& c, int u) {
    for (int i = 1; i <= 3; i++) {
        QPointF v = u ? c[3] - c[3 - i] : c[i] - c[0];
        if (len(v) > EPS) return unit(v);
    }
    return {1, 0};
}

struct Piece { Cubic c; double t0, t1; };

// de Casteljau at u, the t range split proportionally; lines stay lines (p1=p0, p2=p3).
static std::pair<Piece, Piece> cutAt(const Piece& p, double u) {
    auto l = [&](QPointF a, QPointF b) { return QPointF(a.x() + (b.x() - a.x()) * u, a.y() + (b.y() - a.y()) * u); };
    const auto& [p0, p1, p2, p3] = p.c;
    double tm = p.t0 + (p.t1 - p.t0) * u;
    if (isLine(p.c)) { QPointF m = l(p0, p3); return {{{p0, p0, m, m}, p.t0, tm}, {{m, m, p3, p3}, tm, p.t1}}; }
    QPointF p01 = l(p0, p1), p12 = l(p1, p2), p23 = l(p2, p3), a = l(p01, p12), b = l(p12, p23), m = l(a, b);
    return {{{p0, p01, a, m}, p.t0, tm}, {{m, b, p23, p3}, tm, p.t1}};
}

using Width = std::function<double(double)>;

// Split a segment at the width knots inside it, then until each piece turns < 30 degrees, cannot cusp at its widest, and spans <= 1/8 segment.
static std::vector<Piece> pieces(const Piece& seg, const std::vector<double>& cuts, const Width& w) {
    std::vector<Piece> parts{seg}, out;
    for (double t : cuts)
        for (size_t i = 0; i < parts.size(); i++)
            if (parts[i].t0 < t && t < parts[i].t1) {
                auto [a, b] = cutAt(parts[i], (t - parts[i].t0) / (parts[i].t1 - parts[i].t0));
                parts[i] = a;
                parts.insert(parts.begin() + i + 1, b);
                break;
            }
    std::function<void(const Piece&, int)> rec = [&](const Piece& p, int depth) {
        double r = std::max(w(p.t0), w(p.t1));
        if (depth >= MAX_DEPTH || (turn(p.c) < MAX_TURN && safe(p.c, r) && p.t1 - p.t0 <= MAX_DT)) { out.push_back(p); return; }
        auto [a, b] = cutAt(p, 0.5);
        rec(a, depth + 1);
        rec(b, depth + 1);
    };
    for (auto& p : parts) rec(p, 0);
    return out;
}

// Offset at a distance going from d0 to d1 with slopes dd0, dd1 in the piece's own parameter: the cubic Hermite matching the true variable
// offset C + d*N in position and derivative at both ends. The offset tangent is not the skeleton tangent where the width changes.
static Cubic offsetVar(const Cubic& c, double d0, double d1, double dd0, double dd1) {
    if (isLine(c)) {
        QPointF n = normal(c[3] - c[0]), a = c[0] + n * d0, b = c[3] + n * d1;
        return {a, a, b, b};
    }
    auto end = [&](int u, double d, double dd) {
        QPointF at = u ? c[3] : c[0], t = tangent(c, u), n = normal(t);
        double speed = 3 * len(at - (u ? c[2] : c[1]));  // |C'(u)|, a degenerate handle falling back to the chord
        if (speed == 0) speed = 3 * len(c[3] - c[0]);
        return std::pair{at + n * d, (t * (speed * (1 - d * curvatureAt(c, u))) + n * dd) / 3};
    };
    auto [a0, v0] = end(0, d0, dd0);
    auto [a3, v1] = end(1, d1, dd1);
    return {a0, a0 + v0, a3 - v1, a3};
}

static Cubic reversed(const Cubic& c) { return {c[3], c[2], c[1], c[0]}; }
static Cubic lineTo(QPointF a, QPointF b) { return {a, a, b, b}; }

// One closed contour: left offsets forward, a line across, right offsets backward, a line across.
static std::vector<Cubic> body(const std::vector<Piece>& ps, const Width& w) {
    std::vector<Cubic> left, right;
    for (auto& p : ps) {
        // d and its slope at both ends, one-sided so a knot's kink in w stays a kink
        double h = (p.t1 - p.t0) * 1e-4, d0 = w(p.t0), d1 = w(p.t1), s0 = (w(p.t0 + h) - d0) / 1e-4, s1 = (d1 - w(p.t1 - h)) / 1e-4;
        left.push_back(offsetVar(p.c, d0, d1, s0, s1));
        right.push_back(offsetVar(p.c, -d0, -d1, -s0, -s1));
    }
    std::vector<Cubic> out = left;
    out.push_back(lineTo(left.back()[3], right.back()[3]));
    for (size_t i = right.size(); i-- > 0;) out.push_back(reversed(right[i]));
    out.push_back(lineTo(right[0][0], left[0][0]));
    return out;
}

std::vector<std::vector<Cubic>> powerStrokeBodies(const std::vector<Cubic>& skeleton, bool closed, const std::vector<WidthKnot>& knots) {
    std::vector<std::vector<Cubic>> out;
    if (knots.empty()) return out;
    auto spans = widthSpans(knots, skeleton.size(), closed);
    Width w = [&](double t) { return offsetAt(spans, t); };
    std::vector<double> cuts;  // every span end, sorted, once
    for (auto& s : spans) { cuts.push_back(s[0].x()); cuts.push_back(s[3].x()); }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (size_t i = 0; i < skeleton.size(); i++) {
        const Cubic& c = skeleton[i];
        if (std::all_of(c.begin(), c.end(), [&](QPointF p) { return len(p - c[0]) < EPS; })) continue;  // a point, not a segment
        std::vector<double> inside;
        for (double t : cuts) if (i < t && t < i + 1) inside.push_back(t);
        out.push_back(body(pieces({c, double(i), double(i + 1)}, inside, w), w));
    }
    return out;
}

// A closed contour as a path, wound counter-clockwise (positive area) so overlapping pieces add under winding fill.
static QPainterPath oriented(QPainterPath p) {
    p.setFillRule(Qt::WindingFill);
    QPolygonF poly = p.toFillPolygon();
    double area = 0;
    for (int i = 0; i < poly.size(); i++) area += cross(poly[i], poly[(i + 1) % poly.size()]);
    return area < 0 ? p.toReversed() : p;
}

QPainterPath expandPowerStroke(const std::vector<Cubic>& skeleton, bool closed, const std::vector<WidthKnot>& knots) {
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    if (knots.empty()) return out;
    for (auto& body : powerStrokeBodies(skeleton, closed, knots)) {
        QPainterPath p(body[0][0]);
        for (auto& c : body) p.cubicTo(c[1], c[2], c[3]);
        p.closeSubpath();
        out = out.united(oriented(p));
    }
    // Round joins between neighbouring segments (a point-like segment is skipped, as in the bodies). Zero caps add nothing.
    std::vector<size_t> kept;
    for (size_t i = 0; i < skeleton.size(); i++)
        if (!std::all_of(skeleton[i].begin(), skeleton[i].end(), [&](QPointF p) { return len(p - skeleton[i][0]) < EPS; })) kept.push_back(i);
    auto spans = widthSpans(knots, skeleton.size(), closed);
    for (size_t k = 0; k + (closed ? 0 : 1) < kept.size(); k++) {
        const Cubic &a = skeleton[kept[k]], &b = skeleton[kept[(k + 1) % kept.size()]];
        QPointF ta = tangent(a, 1), tb = tangent(b, 0);
        double r = offsetAt(spans, kept[k] + 1);
        if (r <= EPS || (std::abs(cross(ta, tb)) < 1e-9 && QPointF::dotProduct(ta, tb) > 0)) continue;  // no width, or smooth
        QPainterPath circle;
        circle.addEllipse(a[3], r, r);
        out = out.united(oriented(circle));
    }
    return out;
}
