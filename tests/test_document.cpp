#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "brush.h"
#include "miniz.h"
#include "document.h"
#include "ora.h"
#include <QTemporaryDir>
#include "powerstroke.h"
#include "spiro.h"
#include "spiropen.h"
#include "tools.h"
#include "powerstroke_fixtures.h"
#include "powerstroke_body_fixtures.h"
#include "spiro_fixtures.h"
#include "view.h"

// New Document renders white, matching a golden PNG.
static bool newDocumentRendersWhite() {
    QImage golden(GOLDEN_DIR "/new_64x64.pbm");
    if (golden.isNull()) return false;
    QImage got = Document(QSize(64, 64)).render();
    return got.convertToFormat(QImage::Format_RGB32) == golden.convertToFormat(QImage::Format_RGB32);
}

static bool near(QPointF a, QPointF b) { return std::abs(a.x() - b.x()) < 1e-6 && std::abs(a.y() - b.y()) < 1e-6; }

// A point under pan + zoom + rotate + mirror maps to a known view spot and back to the same document spot.
static bool viewMapsAndRoundTrips() {
    View v;
    v.pan({10, 20});                 // doc (5,5) -> view (15,25)
    v.zoomAt({0, 0}, 2);             // -> (30,50)
    v.rotateAt({0, 0}, 90);          // y-down, clockwise: (x,y) -> (-y,x) = (-50,30)
    v.mirrorAt({0, 0});              // flips view x: (50,30)
    QPointF doc(5, 5);
    return near(v.toView(doc), {50, 30}) && near(v.toDoc({50, 30}), doc);
}

// Zooming, rotating and mirroring about an anchor leave the anchor's document point under it.
static bool viewOpsKeepAnchorFixed() {
    View v;
    v.pan({-7, 13});
    QPointF a(100, 80), under = v.toDoc(a);
    v.zoomAt(a, 3);
    v.rotateAt(a, 37);
    v.mirrorAt(a);
    return near(v.toDoc(a), under);
}

// A brush radius on screen scales with zoom only: rotation and mirror leave it unchanged.
static bool viewScreenRadiusFollowsZoomOnly() {
    View v;
    bool ok = std::abs(v.screenRadius(10) - 10) < 1e-9;
    v.zoomAt({0, 0}, 2);
    ok = ok && std::abs(v.screenRadius(10) - 20) < 1e-9;
    v.rotateAt({3, 4}, 37);
    v.mirrorAt({5, 6});
    return ok && std::abs(v.screenRadius(10) - 20) < 1e-9;
}

// Each fixture node list becomes the same Beziers as the reference spiro.c, within 1e-6.
static bool spiroMatchesReference() {
    bool ok = true;
    for (const auto& f : kSpiroFixtures) {
        auto path = spiroToPath(f.nodes, f.closed);
        bool same = path && size_t(path->elementCount()) == [&] {  // a curve is 3 elements, a line or move 1
            size_t n = 0;
            for (auto& r : f.out) n += r[0] == 2 ? 3 : 1;
            return n;
        }();
        for (int i = 0, row = 0; same && i < path->elementCount(); row++) {
            auto& r = f.out[row];
            int k = r[0] == 2 ? 3 : 1;
            for (int j = 0; j < k; j++)
                same = same && near(path->elementAt(i + j), {r[1 + 2 * j], r[2 + 2 * j]});
            i += k;
        }
        if (!same) std::printf("  mismatch: %s\n", f.name);
        ok = ok && same;
    }
    return ok;
}

// Coincident nodes and a path the solver cannot fit give no curve, rather than NaN Beziers.
static bool spiroFailsInsteadOfGarbage() {
    using T = SpiroType;
    bool fine = spiroToPath({{0, 0, T::G2}, {50, 50, T::G2}, {60, 50, T::G2}, {100, 0, T::G2}}, false).has_value();
    bool coincident = !spiroToPath({{0, 0, T::G2}, {50, 50, T::G2}, {50, 50, T::G2}, {100, 0, T::G2}}, false);
    bool hairpin = !spiroToPath({{0, 0, T::G2}, {100, 0, T::G2}, {0, 5, T::G2}, {100, 10, T::G2}, {0, 15, T::G2}, {100, 20, T::G2}}, false);
    return fine && coincident && hairpin;
}

// The bundled pencil.myb loads, and its "opaque" setting reads back the file's 0.7.
static bool bundledPencilLoads() {
    auto b = loadBrush(BRUSH_DIR "/pencil.myb");
    return b && mypaint_brush_get_base_value(b.get(), MYPAINT_BRUSH_SETTING_OPAQUE) == 0.7f;
}

// The bundled brush list includes pencil, is sorted by name, and every entry loads.
static bool bundledBrushesListAndLoad() {
    auto list = listBrushes(BRUSH_DIR);
    bool hasPencil = false;
    for (size_t i = 0; i < list.size(); i++) {
        hasPencil |= list[i].name == "pencil";
        if (i && !(list[i - 1].name < list[i].name)) return false;
        if (!loadBrush(list[i].path)) return false;
    }
    return hasPencil && list.size() > 1;
}

// A recorded pen-sample list (pressure ramping up along a wave) replayed with the pencil renders as the golden.
static bool replayedPencilStrokeMatchesGolden() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    std::vector<PenSample> samples;
    for (int i = 0; i < 16; i++)
        samples.push_back({4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01});
    Document doc(QSize(64, 64));
    doc.stroke(pencil, samples);
    QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
    QImage golden(GOLDEN_DIR "/pencil_wave_64x64.pgm");
    bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
    if (!same) {  // write plain-text PGM (Qt only writes binary) to eyeball and copy into golden/
        if (FILE* f = std::fopen("actual_pencil_wave.pgm", "w")) {
            std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
            for (int y = 0; y < got.height(); y++)
                for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
            std::fclose(f);
        }
    }
    return same;
}

// The same samples fed one call at a time (as live pen events arrive) render as the same golden.
static bool liveFedPencilStrokeMatchesGolden() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    for (int i = 0; i < 16; i++)
        doc.stroke(pencil, {{4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01}}, i > 0);
    QImage golden(GOLDEN_DIR "/pencil_wave_64x64.pgm");
    return !golden.isNull() && doc.render().convertToFormat(QImage::Format_Grayscale8) == golden.convertToFormat(QImage::Format_Grayscale8);
}

// A wavy stroke across row y with pressure ramping up, so different y give strokes that touch different pixels.
static std::vector<PenSample> waveAt(float y) {
    std::vector<PenSample> samples;
    for (int i = 0; i < 16; i++) samples.push_back({4.f + 3.8f * i, y + 6.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01});
    return samples;
}

// Two strokes, undo twice gives the blank layer; redo brings back the first stroke, redo again the second.
static bool undoRestoresAndRedoReapplies() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    QImage blank = doc.render().copy();
    doc.stroke(pencil, waveAt(16));
    QImage first = doc.render().copy();
    doc.stroke(pencil, waveAt(48));
    QImage second = doc.render().copy();
    if (first == blank || second == first) return false;  // the strokes must have painted something
    bool a = doc.undo() && doc.render() == first;
    bool b = doc.undo() && doc.render() == blank;
    bool c = !doc.undo();
    bool d = doc.redo() && doc.render() == first;
    bool e = doc.redo() && doc.render() == second;
    bool f = !doc.redo();
    return a && b && c && d && e && f;
}

