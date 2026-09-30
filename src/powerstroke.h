#pragma once
#include <QPainterPath>
#include <QPointF>
#include <array>
#include <vector>

// A Width Knot: path time t (segment index + fraction) and the FULL width there.
struct WidthKnot { double t, width; };
using Cubic = std::array<QPointF, 4>;

// The interpolated (path time, half-width) curve as cubic spans, x increasing span to span. Cubic interpolator, zero caps:
// an open path of n segments gets zero-width ends at t=0 and t=n, a closed one wraps its neighbours around.
std::vector<Cubic> widthSpans(std::vector<WidthKnot> knots, int n, bool closed);
// Half width at path time t.
double offsetAt(const std::vector<Cubic>& spans, double t);
// Full width at path time t.
double widthAt(const std::vector<WidthKnot>& knots, double t, int n, bool closed);
// One closed body contour per skeleton segment (lines as p1=p0, p2=p3; n = skeleton.size(), path time t = segment index + fraction):
// left offsets forward, a line across, right offsets back, a line across. Joins, caps and the union are not here. Empty without knots.
std::vector<std::vector<Cubic>> powerStrokeBodies(const std::vector<Cubic>& skeleton, bool closed, const std::vector<WidthKnot>& knots);
// The filled outline of the whole stroke: bodies, round joins at every corner, zero caps (add nothing), united by QPainterPath::united.
// Same inputs as powerStrokeBodies; an empty path without knots.
QPainterPath expandPowerStroke(const std::vector<Cubic>& skeleton, bool closed, const std::vector<WidthKnot>& knots);
