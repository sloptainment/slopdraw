#pragma once
#include <QPainter>
#include <QTransform>
#include <optional>
#include <set>
#include <vector>
#include "document.h"

// Draws refs (last on top) into p, document coordinates mapped by view (#537). Each
// is drawn from the smallest level of its power-of-two pyramid still at least the
// on-screen scale, clipped to its crop, at its opacity.
void drawReferences(
  QPainter &p, const std::vector<Reference> &refs, const QTransform &view);
// An imported picture's transform (#539): image pixels 1:1 to document pixels,
// scaled down if it would not fit inside a viewSize widget under view, centred on
// the widget's centre.
QTransform placeReference(QSize image, QSize viewSize, const QTransform &view);
// The topmost reference whose cropped area holds the document point p, -1 for none
// (#542).
int referenceAt(const std::vector<Reference> &refs, QPointF p);
// Color pick (#541): the document point p's color. With showReferences, the topmost
// reference under p gives its picture pixel at full strength (opacity ignored, even
// beyond the document edge); else doc.colorAt, invalid off the document.
QColor pickColorAt(const Document &doc, QPointF p, bool showReferences);
// The references whose cropped area touches box (#542).
std::set<int> referencesIn(const std::vector<Reference> &refs, const QRectF &box);
// The union of the picked references' cropped areas, in document coordinates: the
// Transform box.
QRectF
referencesBounds(const std::vector<Reference> &refs, const std::set<int> &picks);
// refs with each picked one's placement followed by m.
std::vector<Reference> transformReferences(
  std::vector<Reference> refs, const std::set<int> &picks, const QTransform &m);
// refs with the picked ones moved to the top (front) or bottom, their relative order kept (#545).
std::vector<Reference> reorderReferences(
  const std::vector<Reference> &refs, const std::set<int> &picks, bool front);
// Crop mode (#546), in image pixels: the crop edge or corner (hx, hy each -1, 0 or
// 1) within tol of p, or none.
std::optional<QPoint> cropEdgeAt(const QRectF &crop, QPointF p, double tol);
// crop with edge h moved to p, kept inside the picture and at least 1 px wide and
// high.
QRectF cropTo(QRectF crop, QSize image, QPoint h, QPointF p);