// A stroke after an undo replaces the undone one: redo has nothing, and undo goes back through the new stroke.
static bool newStrokeAfterUndoDiscardsRedo() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    QImage blank = doc.render().copy();
    doc.stroke(pencil, waveAt(16));
    doc.undo();
    doc.stroke(pencil, waveAt(48));
    QImage second = doc.render().copy();
    bool noRedo = !doc.redo() && doc.render() == second;
    bool back = doc.undo() && doc.render() == blank && !doc.undo();
    return noRedo && back;
}

// Strokes fed one sample per call (live pen input) are still one undo step.
static bool liveFedStrokeIsOneUndoStep() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    QImage blank = doc.render().copy();
    auto samples = waveAt(32);
    for (size_t i = 0; i < samples.size(); i++) doc.stroke(pencil, {samples[i]}, i > 0);
    return doc.render() != blank && doc.undo() && doc.render() == blank && !doc.undo();
}

// With a budget of one byte only the newest step survives: undo reaches the state before it, and no further.
static bool budgetDropsOldestKeepsNewest() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64), 1);
    doc.stroke(pencil, waveAt(16));
    QImage first = doc.render().copy();
    doc.stroke(pencil, waveAt(48));
    return doc.undo() && doc.render() == first && !doc.undo();
}

// After the pencil wave, colorAt reads the golden's gray at a painted pixel (53,34), white elsewhere, and invalid off the canvas.
static bool colorAtReadsCompositePixel() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    for (int i = 0; i < 16; i++)  // the golden's samples, as in replayedPencilStrokeMatchesGolden
        doc.stroke(pencil, {{4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01}}, i > 0);
    QColor painted = doc.colorAt({53, 34}), white = doc.colorAt({0, 0});
    return painted == QColor(137, 137, 137, 255) && white == QColor(255, 255, 255, 255) &&
           !doc.colorAt({-1, 0}).isValid() && !doc.colorAt({64, 0}).isValid() && !doc.colorAt({0, 64}).isValid();
}

// Replaying the pencil wave's samples with the eraser flag lightens the painted stroke (golden), and undo restores the painted-only image.
static bool eraserStrokeErasesAndUndoes() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    std::vector<PenSample> samples;  // the golden's samples, as in replayedPencilStrokeMatchesGolden
    for (int i = 0; i < 16; i++)
        samples.push_back({4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01});
    Document doc(QSize(64, 64));
    doc.stroke(pencil, samples);
    QImage painted = doc.render().copy();
    doc.stroke(pencil, samples, false, true);
    QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
    QImage golden(GOLDEN_DIR "/pencil_wave_erased_64x64.pgm");
    bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
    if (!same) {  // write plain-text PGM to eyeball and copy into golden/
        if (FILE* f = std::fopen("actual_pencil_wave_erased.pgm", "w")) {
            std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
            for (int y = 0; y < got.height(); y++)
                for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
            std::fclose(f);
        }
    }
    bool lighter = doc.colorAt({53, 34}).red() > 137;  // the pixel colorAtReadsCompositePixel finds at 137 after painting
    return same && lighter && doc.undo() && doc.render() == painted;
}

// The pencil wave painted after setBrushColor(red) is red at (53,34), where the default gray left 137.
static bool brushColorPaintsInThatColor() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    setBrushColor(pencil.get(), Qt::red);
    Document doc(QSize(64, 64));
    for (int i = 0; i < 16; i++)  // the golden's samples, as in colorAtReadsCompositePixel
        doc.stroke(pencil, {{4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01}}, i > 0);
    QColor c = doc.colorAt({53, 34});
    return c == QColor(255, 137, 137, 255);
}

// The pencil wave painted with a larger radius covers more pixels, and with lower opacity is lighter at (53,34), than the default.
static bool brushRadiusAndOpacityChangeTheStroke() {
    auto render = [](float radius, float opacity) {
        auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
        if (radius) setBrushRadius(pencil.get(), radius);
        if (opacity) setBrushOpacity(pencil.get(), opacity);
        Document doc(QSize(64, 64));
        for (int i = 0; i < 16; i++)  // the golden's samples, as in brushColorPaintsInThatColor
            doc.stroke(pencil, {{4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01}}, i > 0);
        return doc.render();
    };
    auto inked = [](const QImage& im) {
        int n = 0;
        for (int y = 0; y < im.height(); y++)
            for (int x = 0; x < im.width(); x++) n += im.pixelColor(x, y) != QColor(Qt::white);
        return n;
    };
    QImage base = render(0, 0), big = render(4, 0), faint = render(0, 0.2f);
    return inked(big) > inked(base) && faint.pixelColor(53, 34).value() > base.pixelColor(53, 34).value();
}

// Fed as Canvas does (first sample continuing=false, dt 0), a second stroke far from the first leaves the pixels between them white, and is its own undo step.
static bool secondStrokeDoesNotJoinFirst() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    auto live = [&](float x0, float y0) {
        for (int i = 0; i < 5; i++) doc.stroke(pencil, {{x0 + 1.f * i, y0 + 1.f * i, 0.5f, 0, 0, i ? 0.01 : 0.0}}, i > 0);
    };
    live(10, 10);
    QImage first = doc.render().copy();
    live(50, 50);
    QImage both = doc.render();
    for (int t = 23; t <= 41; t++)  // the line from (14,14) to (50,50), clear of both strokes' dabs
        if (both.pixelColor(t, t) != QColor(Qt::white)) return false;
    return doc.undo() && doc.render() == first;
}

// Width at sampled path times matches slopfont's widthAt, including knots only at the ends (uniform between tapered zero caps).
static bool widthAtMatchesSlopfont() {
    for (const auto& f : kWidthFixtures)
        for (const auto& row : f.out)
            if (std::abs(widthAt(f.knots, row[0], f.n, f.closed) - row[1]) > 1e-6) return false;
    return true;
}

// Pre-union body contours per skeleton segment match slopfont's (straight, corner, curved, closed), edge control points within 1e-6.
static bool powerStrokeBodiesMatchSlopfont() {
    for (const auto& f : kBodyFixtures) {
        auto got = powerStrokeBodies(f.skeleton, f.closed, f.knots);
        if (got.size() != f.bodies.size()) return false;
        for (size_t i = 0; i < got.size(); i++) {
            if (got[i].size() != f.bodies[i].size()) return false;
            for (size_t j = 0; j < got[i].size(); j++)
                for (int k = 0; k < 4; k++)
                    if (std::hypot(got[i][j][k].x() - f.bodies[i][j][k].x(), got[i][j][k].y() - f.bodies[i][j][k].y()) > 1e-6) return false;
        }
    }
    return true;
}

// Knots only at the ends: a uniform 10-wide stroke; zero caps add nothing, so it ends flat at both ends (widthAt is 10 at t=0, per the fixtures).
static bool endKnotsGiveUniformStrokeWithTaperedCaps() {
    auto out = expandPowerStroke({Cubic{{{0, 0}, {0, 0}, {100, 0}, {100, 0}}}}, false, {{0, 10}, {1, 10}});
    return out.contains(QPointF(50, 4.9)) && out.contains(QPointF(50, -4.9)) && !out.contains(QPointF(50, 5.1)) && !out.contains(QPointF(50, -5.1)) &&
           out.contains(QPointF(0.5, 4)) && out.contains(QPointF(99.5, -4)) && !out.contains(QPointF(-0.5, 0)) && !out.contains(QPointF(100.5, 0));
}

