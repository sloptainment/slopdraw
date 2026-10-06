#pragma once
#include <QString>
#include <optional>
#include "document.h"

// Writes doc as an OpenRaster file: mimetype (stored, first), stack.xml,
// data/layerN.png per layer, mergedimage.png, Thumbnails/thumbnail.png.
bool writeOra(const Document &doc, const QString &path);
// A fresh Document (empty undo history) with the file's layers, order, names,
// visibility, opacity and pixels; nullopt if unreadable.
std::optional<Document> readOra(const QString &path);
// Writes doc's visible composite (render()) as a PNG.
bool exportPng(const Document &doc, const QString &path);
// The Vector Paths in one layer's data/layerN.svg; missing
// mydraw:kind/mode/fillcolor read as Spiro, Stroke, black.
std::vector<Document::VPath> readVectorSvg(const QByteArray &data);
