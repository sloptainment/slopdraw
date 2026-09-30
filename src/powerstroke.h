#pragma once
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