// A corner gets a round join of the half-width there: (63,-3) lies only in the join, (65,-5) is past its radius 6.
static bool cornerGetsRoundJoin() {
    auto out = expandPowerStroke({Cubic{{{0, 0}, {0, 0}, {60, 0}, {60, 0}}}, Cubic{{{60, 0}, {60, 0}, {60, 50}, {60, 50}}}}, false, {{1, 12}});
    return out.contains(QPointF(63, -3)) && !out.contains(QPointF(65, -5)) && out.contains(QPointF(30, 0)) && !out.contains(QPointF(30, -8));
}

// A closed skeleton has no caps and its wrap-around corner is joined too: the square's start corner is filled.
static bool closedSkeletonJoinsWrapAround() {
    Cubic a{{{0, 0}, {0, 0}, {50, 0}, {50, 0}}}, b{{{50, 0}, {50, 0}, {50, 50}, {50, 50}}}, c{{{50, 50}, {50, 50}, {0, 50}, {0, 50}}}, d{{{0, 50}, {0, 50}, {0, 0}, {0, 0}}};
    auto out = expandPowerStroke({a, b, c, d}, true, {{0, 10}});
    return out.contains(QPointF(-3, -3)) && out.contains(QPointF(53, 53)) && !out.contains(QPointF(25, 25)) && !out.contains(QPointF(-4, -4));
}

// Dragging right grows the radius exponentially (100 px = x e), left shrinks it, and the result stays within the size slider's 1..200.
static bool dragResizesRadiusWithinRange() {
    return std::fabs(dragRadius(10, 100) - 27.1828f) < 1e-3 && std::fabs(dragRadius(10, -100) - 3.6788f) < 1e-3 && dragRadius(10, 0) == 10
        && dragRadius(10, 1000) == 200 && dragRadius(10, -1000) == 1;
}

// A figure-eight point list fills both loops solid black, antialiased, matching a golden; erase mode cuts a square out of it
// (golden); undo restores each; a pentagram's doubly-wound center is filled (non-zero, not even-odd).
static bool fillsFigureEightAndErases() {
    auto grayGolden = [](const Document& doc, const char* name, const char* actual) {
        QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
        QImage golden(QString(GOLDEN_DIR "/") + name);
        bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
        if (!same) {  // write plain-text PGM to eyeball and copy into golden/
            if (FILE* f = std::fopen(actual, "w")) {
                std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
                for (int y = 0; y < got.height(); y++)
                    for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
                std::fclose(f);
            }
        }
        return same;
    };
    const std::vector<QPointF> eight = {{8, 8}, {56, 56}, {56, 8}, {8, 56}};
    Document doc(QSize(64, 64));
    QImage blank = doc.render().copy();
    doc.fill(Qt::black, eight);
    QImage filled = doc.render().copy();
    bool loops = doc.colorAt({50, 32}) == Qt::black && doc.colorAt({14, 32}) == Qt::black && doc.colorAt({32, 8}) == Qt::white;
    int edge = doc.colorAt({20, 20}).red();  // on the 45-degree edge: partial coverage
    bool same = grayGolden(doc, "fill_figure_eight_64x64.pgm", "actual_fill_figure_eight.pgm");
    doc.fill(Qt::black, {{24, 24}, {40, 24}, {40, 40}, {24, 40}}, true);  // cuts a square out
    QImage cut = doc.render().copy();
    bool erased = doc.colorAt({32, 30}) == Qt::white && doc.colorAt({50, 32}) == Qt::black && grayGolden(doc, "fill_figure_eight_erased_64x64.pgm", "actual_fill_figure_eight_erased.pgm");
    bool undone = doc.undo() && doc.render() == filled && doc.redo() && doc.render() == cut && doc.undo() && doc.undo() && doc.render() == blank;
    Document star(QSize(64, 64));
    star.fill(Qt::black, {{32, 4}, {46, 58}, {6, 22}, {58, 22}, {18, 58}});
    return loops && edge > 0 && edge < 255 && same && erased && undone && star.colorAt({32, 32}) == Qt::black;
}

// A fill at a colour's alpha lands that translucent, and erase mode at an alpha moves pixels part-way to white (#212).
static bool fillHonoursAlpha() {
    const std::vector<QPointF> square = {{8, 8}, {56, 8}, {56, 56}, {8, 56}};
    Document doc(QSize(64, 64));
    doc.fill(QColor(0, 0, 0, 128), square);
    int half = doc.colorAt({32, 32}).red();  // black at 50% over white
    Document black(QSize(64, 64));
    black.addLayer();
    black.fill(Qt::black, {{0, 0}, {64, 0}, {64, 64}, {0, 64}});
    black.fill(QColor(255, 255, 255, 128), square, true);
    int grey = black.colorAt({32, 32}).red();  // moved halfway toward white
    black.fill(QColor(255, 255, 255, 255), square, true);
    return half > 100 && half < 156 && grey > 100 && grey < 156 && black.colorAt({32, 32}) == Qt::white && black.undo() && black.colorAt({32, 32}).red() == grey;
}

// A stroke on an added layer lands on it alone: hiding it leaves the blank white render, showing it reads the single-layer golden's gray at (53,34).
static bool strokeLandsOnCurrentLayerOnly() {
    auto pencil = loadBrush(BRUSH_DIR "/pencil.myb");
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    QImage blank = doc.render().copy();
    doc.addLayer();
    for (int i = 0; i < 16; i++)  // the golden's samples, as in colorAtReadsCompositePixel
        doc.stroke(pencil, {{4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01}}, i > 0);
    int shown = doc.colorAt({53, 34}).red();
    doc.setVisible(1, false);
    bool hidden = doc.render() == blank;
    doc.setVisible(1, true);
    doc.setCurrent(0);
    doc.fill(Qt::black, {{0, 0}, {8, 0}, {8, 8}, {0, 8}});  // lands on the background: still there with layer 1 hidden
    doc.setVisible(1, false);
    return std::abs(shown - 137) <= 2 && hidden && doc.colorAt({4, 4}) == Qt::black;
}

// Background, then a 50% layer with a black square (8,8)-(40,40), a hidden layer with a black square, and a full layer with a black square
// (24,24)-(56,56): 50% gray where only the first shows, black in the overlap, white elsewhere (golden).
static bool compositeOfThreeLayersMatchesGolden() {
    Document doc(QSize(64, 64));
    auto square = [&](double lo, double hi) { doc.fill(Qt::black, {{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}}); };
    doc.addLayer(); square(8, 40); doc.setOpacity(1, 0.5);
    doc.addLayer(); square(0, 64); doc.setVisible(2, false);
    doc.addLayer(); square(24, 56);
    bool pixels = std::abs(doc.colorAt({12, 12}).red() - 128) <= 1 && doc.colorAt({30, 30}) == Qt::black &&
                  doc.colorAt({60, 60}) == Qt::white && doc.colorAt({50, 50}) == Qt::black;
    QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
    QImage golden(GOLDEN_DIR "/layers_composite_64x64.pgm");
    bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
    if (!same) {  // write plain-text PGM to eyeball and copy into golden/
        if (FILE* f = std::fopen("actual_layers_composite.pgm", "w")) {
            std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
            for (int y = 0; y < got.height(); y++)
                for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
            std::fclose(f);
        }
    }
    return pixels && same;
}

