#pragma once
#include <QPainterPath>
#include <optional>
#include <vector>

// A Spiro Node's type: Corner is G0, G2 and G4 are smooth, Left/Right are one-way (curve on one side only).
enum class SpiroType : char { Corner = 'v', G4 = 'o', G2 = 'c', Left = '[', Right = ']' };
struct SpiroNode { double x, y; SpiroType type; };

// Cubic Bezier path through the nodes (Levien's Spiro solver). Open paths ignore the end nodes' types.
// nullopt when there is no valid curve: fewer than 2 nodes, coincident neighbours, or the solver does not converge.
std::optional<QPainterPath> spiroToPath(const std::vector<SpiroNode>& nodes, bool closed);
