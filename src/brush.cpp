#include "brush.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

std::shared_ptr<MyPaintBrush> loadBrush(const QString& mybPath) {
    QFile f(mybPath);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    std::shared_ptr<MyPaintBrush> b(mypaint_brush_new(), mypaint_brush_unref);
    if (!mypaint_brush_from_string(b.get(), f.readAll().constData())) return nullptr;
    return b;
}

std::vector<BrushEntry> listBrushes(const QString& dir) {
    std::vector<BrushEntry> out;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"*.myb"}, QDir::Files))
        out.push_back({fi.completeBaseName(), fi.filePath()});
    std::sort(out.begin(), out.end(), [](const BrushEntry& a, const BrushEntry& b) { return a.name < b.name; });  // not QDir::Name: that sorts with ".myb" included
    return out;
}

namespace {

// A MyPaintSurface over a QImage; the layer is opaque, so alpha_eraser, lock_alpha and colorize are ignored.
// eraser blends dabs toward white instead of their color (#79; transparent once #44's layers exist).
struct ImageSurface : MyPaintSurface {
    QImage* img;
    bool eraser;
};

// Calls fn(pixel, opacity) for each pixel the dab covers, with libmypaint's hardness falloff (brushmodes.c).
template <class Fn>
void forEachDabPixel(QImage& img, float x, float y, float radius, float hardness, float aspect, float angle, Fn fn) {
    hardness = std::clamp(hardness, 0.f, 0.999f);
    const float sn = std::sin(angle), cs = std::cos(angle), inv_r2 = 1 / (radius * radius);
    const float slope1 = -(1 / hardness - 1), off2 = hardness / (1 - hardness), slope2 = -hardness / (1 - hardness);
    const int x0 = std::max(0, int(std::floor(x - radius * aspect - 1))), x1 = std::min(img.width() - 1, int(std::ceil(x + radius * aspect + 1)));
    const int y0 = std::max(0, int(std::floor(y - radius * aspect - 1))), y1 = std::min(img.height() - 1, int(std::ceil(y + radius * aspect + 1)));
    for (int py = y0; py <= y1; py++) {
        uchar* row = img.scanLine(py);
        for (int px = x0; px <= x1; px++) {
            const float yy = py + 0.5f - y, xx = px + 0.5f - x;
            const float yr = (yy * cs - xx * sn) * aspect, xr = yy * sn + xx * cs;
            const float rr = (yr * yr + xr * xr) * inv_r2;
            if (rr > 1) continue;
            fn(row + 4 * px, rr <= hardness ? 1 + rr * slope1 : off2 + rr * slope2);
        }
    }
}

int drawDab(MyPaintSurface* s, float x, float y, float radius, float r, float g, float b, float opaque, float hardness,
            float, float aspect, float angle, float, float) {
    const bool eraser = static_cast<ImageSurface*>(s)->eraser;
    const float rgb[3] = {eraser ? 1 : r, eraser ? 1 : g, eraser ? 1 : b};
    forEachDabPixel(*static_cast<ImageSurface*>(s)->img, x, y, radius, hardness, aspect, angle, [&](uchar* p, float opa) {
        const float a = opa * opaque;
        for (int c = 0; c < 3; c++) p[c] = uchar(std::lround(a * rgb[c] * 255 + (1 - a) * p[c]));
    });
    return 1;
}

void getColor(MyPaintSurface* s, float x, float y, float radius, float* r, float* g, float* b, float* a) {
    float sum[3] = {}, w = 0;
    forEachDabPixel(*static_cast<ImageSurface*>(s)->img, x, y, radius, 0.5f, 1, 0, [&](uchar* p, float opa) {
        for (int c = 0; c < 3; c++) sum[c] += opa * p[c] / 255.f;
        w += opa;
    });
    *r = w ? sum[0] / w : 1; *g = w ? sum[1] / w : 1; *b = w ? sum[2] / w : 1; *a = 1;
}

}  // namespace

void paintStroke(MyPaintBrush* brush, QImage& layer, const std::vector<PenSample>& samples, bool continuing, bool eraser) {
    ImageSurface s;
    s.draw_dab = drawDab;
    s.get_color = getColor;
    s.begin_atomic = [](MyPaintSurface*) {};
    s.end_atomic = [](MyPaintSurface*, MyPaintRectangle*) {};
    s.destroy = nullptr;
    s.save_png = nullptr;
    s.refcount = 1;
    s.img = &layer;
    s.eraser = eraser;
    if (!continuing) {
        mypaint_brush_reset(brush);
        mypaint_brush_new_stroke(brush);
    }
    for (const auto& p : samples) mypaint_brush_stroke_to(brush, &s, p.x, p.y, p.pressure, p.xtilt, p.ytilt, p.dt);
}