// Add, reorder, opacity, visibility and delete each undo and redo, in one history with fills: a red layer under a blue one, every op
// undone back to the white background and redone forward again; a new op after an undo discards the redo branch.
static bool layerOpsUndoAndRedoInOneHistory() {
    Document doc(QSize(64, 64));
    auto full = [&](QColor c) { doc.fill(c, {{0, 0}, {64, 0}, {64, 64}, {0, 64}}); };
    QImage white = doc.render().copy();
    doc.addLayer(); full(Qt::red);
    QImage red = doc.render().copy();
    doc.addLayer(); full(Qt::blue);
    QImage blue = doc.render().copy();
    bool ok = red != blue;
    auto roundTrip = [&](const QImage& after) {  // the op just done gives `after`; undo gives blue; redo gives `after` again
        bool r = doc.render() == after && doc.undo() && doc.render() == blue && doc.redo() && doc.render() == after && doc.undo();
        return r && doc.render() == blue;
    };
    ok = ok && doc.moveLayer(2, 1) && doc.colorAt({5, 5}) == Qt::red && roundTrip(red);  // reorder, then undo restores the order
    doc.setOpacity(2, 0.5);
    QImage half = doc.render().copy();
    ok = ok && half != blue && roundTrip(half);
    doc.setVisible(2, false);
    ok = ok && roundTrip(red);
    ok = ok && doc.deleteLayer(2) && roundTrip(red);
    ok = ok && doc.undo() && doc.undo() && doc.render() == red && doc.undo() && doc.render() == white && doc.undo() && doc.render() == white;  // add 2 gone
    ok = ok && !doc.undo();
    ok = ok && doc.redo() && doc.redo() && doc.render() == red && doc.redo() && doc.redo() && doc.render() == blue && doc.redo() && doc.render() == red && !doc.redo();
    doc.undo();
    doc.setVisible(1, false);  // a new op discards the redo branch
    return ok && !doc.redo();
}

// Layers are named "Background" then "Layer 1", "Layer 2"; setName is one undo step, a same-name call is none, and a deleted layer's name survives undo.
static bool layerNamesAreStoredAndUndoable() {
    Document doc(QSize(8, 8));
    doc.addLayer(); doc.addLayer();
    bool ok = doc.name(0) == "Background" && doc.name(1) == "Layer 1" && doc.name(2) == "Layer 2";
    doc.setName(1, "Ink");
    doc.setName(1, "Ink");
    ok = ok && doc.name(1) == "Ink" && doc.undo() && doc.name(1) == "Layer 1" && doc.redo() && doc.name(1) == "Ink";
    ok = ok && doc.deleteLayer(1) && doc.undo() && doc.name(1) == "Ink" && doc.name(2) == "Layer 2";
    doc.undo(); doc.undo();  // the rename, then one more: the same-name call pushed no step, so this undoes add 2
    return ok && doc.layerCount() == 2 && doc.redo() && doc.name(2) == "Layer 2";
}

// A zip written with two entries reads back with the same names and bytes, through the vendored miniz.
static bool zipRoundTripsTwoEntries() {
    mz_zip_archive w{};
    void* buf = nullptr; size_t size = 0;
    if (!mz_zip_writer_init_heap(&w, 0, 0)) return false;
    bool ok = mz_zip_writer_add_mem(&w, "mimetype", "image/openraster", 16, MZ_NO_COMPRESSION)
           && mz_zip_writer_add_mem(&w, "data/a.bin", "\x00\x01\xff\x00", 4, MZ_DEFAULT_COMPRESSION)
           && mz_zip_writer_finalize_heap_archive(&w, &buf, &size);
    mz_zip_writer_end(&w);
    if (!ok) return false;
    mz_zip_archive r{};
    ok = mz_zip_reader_init_mem(&r, buf, size, 0) && mz_zip_reader_get_num_files(&r) == 2;
    size_t n1 = 0, n2 = 0;
    void* a = ok ? mz_zip_reader_extract_file_to_heap(&r, "mimetype", &n1, 0) : nullptr;
    void* b = ok ? mz_zip_reader_extract_file_to_heap(&r, "data/a.bin", &n2, 0) : nullptr;
    ok = ok && a && b && n1 == 16 && !std::memcmp(a, "image/openraster", 16) && n2 == 4 && !std::memcmp(b, "\x00\x01\xff\x00", 4);
    mz_free(a); mz_free(b); mz_zip_reader_end(&r); mz_free(buf);
    return ok;
}

// A 3-layer Document (names, hidden, half opacity, painted pixels) saved to .ora and read back matches layer by layer and in composite.
static bool oraRoundTripsThreeLayers() {
    Document doc(QSize(64, 64));
    auto square = [&](double lo, double hi) { doc.fill(Qt::black, {{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}}); };
    doc.addLayer(); square(8, 40); doc.setOpacity(1, 0.5); doc.setName(1, "Ink");
    doc.addLayer(); square(24, 56); doc.setVisible(2, false);
    QTemporaryDir dir;
    QString path = dir.filePath("t.ora");
    if (!writeOra(doc, path)) return false;
    auto back = readOra(path);
    if (!back || back->layerCount() != 3) return false;
    for (int i = 0; i < 3; i++)
        if (back->name(i) != doc.name(i) || back->visible(i) != doc.visible(i) || back->opacity(i) != doc.opacity(i) || back->image(i) != doc.image(i))
            return false;
    return back->render() == doc.render() && !back->undo() && !readOra(dir.filePath("missing.ora"));
}

// A Raster and a Vector Layer (two paths: open with every Spiro Node type and edited knots, closed with a translucent color) saved to .ora
// and read back hold the same nodes, types, closed flags, colors, widths and knots, and render the same; the layer's SVG is in the zip.
static bool oraRoundTripsVectorLayer() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addLayer(); doc.fill(Qt::red, {{0, 0}, {30, 0}, {30, 30}, {0, 30}});
    doc.addVectorLayer();
    doc.addPath({{8, 50, T::Corner}, {20, 40, T::G2}, {32, 50, T::G4}, {44, 40, T::Left}, {56, 50, T::Right}}, false, QColor(10, 20, 30), 6);
    doc.setKnots(2, 0, {{0, 6}, {1.5, 2.25}, {4, 6}});
    doc.addPath({{10, 10, T::G2}, {30, 10, T::G2}, {30, 25, T::G2}, {10, 25, T::G2}}, true, QColor(0, 128, 255, 100), 4);
    doc.setOpacity(2, 0.5);
    QTemporaryDir dir;
    QString path = dir.filePath("t.ora");
    if (!writeOra(doc, path)) return false;
    auto back = readOra(path);
    if (!back || back->layerCount() != 3 || back->vector(1) || !back->vector(2) || back->opacity(2) != 0.5) return false;
    if (back->paths(2).size() != 2) return false;
    for (int i = 0; i < 2; i++) {
        const auto &a = doc.paths(2)[i], &b = back->paths(2)[i];
        if (a.nodes.size() != b.nodes.size() || a.closed != b.closed || a.color != b.color || a.width != b.width || a.knots.size() != b.knots.size()) return false;
        for (size_t j = 0; j < a.nodes.size(); j++)
            if (a.nodes[j].x != b.nodes[j].x || a.nodes[j].y != b.nodes[j].y || a.nodes[j].type != b.nodes[j].type) return false;
        for (size_t j = 0; j < a.knots.size(); j++)
            if (a.knots[j].t != b.knots[j].t || a.knots[j].width != b.knots[j].width) return false;
    }
    mz_zip_archive z{};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray zip = f.readAll();
    if (!mz_zip_reader_init_mem(&z, zip.constData(), size_t(zip.size()), 0)) return false;
    size_t n = 0;
    void* svg = mz_zip_reader_extract_file_to_heap(&z, "data/layer2.svg", &n, 0);
    bool hasSvg = svg && QByteArray(static_cast<const char*>(svg), qsizetype(n)).contains("<path");
    mz_free(svg); mz_zip_reader_end(&z);
    return hasSvg && back->render() == doc.render() && back->image(2) == doc.image(2);
}

