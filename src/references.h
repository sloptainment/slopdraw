#pragma once
#include <QPainter>
#include <QTransform>
#include <vector>
#include "document.h"

// Draws refs (last on top) into p, document coordinates mapped by view (#537). Each is drawn from the smallest
// level of its power-of-two pyramid still at least the on-screen scale, clipped to its crop, at its opacity.
void drawReferences(QPainter& p, const std::vector<Reference>& refs, const QTransform& view);
// An imported picture's transform (#539): image pixels 1:1 to document pixels, scaled down if it would not fit inside a
// viewSize widget under view, centred on the widget's centre.
QTransform placeReference(QSize image, QSize viewSize, const QTransform& view);