// Export PNG writes the visible composite: hidden layer left out, half-opacity layer blended, reloaded pixels equal render().
static bool exportedPngEqualsRender() {
    Document doc(QSize(32, 32));
    doc.addLayer(); doc.fill(Qt::black, {{4, 4}, {20, 4}, {20, 20}, {4, 20}}); doc.setOpacity(1, 0.5);
    doc.addLayer(); doc.fill(Qt::red, {{10, 10}, {30, 10}, {30, 30}, {10, 30}}); doc.setVisible(2, false);
    QTemporaryDir dir;
    QString path = dir.filePath("t.png");
    if (!exportPng(doc, path)) return false;
    QImage back(path);
    return !back.isNull() && back.convertToFormat(QImage::Format_ARGB32) == doc.render().convertToFormat(QImage::Format_ARGB32);
}

// A Vector Layer over a red Raster Layer, holding one straight 3-node Spiro path (black, 8 wide, flat ends at x=8 and x=56): the path
// is solid black across its width and red beyond it or its ends, the render matches a golden, and undo removes the path (redo restores it).
static bool vectorPathRendersAsPowerStroke() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addLayer();
    doc.fill(Qt::red, {{0, 0}, {64, 0}, {64, 64}, {0, 64}});
    int v = doc.addVectorLayer();
    bool added = v == 2 && doc.current() == 2 && doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    auto painted = [&] { return doc.colorAt({32, 31}) == Qt::black && doc.colorAt({32, 35}) == Qt::black; };
    auto bare = [&] { return doc.colorAt({32, 31}) == Qt::red && doc.colorAt({32, 35}) == Qt::red; };
    bool onPath = painted() && doc.colorAt({32, 27}) == Qt::red && doc.colorAt({32, 37}) == Qt::red && doc.colorAt({4, 31}) == Qt::red;
    QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
    QImage golden(GOLDEN_DIR "/vector_path_64x64.pgm");
    bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
    if (!same) {  // write plain-text PGM to eyeball and copy into golden/
        if (FILE* f = std::fopen("actual_vector_path.pgm", "w")) {
            std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
            for (int y = 0; y < got.height(); y++)
                for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
            std::fclose(f);
        }
    }
    bool undone = doc.undo() && bare() && doc.layerCount() == 3;
    bool redone = doc.redo() && painted();
    bool layerGone = doc.undo() && doc.undo() && doc.layerCount() == 2 && doc.redo() && doc.redo() && painted();
    doc.addLayer();
    bool notVector = !doc.addPath({{8, 8, T::G2}, {30, 8, T::G2}}, false, Qt::black, 8);  // a Raster Layer takes no paths
    doc.addVectorLayer();
    bool bad = !doc.addPath({{8, 8, T::G2}, {8, 8, T::G2}}, false, Qt::black, 8);  // no curve: refused
    return added && onPath && same && undone && redone && layerGone && notVector && bad;
}

// On the vector-path fixture, setKnots with a moved t and a new width changes the render (golden), undo restores the default render,
// redo the new one; a Raster Layer, a bad path index or an empty knot list changes nothing.
static bool setKnotsRedrawsAndUndoes() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    QImage before = doc.render();
    bool set = doc.setKnots(1, 0, {{0, 4}, {1, 14}});
    QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
    QImage golden(GOLDEN_DIR "/vector_knots_64x64.pgm");
    bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
    if (!same) {  // write plain-text PGM to eyeball and copy into golden/
        if (FILE* f = std::fopen("actual_vector_knots.pgm", "w")) {
            std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
            for (int y = 0; y < got.height(); y++)
                for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
            std::fclose(f);
        }
    }
    QImage changed = doc.render();
    bool differs = changed != before;
    bool undone = doc.undo() && doc.render() == before;
    bool redone = doc.redo() && doc.render() == changed;
    bool refused = !doc.setKnots(0, 0, {{0, 4}}) && !doc.setKnots(1, 1, {{0, 4}}) && !doc.setKnots(1, 0, {}) && doc.render() == changed;
    return set && same && differs && undone && redone && refused;
}

// On a straight 3-node path at y=32 (width 8, knots at both ends): pathAt finds the path for a point on the curve and none off it or
// on a Raster Layer; knotHandles puts each knot half a width to the side of its path point.
static bool selectingAPathShowsItsKnotHandles() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    auto near = [](QPointF a, QPointF b) { return std::abs(a.x() - b.x()) < 1e-6 && std::abs(a.y() - b.y()) < 1e-6; };
    auto h = doc.knotHandles(1, 0);
    bool hit = doc.pathAt(1, {30, 33}, 3) == 0 && doc.pathAt(1, {30, 40}, 3) == -1 && doc.pathAt(0, {30, 32}, 3) == -1;
    bool handles = h.size() == 2 && near(h[0], {8, 36}) && near(h[1], {56, 36});
    return hit && handles;
}

// On a straight 3-node path at y=32 drawn 40 wide: pathAt hits 15 px off the centre line, inside the drawn stroke, with the 3 px band alone missing it.
static bool wideStrokeIsHitInsideItsBody() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 40);
    return doc.pathAt(1, {30, 47}, 3) == 0 && doc.pathAt(1, {30, 60}, 3) == -1;
}

// A closed 4-node square (8 wide, the Spiro curve bulging round it) is hit deep inside it, where the ring is not drawn; a thin open path (width 2) is still hit
// within 3 px of its centre line, and a point clear of both is not.
static bool closedPathIsHitInsideAndThinPathStaysHittable() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{10, 10, T::G2}, {40, 10, T::G2}, {40, 40, T::G2}, {10, 40, T::G2}}, true, Qt::black, 8);
    doc.addPath({{8, 60, T::G2}, {32, 60, T::G2}, {56, 60, T::G2}}, false, Qt::black, 2);
    return doc.pathAt(1, {25, 25}, 3) == 0 && doc.pathAt(1, {30, 62}, 3) == 1 && doc.pathAt(1, {30, 54}, 3) == -1 && doc.pathAt(1, {2, 2}, 3) == -1;
}

// On a 3-node path at y=32: nodeAt returns the index of the node within tol of a point (the nearest when two qualify), -1 for none,
// for a bad path and on a Raster Layer.
static bool nodeAtFindsTheNodeUnderAPoint() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    return doc.nodeAt(1, 0, {33, 31}, 3) == 1 && doc.nodeAt(1, 0, {9, 32}, 3) == 0 && doc.nodeAt(1, 0, {20, 32}, 3) == -1
        && doc.nodeAt(1, 0, {10, 32}, 30) == 0 && doc.nodeAt(1, 5, {32, 32}, 3) == -1 && doc.nodeAt(0, 0, {32, 32}, 3) == -1;
}

// On a straight 3-node path at y=32 (width 8): projectKnot maps a pointer to the nearest path time and twice its signed offset
// along the left normal (below the path is +); previewKnots redraws without a history step, so a drag that ends by restoring the
// start knots and calling setKnots once undoes in one step.
static bool draggingAKnotProjectsAndUndoesOnce() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    auto near = [](QPointF a, QPointF b) { return std::abs(a.x() - b.x()) < 1e-3 && std::abs(a.y() - b.y()) < 1e-3; };
    auto start = doc.projectKnot(1, 0, {8, 36});
    auto end = doc.projectKnot(1, 0, {56, 40});
    auto mid = doc.projectKnot(1, 0, {32, 30});
    bool ends = start && end && start->t < 1e-3 && std::abs(start->width - 8) < 1e-6 && std::abs(end->width - 16) < 1e-6;
    bool sideways = mid && std::abs(mid->width + 4) < 1e-6 && mid->t > 0 && mid->t < end->t;
    if (!ends || !sideways) return false;
    QImage original = doc.render();
    auto k0 = doc.knots(1, 0), k1 = k0;
    k1[0] = *mid;
    doc.previewKnots(1, 0, k1);
    QImage live = doc.render();
    bool showing = live != original && near(doc.knotHandles(1, 0)[0], {32, 30});
    doc.previewKnots(1, 0, k0);
    bool restored = doc.render() == original;
    doc.setKnots(1, 0, k1);
    bool oneStep = doc.render() == live && doc.undo() && doc.render() == original && doc.redo() && doc.render() == live;
    return showing && restored && oneStep;
}

// addKnot on a straight 2-knot path (width 8, y=32) puts one knot at the path point nearest the click, with the width the stroke has there,
// so the render is unchanged; one undo removes it; a Raster Layer or bad path index is refused.
static bool addingAKnotKeepsTheStrokeAndUndoesOnce() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    QImage original = doc.render();
    auto before = doc.knots(1, 0);
    bool added = doc.addKnot(1, 0, {32, 20});
    auto k = doc.knots(1, 0);
    bool shape = added && before.size() == 2 && k.size() == 3 && k[0].t == before[0].t && k[2].t == before[1].t && std::abs(k[1].t - 1) < 1e-2 && std::abs(k[1].width - 8) < 1e-6;
    bool same = doc.render() == original;
    bool oneStep = doc.undo() && doc.knots(1, 0).size() == 2 && doc.render() == original && doc.redo() && doc.knots(1, 0).size() == 3;
    bool refused = !doc.addKnot(0, 0, {32, 32}) && !doc.addKnot(1, 1, {32, 32});
    return shape && same && oneStep && refused;
}

// Ctrl+click duplicates a knot in place after itself; Ctrl+Alt+click deletes it, but never the last one.
static bool duplicatingAndDeletingKnots() {
    std::vector<WidthKnot> k{{0, 4}, {1, 8}, {2, 6}};
    auto same = [](const std::vector<WidthKnot>& a, const std::vector<WidthKnot>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) if (a[i].t != b[i].t || a[i].width != b[i].width) return false;
        return true;
    };
    bool dup = same(duplicateKnot(k, 1), {{0, 4}, {1, 8}, {1, 8}, {2, 6}});
    bool del = same(deleteKnot(k, 1), {{0, 4}, {2, 6}});
    std::vector<WidthKnot> one{{1, 8}};
    return dup && del && same(deleteKnot(one, 0), one);
}

// On the vector-path fixture, setNodes retyping the middle node from G2 to corner changes the render (golden); undo restores the original,
// redo the retyped one; a Raster Layer, a bad path index or a node set with no curve changes nothing.
static bool setNodesRedrawsAndUndoes() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 8, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    QImage before = doc.render();
    bool set = doc.setNodes(1, 0, {{8, 32, T::G2}, {32, 8, T::Corner}, {56, 32, T::G2}}, false);
    QImage got = doc.render().convertToFormat(QImage::Format_Grayscale8);
    QImage golden(GOLDEN_DIR "/vector_nodes_64x64.pgm");
    bool same = !golden.isNull() && got == golden.convertToFormat(QImage::Format_Grayscale8);
    if (!same) {  // write plain-text PGM to eyeball and copy into golden/
        if (FILE* f = std::fopen("actual_vector_nodes.pgm", "w")) {
            std::fprintf(f, "P2\n%d %d\n255\n", got.width(), got.height());
            for (int y = 0; y < got.height(); y++)
                for (int x = 0; x < got.width(); x++) std::fprintf(f, "%d%c", got.constScanLine(y)[x], x + 1 < got.width() ? ' ' : '\n');
            std::fclose(f);
        }
    }
    QImage changed = doc.render();
    bool differs = changed != before;
    bool undone = doc.undo() && doc.render() == before;
    bool redone = doc.redo() && doc.render() == changed;
    bool refused = !doc.setNodes(0, 0, {{8, 32, T::G2}, {32, 8, T::G2}}, false) && !doc.setNodes(1, 1, {{8, 32, T::G2}, {32, 8, T::G2}}, false)
        && !doc.setNodes(1, 0, {{8, 32, T::G2}, {8, 32, T::G2}}, false) && doc.render() == changed;
    return set && same && differs && undone && redone && refused;
}

// A closed 4-node square path (width 12) is a ring: the stroke is black at the node (8,8) and the hole at the centre stays white.
static bool closedPathIsARing() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    bool added = doc.addPath({{8, 8, T::G2}, {56, 8, T::G2}, {56, 56, T::G2}, {8, 56, T::G2}}, true, Qt::black, 12);
    return added && doc.colorAt({8, 8}) == Qt::black && doc.colorAt({32, 32}) == Qt::white;
}

// Moving selected nodes {0, 2} of a 3-node path by (+4, -2) gives the expected node list (types and the unselected node unchanged);
// previewNodes shows it live with no history step, and ending the drag (restore, then one setNodes) undoes in one step.
static bool draggingNodesMovesThemTogetherAndUndoesOnce() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 8, T::Corner}, {56, 32, T::G2}}, false, Qt::black, 8);
    QImage original = doc.render();
    auto n0 = doc.nodes(1, 0);
    auto moved = moveNodes(n0, {0, 2}, {4, -2});
    bool listOk = moved.size() == 3 && moved[0].x == 12 && moved[0].y == 30 && moved[0].type == T::G2 && moved[1].x == 32 && moved[1].y == 8
        && moved[1].type == T::Corner && moved[2].x == 60 && moved[2].y == 30;
    if (!listOk) return false;
    doc.previewNodes(1, 0, moved, false);
    QImage live = doc.render();
    bool showing = live != original && doc.nodes(1, 0)[0].x == 12;
    doc.previewNodes(1, 0, n0, false);
    bool restored = doc.render() == original;
    doc.setNodes(1, 0, moved, false);
    bool oneStep = doc.render() == live && doc.undo() && doc.render() == original && doc.redo() && doc.render() == live;
    return showing && restored && oneStep;
}

// Removing selected nodes {0, 2} of a 4-node path leaves the other two; removing so that fewer than 2 remain is refused; retyping
// selected nodes {1, 2} changes only their type. Committed through setNodes, each is one undo step.
static bool deletingAndRetypingNodes() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {24, 8, T::G2}, {40, 56, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    QImage original = doc.render();
    auto n0 = doc.nodes(1, 0);
    auto left = removeNodes(n0, {0, 2});
    if (!left || left->size() != 2 || (*left)[0].x != 24 || (*left)[1].x != 56) return false;
    if (removeNodes(n0, {0, 1, 2})) return false;
    auto typed = retypeNodes(n0, {1, 2}, T::Corner);
    if (typed[0].type != T::G2 || typed[1].type != T::Corner || typed[2].type != T::Corner || typed[3].type != T::G2 || typed[2].x != 40) return false;
    if (!doc.setNodes(1, 0, *left, false) || doc.render() == original) return false;
    return doc.undo() && doc.render() == original;
}

// transformNodes maps a path's nodes through a scale+rotate transform (x' = -3y + 5, y' = 2x + 7) and leaves node types, closed, knots
// (t and width) and the rest of the path as they were.
static bool transformNodesMapsNodesAndKeepsTheRest() {
    using T = SpiroType;
    Document::VPath p{{{10, 20, T::G2}, {1, 0, T::Corner}, {0, 1, T::Left}}, true, Qt::red, 6, {{0, 3}, {2, 4}}};
    auto q = transformNodes(p, QTransform(0, 2, -3, 0, 5, 7));
    bool nodes = q.nodes.size() == 3 && q.nodes[0].x == -55 && q.nodes[0].y == 27 && q.nodes[1].x == 5 && q.nodes[1].y == 9
        && q.nodes[2].x == 2 && q.nodes[2].y == 7;
    bool types = q.nodes[0].type == T::G2 && q.nodes[1].type == T::Corner && q.nodes[2].type == T::Left;
    bool knots = q.knots.size() == 2 && q.knots[0].t == 0 && q.knots[0].width == 3 && q.knots[1].t == 2 && q.knots[1].width == 4;
    return nodes && types && knots && q.closed && q.color == Qt::red && q.width == 6;
}

// The Spiro Pen's click sequence: nodes take the current type, finishing open or closed is one undo step, clicking near the first node
// asks to close, Backspace drops the last node, cancel drops all, and too few nodes leave the pen as it was.
static bool spiroPenPlacesNodesAndFinishes() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    SpiroPen pen;
    pen.click({8, 32});
    pen.type = T::Corner;
    pen.click({32, 32});
    pen.type = T::G2;
    pen.click({56, 32});
    bool nodes = pen.nodes.size() == 3 && pen.nodes[0].type == T::G2 && pen.nodes[1].type == T::Corner && pen.nodes[2].x == 56;
    bool near = pen.onFirst({10, 33}, 4) && !pen.onFirst({20, 32}, 4);
    bool open = pen.finish(doc, false, Qt::black, 8) && pen.nodes.empty() && doc.colorAt({32, 31}) == Qt::black;
    bool oneStep = doc.undo() && doc.colorAt({32, 31}) == Qt::white && doc.layerCount() == 2;  // the path, not the layer
    pen.click({8, 8});
    pen.click({56, 8});
    pen.click({56, 56});
    pen.click({8, 56});
    bool closed = pen.finish(doc, true, Qt::black, 12) && doc.colorAt({8, 8}) != Qt::white && doc.undo() && doc.colorAt({8, 8}) == Qt::white;  // one undo step (how a closed path fills is #119's, see board)
    pen.click({1, 1});
    pen.click({9, 9});
    pen.backspace();
    bool back = pen.nodes.size() == 1 && pen.nodes[0].x == 1;
    bool tooFew = !pen.finish(doc, false, Qt::black, 4) && pen.nodes.size() == 1;
    pen.cancel();
    return nodes && near && open && oneStep && closed && back && tooFew && pen.nodes.empty();
}

// The Tool Options picker lists the five Spiro Node types in Inkscape-ish order (#53); picking row i makes the next placed node that type.
static bool pickerRowsSetTheNextNodeType() {
    using T = SpiroType;
    const T want[] = {T::Corner, T::G4, T::G2, T::Left, T::Right};
    const char* names[] = {"Corner", "G4", "G2", "Left", "Right"};
    if (std::size(kNodeTypes) != 5) return false;
    SpiroPen pen;
    for (int i = 0; i < 5; i++) {
        pen.type = kNodeTypes[i].type;
        pen.click({double(i), 0});
        if (pen.nodes.back().type != want[i] || std::string(kNodeTypes[i].name) != names[i]) return false;
    }
    return true;
}

// A layer switch keeps a tool that fits the new layer's kind, else picks the last tool used on that kind (#161), Brush and Spiro Pen the first time.
static bool toolFollowsLayerKind() {
    return toolForLayer(false, SpiroPenTool) == BrushTool && toolForLayer(true, BrushTool) == SpiroPenTool
        && toolForLayer(false, NodeKnotEditTool, LassoFillTool, NodeKnotEditTool) == LassoFillTool
        && toolForLayer(true, LassoFillTool, LassoFillTool, NodeKnotEditTool) == NodeKnotEditTool
        && toolForLayer(false, LassoFillTool) == LassoFillTool && toolForLayer(true, NodeKnotEditTool) == NodeKnotEditTool
        && toolForLayer(false, SelectTool) == SelectTool && toolForLayer(true, SelectTool) == SelectTool  // Select fits both kinds (#221)
        && toolForLayer(false, BoxSelectTool) == BoxSelectTool && toolForLayer(true, BoxSelectTool) == SpiroPenTool  // Box Select fits Raster Layers only (#204)
        && toolForLayer(true, BoxSelectTool, BrushTool, NodeKnotEditTool) == NodeKnotEditTool;
}

// rasterizeLayer turns a Vector Layer into a Raster Layer with the same pixels, name, visibility and opacity and no paths; one undo
// gives back the editable Vector Layer, redo rasterizes again; a Raster Layer or bad index is refused.
static bool rasterizingAVectorLayerUndoesOnce() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{8, 32, T::G2}, {32, 32, T::G2}, {56, 32, T::G2}}, false, Qt::black, 8);
    doc.setOpacity(1, 0.5);
    QImage pixels = doc.image(1);
    QString name = doc.name(1);
    bool done = doc.rasterizeLayer(1);
    bool raster = done && !doc.vector(1) && doc.paths(1).empty() && doc.image(1) == pixels && doc.name(1) == name && doc.opacity(1) == 0.5 && doc.visible(1);
    bool undone = doc.undo() && doc.vector(1) && doc.paths(1).size() == 1 && doc.image(1) == pixels;
    bool redone = doc.redo() && !doc.vector(1) && doc.paths(1).empty() && doc.image(1) == pixels;
    bool refused = !doc.rasterizeLayer(1) && !doc.rasterizeLayer(0) && !doc.rasterizeLayer(5);
    return raster && undone && redone && refused;
}

// setPaths replaces a Vector Layer's whole path list: a list missing two of three paths removes exactly those and redraws; one undo
// restores the list and the render, redo removes them again; a Raster Layer or bad index is refused and pushes no step.
static bool settingPathsUndoesOnce() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    for (int y : {10, 30, 50}) doc.addPath({{8, double(y), T::G2}, {32, double(y), T::G2}, {56, double(y), T::G2}}, false, Qt::black, 6);
    QImage all = doc.render();
    auto keep = doc.paths(1);
    keep.erase(keep.begin() + 1, keep.end());
    bool set = doc.setPaths(1, keep) && doc.paths(1).size() == 1 && doc.render() != all;
    QImage one = doc.render();
    bool undone = doc.undo() && doc.paths(1).size() == 3 && doc.render() == all;
    bool redone = doc.redo() && doc.paths(1).size() == 1 && doc.render() == one;
    bool refused = !doc.setPaths(0, keep) && !doc.setPaths(5, keep) && doc.undo() && doc.render() == all;
    return set && undone && redone && refused;
}

// pathsIn returns the indexes of the paths a box touches: a box over two of three horizontal paths gives those two (a box in the
// gap between them gives none), and a Raster Layer gives none. Deleting the others through setPaths undoes in one step.
static bool boxPicksThePathsItTouches() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    for (int y : {10, 30, 50}) doc.addPath({{8, double(y), T::G2}, {32, double(y), T::G2}, {56, double(y), T::G2}}, false, Qt::black, 6);
    auto hit = doc.pathsIn(1, QRectF(20, 5, 10, 30));
    bool two = hit == std::vector<int>{0, 1} && doc.pathsIn(1, QRectF(20, 15, 10, 10)).empty() && doc.pathsIn(0, QRectF(0, 0, 64, 64)).empty();
    QImage all = doc.render();
    auto rest = doc.paths(1);
    rest.erase(rest.begin(), rest.begin() + 2);
    bool deleted = doc.setPaths(1, rest) && doc.paths(1).size() == 1 && doc.undo() && doc.paths(1).size() == 3 && doc.render() == all;
    return two && deleted;
}

// clearRect clears a rectangle of the current Raster Layer: transparent on an added layer, white on the background, outside pixels
// untouched; one undo restores the exact pixels, redo clears again; a Vector Layer is refused and pushes no step.
static bool clearingARectangleUndoesOnce() {
    Document doc(QSize(64, 64));
    doc.fill(Qt::red, {{0, 0}, {64, 0}, {64, 64}, {0, 64}});  // paints the background
    doc.addLayer();
    doc.fill(Qt::blue, {{0, 0}, {64, 0}, {64, 64}, {0, 64}});
    QImage before = doc.image(1);
    bool cleared = doc.clearRect(QRect(10, 20, 8, 6)) && doc.image(1).pixelColor(12, 22).alpha() == 0 && doc.image(1).pixelColor(9, 22) == QColor(Qt::blue)
        && doc.image(1).pixelColor(18, 22) == QColor(Qt::blue) && doc.image(1).pixelColor(12, 26) == QColor(Qt::blue) && doc.image(1).pixelColor(12, 19) == QColor(Qt::blue);
    QImage after = doc.image(1);
    bool undone = doc.undo() && doc.image(1) == before;
    bool redone = doc.redo() && doc.image(1) == after;
    doc.setCurrent(0);
    QImage bg = doc.image(0);
    bool white = doc.clearRect(QRect(0, 0, 4, 4)) && doc.image(0).pixelColor(1, 1) == QColor(Qt::white) && doc.image(0).pixelColor(4, 1) == QColor(Qt::red) && doc.undo() && doc.image(0) == bg;
    doc.addVectorLayer();
    bool refused = !doc.clearRect(QRect(0, 0, 4, 4)) && doc.undo() && doc.layerCount() == 2;
    return cleared && undone && redone && white && refused;
}

int main() {
    int failed = 0;
    auto run = [&](const char* name, bool (*fn)()) {
        bool ok = fn();
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    run("newDocumentRendersWhite", newDocumentRendersWhite);
    run("viewMapsAndRoundTrips", viewMapsAndRoundTrips);
    run("viewOpsKeepAnchorFixed", viewOpsKeepAnchorFixed);
    run("viewScreenRadiusFollowsZoomOnly", viewScreenRadiusFollowsZoomOnly);
    run("spiroMatchesReference", spiroMatchesReference);
    run("spiroFailsInsteadOfGarbage", spiroFailsInsteadOfGarbage);
    run("bundledPencilLoads", bundledPencilLoads);
    run("bundledBrushesListAndLoad", bundledBrushesListAndLoad);
    run("replayedPencilStrokeMatchesGolden",replayedPencilStrokeMatchesGolden);
    run("liveFedPencilStrokeMatchesGolden", liveFedPencilStrokeMatchesGolden);
    run("undoRestoresAndRedoReapplies", undoRestoresAndRedoReapplies);
    run("newStrokeAfterUndoDiscardsRedo", newStrokeAfterUndoDiscardsRedo);
    run("liveFedStrokeIsOneUndoStep", liveFedStrokeIsOneUndoStep);
    run("budgetDropsOldestKeepsNewest", budgetDropsOldestKeepsNewest);
    run("colorAtReadsCompositePixel", colorAtReadsCompositePixel);
    run("eraserStrokeErasesAndUndoes", eraserStrokeErasesAndUndoes);
    run("brushColorPaintsInThatColor", brushColorPaintsInThatColor);
    run("brushRadiusAndOpacityChangeTheStroke", brushRadiusAndOpacityChangeTheStroke);
    run("secondStrokeDoesNotJoinFirst", secondStrokeDoesNotJoinFirst);
    run("widthAtMatchesSlopfont", widthAtMatchesSlopfont);
    run("powerStrokeBodiesMatchSlopfont", powerStrokeBodiesMatchSlopfont);
    run("endKnotsGiveUniformStrokeWithTaperedCaps", endKnotsGiveUniformStrokeWithTaperedCaps);
    run("cornerGetsRoundJoin", cornerGetsRoundJoin);
    run("closedSkeletonJoinsWrapAround", closedSkeletonJoinsWrapAround);
    run("dragResizesRadiusWithinRange", dragResizesRadiusWithinRange);
    run("fillsFigureEightAndErases", fillsFigureEightAndErases);
    run("fillHonoursAlpha", fillHonoursAlpha);
    run("strokeLandsOnCurrentLayerOnly", strokeLandsOnCurrentLayerOnly);
    run("compositeOfThreeLayersMatchesGolden", compositeOfThreeLayersMatchesGolden);
    run("layerOpsUndoAndRedoInOneHistory", layerOpsUndoAndRedoInOneHistory);
    run("layerNamesAreStoredAndUndoable", layerNamesAreStoredAndUndoable);
    run("zipRoundTripsTwoEntries", zipRoundTripsTwoEntries);
    run("oraRoundTripsThreeLayers", oraRoundTripsThreeLayers);
    run("oraRoundTripsVectorLayer", oraRoundTripsVectorLayer);
    run("exportedPngEqualsRender", exportedPngEqualsRender);
    run("vectorPathRendersAsPowerStroke", vectorPathRendersAsPowerStroke);
    run("closedPathIsARing", closedPathIsARing);
    run("setKnotsRedrawsAndUndoes", setKnotsRedrawsAndUndoes);
    run("setNodesRedrawsAndUndoes", setNodesRedrawsAndUndoes);
    run("selectingAPathShowsItsKnotHandles", selectingAPathShowsItsKnotHandles);
    run("wideStrokeIsHitInsideItsBody", wideStrokeIsHitInsideItsBody);
    run("closedPathIsHitInsideAndThinPathStaysHittable", closedPathIsHitInsideAndThinPathStaysHittable);
    run("nodeAtFindsTheNodeUnderAPoint", nodeAtFindsTheNodeUnderAPoint);
    run("draggingAKnotProjectsAndUndoesOnce", draggingAKnotProjectsAndUndoesOnce);
    run("addingAKnotKeepsTheStrokeAndUndoesOnce", addingAKnotKeepsTheStrokeAndUndoesOnce);
    run("duplicatingAndDeletingKnots", duplicatingAndDeletingKnots);
    run("draggingNodesMovesThemTogetherAndUndoesOnce", draggingNodesMovesThemTogetherAndUndoesOnce);
    run("deletingAndRetypingNodes", deletingAndRetypingNodes);
    run("transformNodesMapsNodesAndKeepsTheRest", transformNodesMapsNodesAndKeepsTheRest);
    run("spiroPenPlacesNodesAndFinishes", spiroPenPlacesNodesAndFinishes);
    run("pickerRowsSetTheNextNodeType", pickerRowsSetTheNextNodeType);
    run("toolFollowsLayerKind", toolFollowsLayerKind);
    run("settingPathsUndoesOnce", settingPathsUndoesOnce);
    run("rasterizingAVectorLayerUndoesOnce", rasterizingAVectorLayerUndoesOnce);
    run("boxPicksThePathsItTouches", boxPicksThePathsItTouches);
    run("clearingARectangleUndoesOnce", clearingARectangleUndoesOnce);
    return failed != 0;
}
