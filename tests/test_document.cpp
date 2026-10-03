#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "brush.h"
#include "miniz.h"
#include "document.h"
#include "ora.h"
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include "powerstroke.h"
#include "references.h"
#include "spiro.h"
#include "spiropen.h"
#include "stabilizer.h"
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

// zoom() and rotation() read back composed ops, mirrored too; setZoom/setRotation hold the point and the other quantity.
static bool stabilizerAveragesAndFinishesAtThePen() {
    auto eq = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    // A zigzag: x 0,4,8, y 0,6,0, pressure 0.2,0.5,0.8.
    PenSample zig[] = {{0, 0, 0.2f, 0, 0, 0.01}, {4, 6, 0.5f, 0, 0, 0.01}, {8, 0, 0.8f, 0, 0, 0.01}};
    Stabilizer off{3, false}, on{3, true}, pass{0, false};
    for (auto& s : zig) off.push(s), on.push(s), pass.push(s);
    PenSample m = off.sample();
    bool ok = eq(m.x, 4) && eq(m.y, 2) && eq(m.pressure, 0.8);
    ok = ok && eq(on.sample().pressure, 0.5);
    PenSample p = pass.sample();
    ok = ok && eq(p.x, 8) && eq(p.y, 0) && eq(p.pressure, 0.8) && pass.finish().empty();
    auto tail = off.finish();
    ok = ok && !tail.empty() && eq(tail.back().x, 8) && eq(tail.back().y, 0);
    // Each finishing sample moves toward the pen, never past it.
    for (size_t i = 1; i < tail.size(); ++i) ok = ok && tail[i].x >= tail[i - 1].x && tail[i].x <= 8;
    return ok;
}

static bool viewReportsAndSetsZoomRotation() {
    auto eq = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    View v;
    v.pan({-7, 13});
    v.zoomAt({3, 4}, 2.5);
    v.rotateAt({5, 1}, 30);
    bool ok = eq(v.zoom(), 2.5) && eq(v.rotation(), 30);
    v.mirrorAt({2, 2});
    ok = ok && eq(v.zoom(), 2.5) && eq(v.rotation(), -30);
    v.rotateAt({0, 0}, 20);
    ok = ok && eq(v.rotation(), -10);
    QPointF a(100, 80), under = v.toDoc(a);
    v.setZoom(a, 4);
    ok = ok && eq(v.zoom(), 4) && eq(v.rotation(), -10) && near(v.toDoc(a), under);
    v.setRotation(a, 170);
    return ok && eq(v.rotation(), 170) && eq(v.zoom(), 4) && near(v.toDoc(a), under);
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

// The tests' own pencil.myb loads, and its "opaque" setting reads back the file's 0.7.
static bool bundledPencilLoads() {
    auto b = loadBrush(TEST_PENCIL);
    return b && mypaint_brush_get_base_value(b.get(), MYPAINT_BRUSH_SETTING_OPAQUE) == 0.7f;
}

// The bundled brush list is sorted by name, and every entry loads.
static bool bundledBrushesListAndLoad() {
    auto list = listBrushes(BRUSH_DIR);
    for (size_t i = 0; i < list.size(); i++) {
        if (i && !(list[i - 1].name < list[i].name)) return false;
        if (!loadBrush(list[i].path)) return false;
    }
    return list.size() > 1;
}

// A recorded pen-sample list (pressure ramping up along a wave) replayed with the pencil renders as the golden.
static bool replayedPencilStrokeMatchesGolden() {
    auto pencil = loadBrush(TEST_PENCIL);
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
    auto pencil = loadBrush(TEST_PENCIL);
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
    auto pencil = loadBrush(TEST_PENCIL);
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
    auto pencil = loadBrush(TEST_PENCIL);
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
    auto pencil = loadBrush(TEST_PENCIL);
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    QImage blank = doc.render().copy();
    auto samples = waveAt(32);
    for (size_t i = 0; i < samples.size(); i++) doc.stroke(pencil, {samples[i]}, i > 0);
    return doc.render() != blank && doc.undo() && doc.render() == blank && !doc.undo();
}

// With a budget of one byte only the newest step survives: undo reaches the state before it, and no further.
static bool budgetDropsOldestKeepsNewest() {
    auto pencil = loadBrush(TEST_PENCIL);
    if (!pencil) return false;
    Document doc(QSize(64, 64), 1);
    doc.stroke(pencil, waveAt(16));
    QImage first = doc.render().copy();
    doc.stroke(pencil, waveAt(48));
    return doc.undo() && doc.render() == first && !doc.undo();
}

// Undo step ids (#240's dirty marker): 0 when fresh, a new id per edit, undo/redo walk them, a budget drop keeps the current one.
static bool stepIdsTrackTheHistory() {
    auto pencil = loadBrush(TEST_PENCIL);
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    int fresh = doc.stepId();
    doc.stroke(pencil, waveAt(16));
    int a = doc.stepId();
    doc.stroke(pencil, waveAt(48));
    int b = doc.stepId();
    doc.undo();
    int undone = doc.stepId();
    doc.redo();
    int redone = doc.stepId();
    doc.undo();
    doc.stroke(pencil, waveAt(32));
    int c = doc.stepId();
    Document tiny(QSize(64, 64), 1);
    tiny.stroke(pencil, waveAt(16));
    tiny.stroke(pencil, waveAt(48));
    int last = tiny.stepId();
    tiny.undo();
    int dropped = tiny.stepId();
    return fresh == 0 && a != 0 && b != a && undone == a && redone == b && c != a && c != b && c != 0 &&
           tiny.stepId() == dropped && dropped != 0 && dropped != last;
}

// After the pencil wave, colorAt reads the golden's gray at a painted pixel (53,34), white elsewhere, and invalid off the canvas.
static bool colorAtReadsCompositePixel() {
    auto pencil = loadBrush(TEST_PENCIL);
    if (!pencil) return false;
    Document doc(QSize(64, 64));
    for (int i = 0; i < 16; i++)  // the golden's samples, as in replayedPencilStrokeMatchesGolden
        doc.stroke(pencil, {{4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01}}, i > 0);
    QColor painted = doc.colorAt({53, 34}), white = doc.colorAt({0, 0});
    return painted == QColor(137, 137, 137, 255) && white == QColor(255, 255, 255, 255) &&
           !doc.colorAt({-1, 0}).isValid() && !doc.colorAt({64, 0}).isValid() && !doc.colorAt({0, 64}).isValid();
}

// Replaying the pencil wave's samples with the eraser flag takes the painted stroke's alpha down (golden), and undo restores the painted-only image.
static bool eraserStrokeErasesAndUndoes() {
    auto pencil = loadBrush(TEST_PENCIL);
    if (!pencil) return false;
    std::vector<PenSample> samples;  // the golden's samples, as in replayedPencilStrokeMatchesGolden
    for (int i = 0; i < 16; i++)
        samples.push_back({4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 0.1f + 0.06f * i, 0, 0, 0.01});
    Document doc(QSize(64, 64));
    doc.stroke(pencil, samples);
    QImage painted = doc.render().copy();
    doc.stroke(pencil, samples, false, true);
    QImage got = doc.render().convertToFormat(QImage::Format_Alpha8);  // erase lives in the alpha; the golden is that channel
    got.reinterpretAsFormat(QImage::Format_Grayscale8);
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
    bool lighter = doc.colorAt({53, 34}).alpha() < 255;  // the pixel colorAtReadsCompositePixel finds painted
    return same && lighter && doc.undo() && doc.render() == painted;
}

// The pencil wave painted after setBrushColor(red) is red at (53,34), where the default gray left 137.
static bool brushColorPaintsInThatColor() {
    auto pencil = loadBrush(TEST_PENCIL);
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
        auto pencil = loadBrush(TEST_PENCIL);
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
    auto pencil = loadBrush(TEST_PENCIL);
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
    auto grayGolden = [](const Document& doc, const char* name, const char* actual, bool alpha = false) {
        QImage got = doc.render().convertToFormat(alpha ? QImage::Format_Alpha8 : QImage::Format_Grayscale8);  // erase lives in the alpha
        got.reinterpretAsFormat(QImage::Format_Grayscale8);
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
    bool erased = doc.colorAt({32, 30}).alpha() == 0 && doc.colorAt({50, 32}) == Qt::black && grayGolden(doc, "fill_figure_eight_erased_64x64.pgm", "actual_fill_figure_eight_erased.pgm", true);
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
    auto pencil = loadBrush(TEST_PENCIL);
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

// Alpha Lock on a Raster Layer and Inherit Alpha on another saved to .ora read back set, the rest off, with nothing to undo.
static bool oraRoundTripsAlphaFlags() {
    Document doc(QSize(64, 64));
    doc.addLayer(); doc.addLayer();
    doc.setAlphaLock(1, true); doc.setInheritAlpha(2, true);
    QTemporaryDir dir;
    QString path = dir.filePath("t.ora");
    if (!writeOra(doc, path)) return false;
    auto back = readOra(path);
    if (!back || back->layerCount() != 3) return false;
    for (int i = 0; i < 3; i++)
        if (back->alphaLock(i) != (i == 1) || back->inheritAlpha(i) != (i == 2)) return false;
    return !back->undo();
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

// A path with mode Both and a fill color round-trips through .ora; an SVG written before kind/mode/fillcolor existed reads as Spiro, Stroke (#296).
static bool oraRoundTripsPaintMode() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{10, 10, T::G2}, {30, 10, T::G2}, {30, 25, T::G2}, {10, 25, T::G2}}, true, QColor(10, 20, 30), 4);
    auto ps = doc.paths(1);
    ps[0].mode = Document::PaintMode::Both;
    ps[0].fillColor = QColor(200, 100, 50, 128);
    doc.setPaths(1, ps);
    QTemporaryDir dir;
    QString path = dir.filePath("t.ora");
    if (!writeOra(doc, path)) return false;
    auto back = readOra(path);
    if (!back || back->paths(1).size() != 1) return false;
    const auto& b = back->paths(1)[0];
    if (b.kind != Document::PathKind::Spiro || b.mode != Document::PaintMode::Both || b.fillColor != QColor(200, 100, 50, 128)) return false;
    auto old = readVectorSvg(R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:mydraw="urn:mydraw"><path d="M0 0Z" fill="#0a141e" fill-opacity="1" mydraw:nodes="8,50,v 56,50,v" mydraw:closed="0" mydraw:width="6" mydraw:knots="0,6 1,6"/></svg>)");
    return old.size() == 1 && old[0].kind == Document::PathKind::Spiro && old[0].mode == Document::PaintMode::Stroke && old[0].color == QColor(10, 20, 30);
}

// A Bezier path round-trips through .ora with its handles and node types exact, beside a Spiro path (#307).
static bool oraRoundTripsBezier() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{10, 10, T::G2}, {30, 10, T::G2}, {30, 25, T::G2}, {10, 25, T::G2}}, true, QColor(10, 20, 30), 4);
    Document::VPath v{{}, false, Qt::black, 4, {{0, 4}, {1.5, 7}, {2, 4}}, Document::PathKind::Bezier};
    v.bezier = {{{12, 48}, {12, 48}, {28.25, 0.5}, BezierType::Cusp},
                {{30, 30}, {25, 35}, {35, 25}, BezierType::Smooth},
                {{52, 16}, {50.125, 10}, {52, 16}, BezierType::Symmetric}};
    auto ps = doc.paths(1);
    ps.push_back(v);
    doc.setPaths(1, ps);
    QTemporaryDir dir;
    QString path = dir.filePath("t.ora");
    if (!writeOra(doc, path)) return false;
    auto back = readOra(path);
    if (!back || back->paths(1).size() != 2) return false;
    const auto& s = back->paths(1)[0];
    const auto& b = back->paths(1)[1];
    if (s.kind != Document::PathKind::Spiro || s.nodes.size() != 4 || !s.bezier.empty()) return false;
    if (b.kind != Document::PathKind::Bezier || !b.nodes.empty() || b.bezier.size() != 3 || b.knots.size() != 3) return false;
    for (size_t i = 0; i < 3; ++i)
        if (b.bezier[i].p != v.bezier[i].p || b.bezier[i].in != v.bezier[i].in || b.bezier[i].out != v.bezier[i].out
            || b.bezier[i].type != v.bezier[i].type) return false;
    return back->render() == doc.render();
}

// The written SVG draws what the mode draws: Stroke and Both write the outline in the stroke color, Fill the curve in the fill color (#300).
static bool svgFollowsPaintMode() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{10, 10, T::G2}, {30, 10, T::G2}, {30, 25, T::G2}, {10, 25, T::G2}}, true, QColor(10, 20, 30), 4);
    QTemporaryDir dir;
    QString path = dir.filePath("t.ora");
    for (auto m : {Document::PaintMode::Stroke, Document::PaintMode::Fill, Document::PaintMode::Both}) {
        auto ps = doc.paths(1);
        ps[0].mode = m;
        ps[0].fillColor = QColor(200, 100, 50, 128);
        doc.setPaths(1, ps);
        if (!writeOra(doc, path)) return false;
        mz_zip_archive z{};
        if (!mz_zip_reader_init_file(&z, path.toUtf8().constData(), 0)) return false;
        size_t n = 0;
        void* raw = mz_zip_reader_extract_file_to_heap(&z, "data/layer1.svg", &n, 0);
        QByteArray svg(static_cast<const char*>(raw), qsizetype(n));
        mz_free(raw); mz_zip_reader_end(&z);
        QXmlStreamReader r(svg);
        while (!r.atEnd() && !(r.readNext() == QXmlStreamReader::StartElement && r.name() == u"path")) {}
        auto a = r.attributes();
        bool fill = m == Document::PaintMode::Fill;
        QPainterPath expect = fill ? *spiroToPath(ps[0].nodes, true) : Document::outline(ps[0]);
        QString c = (fill ? ps[0].fillColor : ps[0].color).name();
        double op = (fill ? ps[0].fillColor : ps[0].color).alphaF();
        // d's first point is the path's first element.
        QString start = QString("M%1 %2").arg(expect.elementAt(0).x, 0, 'g', 17).arg(expect.elementAt(0).y, 0, 'g', 17);
        if (!a.value("d").startsWith(start) || a.value("fill") != c || std::abs(a.value("fill-opacity").toDouble() - op) > 1e-9) return false;
        auto back = readOra(path);
        if (!back || back->paths(1)[0].mode != m) return false;
    }
    return true;
}

// The Pen's Paint mode goes on the next path: Fill fills the inside in the stroke color, the default stays Stroke (#301).
static bool penPaintModeGoesOnNextPath() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    SpiroPen pen;
    pen.nodes = {{10, 10, T::Corner}, {50, 10, T::Corner}, {50, 50, T::Corner}, {10, 50, T::Corner}};
    if (!pen.finish(doc, true, QColor(10, 20, 30), 2) || doc.paths(1)[0].mode != Document::PaintMode::Stroke) return false;
    pen.nodes = {{10, 10, T::Corner}, {50, 10, T::Corner}, {50, 50, T::Corner}, {10, 50, T::Corner}};
    if (!pen.finish(doc, true, QColor(10, 20, 30), 2, Document::PaintMode::Fill)) return false;
    auto v = doc.paths(1)[1];
    return v.mode == Document::PaintMode::Fill && v.fillColor == QColor(10, 20, 30) && doc.colorAt({30, 30}) == QColor(10, 20, 30);
}

// The Fill swatch is the next path's fill color, apart from its stroke color (#302).
static bool penFillSwatchGoesOnNextPath() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    SpiroPen pen;
    pen.nodes = {{10, 10, T::Corner}, {50, 10, T::Corner}, {50, 50, T::Corner}, {10, 50, T::Corner}};
    if (!pen.finish(doc, true, QColor(10, 20, 30), 2, Document::PaintMode::Both, QColor(200, 0, 0))) return false;
    auto v = doc.paths(1)[0];
    return v.color == QColor(10, 20, 30) && v.fillColor == QColor(200, 0, 0) && doc.colorAt({30, 30}) == QColor(200, 0, 0);
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

// A closed 4-node Fill square (8 wide, the Spiro curve bulging round it) is hit deep inside it, where the ring is not drawn; a thin open path (width 2) is still hit
// within 3 px of its centre line, and a point clear of both is not.
static bool closedPathIsHitInsideAndThinPathStaysHittable() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{10, 10, T::G2}, {40, 10, T::G2}, {40, 40, T::G2}, {10, 40, T::G2}}, true, Qt::black, 8);
    doc.addPath({{8, 60, T::G2}, {32, 60, T::G2}, {56, 60, T::G2}}, false, Qt::black, 2);
    auto ps = doc.paths(1);
    ps[0].mode = Document::PaintMode::Fill;
    doc.setPaths(1, ps);
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

// The Transform box (#230): pickedBounds is the union of the picked paths' outlines (two of three picked give those two, not the third);
// moving them through transformPaths then setPaths shifts only those, the bounds follow, and one undo restores the nodes and the render.
static bool transformBoxBoundsAndMovesPickedPaths() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    for (int y : {10, 30, 50}) doc.addPath({{8, double(y), T::G2}, {32, double(y), T::G2}, {56, double(y), T::G2}}, false, Qt::black, 6);
    QImage all = doc.render();
    QRectF b = pickedBounds(doc.paths(1), {0, 1});
    bool bounds = b.left() > 3 && b.left() < 9 && b.right() > 55 && b.right() < 61 && b.top() > 5 && b.top() < 10 && b.bottom() > 30 && b.bottom() < 36;
    bool moved = doc.setPaths(1, transformPaths(doc.paths(1), {0, 1}, QTransform::fromTranslate(5, 0)));
    bool only = moved && doc.paths(1)[0].nodes[0].x == 13 && doc.paths(1)[1].nodes[0].x == 13 && doc.paths(1)[2].nodes[0].x == 8;
    bool follows = std::abs(pickedBounds(doc.paths(1), {0, 1}).left() - (b.left() + 5)) < 1e-6;
    bool undone = doc.undo() && doc.paths(1)[0].nodes[0].x == 8 && doc.render() == all;
    return bounds && only && follows && undone;
}

// Transform box scale (#231): scaleMatrix(box, handle, pointer, keepRatio) scales about the opposite corner or edge; boxHandleAt finds a handle within tol.
// Box (10,10)-(50,30): dragging the bottom-right corner to (90,30) doubles x about (10,10); with Shift both axes double; the top edge dragged
// to y -10 doubles y about the bottom edge and leaves x alone. Through setPaths, one undo restores the nodes.
static bool transformBoxScalesAboutOppositeHandle() {
    using T = SpiroType;
    QRectF box(10, 10, 40, 20);
    QTransform corner = scaleMatrix(box, {1, 1}, {90, 30}, false), ratio = scaleMatrix(box, {1, 1}, {90, 30}, true), edge = scaleMatrix(box, {0, -1}, {20, -10}, false);
    bool c = corner.map(QPointF(50, 30)) == QPointF(90, 30) && corner.map(QPointF(10, 10)) == QPointF(10, 10);
    bool r = ratio.map(QPointF(50, 30)) == QPointF(90, 50) && ratio.map(QPointF(10, 10)) == QPointF(10, 10);
    bool e = edge.map(QPointF(30, 10)) == QPointF(30, -10) && edge.map(QPointF(50, 30)) == QPointF(50, 30);
    bool hit = boxHandleAt(box, {52, 31}, 6) == QPoint(1, 1) && boxHandleAt(box, {30, 8}, 6) == QPoint(0, -1) && !boxHandleAt(box, {30, 20}, 6) && !boxHandleAt(box, {70, 20}, 6);
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    for (int y : {10, 30}) doc.addPath({{8, double(y), T::G2}, {32, double(y), T::G2}, {56, double(y), T::G2}}, false, Qt::black, 6);
    QImage before = doc.render();
    bool scaled = doc.setPaths(1, transformPaths(doc.paths(1), {0}, scaleMatrix(QRectF(8, 10, 48, 0), {1, 0}, {32, 10}, false)));
    bool nodes = scaled && doc.paths(1)[0].nodes[2].x == 32 && doc.paths(1)[1].nodes[2].x == 56;
    bool undone = doc.undo() && doc.paths(1)[0].nodes[2].x == 56 && doc.render() == before;
    return c && r && e && hit && nodes && undone;
}

// Transform box rotate (#202): rotateMatrix(centre, start, pointer) turns about the centre by the angle the pointer swept; BoxFrame is the box
// turned about its rect's centre, and frameAfter follows it through a transform. Box (0,0)-(100,50), centre (50,25): the pointer sweeping from
// the right of the centre to below it is a quarter turn, the frame then reads 90 degrees, its right handle sits below the box and dragging that
// handle to (50,125) scales along the rotated axis only (x of the document stays put).
static bool transformBoxRotatesAndScalesAlongItsAxes() {
    auto near = [](QPointF a, QPointF b) { return std::abs(a.x() - b.x()) < 1e-9 && std::abs(a.y() - b.y()) < 1e-9; };
    QTransform r = rotateMatrix({50, 25}, {100, 25}, {50, 75});
    bool turn = near(r.map(QPointF(100, 25)), {50, 75}) && near(r.map(QPointF(50, 25)), {50, 25});
    BoxFrame f{QRectF(0, 0, 100, 50), 0}, turned = frameAfter(f, r);
    bool frame = std::abs(turned.angle - 90) < 1e-9 && near(turned.rect.center(), {50, 25}) && turned.rect.size() == f.rect.size();
    bool hit = frameHandleAt(turned, {50, 75}, 6) == QPoint(1, 0) && !frameHandleAt(turned, {100, 25}, 6);
    QTransform m = frameScaleMatrix(turned, {1, 0}, {50, 125}, false);
    bool scale = near(m.map(QPointF(50, 75)), {50, 125}) && near(m.map(QPointF(50, -25)), {50, -25}) && near(m.map(QPointF(100, -25)), {100, -25});
    BoxFrame grown = frameAfter(turned, m);
    bool follows = std::abs(grown.angle - 90) < 1e-9 && near(grown.rect.center(), {50, 50}) && std::abs(grown.rect.width() - 150) < 1e-9 && std::abs(grown.rect.height() - 50) < 1e-9;
    return turn && frame && hit && scale && follows;
}

// Width on picks (#305): rewidthPath scales every Width Knot by new/old width, t untouched. A tapered path of width 6 with knots
// (0,2) (1,3) (2,1) at width 12 has knots (0,4) (1,6) (2,2); one undo of the setPaths restores the knots.
static bool rewidthPathScalesKnots() {
    using T = SpiroType;
    Document::VPath p{{{10, 20, T::G2}, {30, 20, T::G2}, {50, 20, T::G2}}, false, Qt::red, 6, {{0, 2}, {1, 3}, {2, 1}}};
    auto q = rewidthPath(p, 12);
    bool scaled = q.width == 12 && q.knots.size() == 3 && q.knots[0].width == 4 && q.knots[1].width == 6 && q.knots[2].width == 2;
    bool ts = q.knots[0].t == 0 && q.knots[1].t == 1 && q.knots[2].t == 2;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath(p.nodes, false, Qt::black, 6);
    auto before = doc.paths(1)[0].knots;
    bool set = doc.setPaths(1, std::vector<Document::VPath>{rewidthPath(doc.paths(1)[0], 12)}) && doc.paths(1)[0].knots[0].width == 2 * before[0].width;
    bool undone = doc.undo() && doc.paths(1)[0].knots[0].width == before[0].width && doc.paths(1)[0].width == 6;
    return scaled && ts && set && undone;
}

// Flip and Scale widths (#203): flipMatrix mirrors about a centre, horizontally (x) or vertically (y); transformNodes and transformPaths
// scale each Width Knot by sqrt(|det|) of the transform only when asked. A path with knots (0,8) and (2,4) under a 0.5 scale gives 4 and 2
// with it on and 8 and 4 with it off; a flip about x = 50 maps node x 10 to 90, its determinant -1 leaves widths as they are, and one undo restores it.
static bool flipMirrorsAndScaleWidthsIsOptIn() {
    using T = SpiroType;
    Document::VPath p{{{10, 20, T::G2}, {30, 20, T::G2}, {50, 20, T::G2}}, false, Qt::red, 6, {{0, 8}, {2, 4}}};
    QTransform half = QTransform().scale(0.5, 0.5);
    auto on = transformNodes(p, half, true), off = transformNodes(p, half);
    bool widths = on.knots[0].width == 4 && on.knots[1].width == 2 && off.knots[0].width == 8 && off.knots[1].width == 4;
    QTransform h = flipMatrix({50, 25}, true), v = flipMatrix({50, 25}, false);
    bool mirror = h.map(QPointF(10, 20)) == QPointF(90, 20) && v.map(QPointF(10, 20)) == QPointF(10, 30);
    bool kept = transformNodes(p, h, true).knots[0].width == 8;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath(p.nodes, false, Qt::black, 6);
    QImage before = doc.render();
    bool flipped = doc.setPaths(1, transformPaths(doc.paths(1), {0}, flipMatrix({32, 20}, true))) && doc.paths(1)[0].nodes[0].x == 54;
    bool undone = doc.undo() && doc.paths(1)[0].nodes[0].x == 10 && doc.render() == before;
    return widths && mirror && kept && flipped && undone;
}

// Raster selection transform (#205): transformRect lifts a rectangle of the current Raster Layer, clears its source and draws the lifted pixels
// through a transform as one undo step. A red 8x6 block at (10,20) on a blue layer, translated by (20,5): red at (30,25), source transparent,
// one undo restores the exact image. floatRect is the live version: each call starts again from the untouched pixels, so a 0.5 scale followed
// by the same translation ends exactly where a plain translation does, and cancelFloat puts the layer back with no undo step.
static bool transformRectMovesPixelsAndUndoesOnce() {
    Document doc(QSize(64, 64));
    doc.addLayer();
    doc.fill(Qt::red, {{10, 20}, {18, 20}, {18, 26}, {10, 26}});
    QImage before = doc.image(1);
    QRect box(8, 18, 12, 10);
    QTransform move = QTransform::fromTranslate(20, 5);
    bool moved = doc.transformRect(box, move) && doc.image(1).pixelColor(30, 25) == QColor(Qt::red) && doc.image(1).pixelColor(37, 30) == QColor(Qt::red)
        && doc.image(1).pixelColor(29, 25).alpha() == 0 && doc.image(1).pixelColor(38, 25).alpha() == 0 && doc.image(1).pixelColor(11, 21).alpha() == 0;
    QImage after = doc.image(1);
    bool undone = doc.undo() && doc.image(1) == before;
    bool redone = doc.redo() && doc.image(1) == after && doc.undo() && doc.image(1) == before;
    bool live = doc.floatRect(box, QTransform().scale(0.5, 0.5)) && doc.image(1) != before && doc.floatRect(box, move) && doc.image(1) == after;
    doc.cancelFloat();
    bool cancelled = doc.image(1) == before && !doc.redo();
    doc.addVectorLayer();
    bool refused = !doc.transformRect(box, move) && !doc.floatRect(box, move);
    return moved && undone && redone && live && cancelled && refused;
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

// Shift+click in the Pen (#274): a click with the corner flag places a Corner whatever the picker holds; a plain click still uses it.
static bool cornerClickIgnoresThePickerType() {
    SpiroPen pen;
    pen.type = SpiroType::G4;
    pen.click({0, 0}, true);
    pen.click({1, 0});
    return pen.nodes[0].type == SpiroType::Corner && pen.nodes[1].type == SpiroType::G4 && pen.type == SpiroType::G4;
}

// On a half circle through 3 nodes (two 90-degree segments, which Spiro splits into several cubics each), addNode at the lower-right
// quarter puts the new node at index 2 on the click, a click on an existing node adds nothing, every knot stays where it was on the path,
// and one undo restores nodes and knots together.
static bool addNodeSplitsTheSegmentUnderTheClick() {
    using T = SpiroType;
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    doc.addPath({{32, 12, T::G2}, {52, 32, T::G2}, {32, 52, T::G2}}, false, Qt::black, 8);
    auto before = doc.knots(1, 0);
    double mid = doc.projectKnot(1, 0, {52, 32})->t;
    doc.setKnots(1, 0, {before[0], {mid, 8}, before[1]});
    auto handles = doc.knotHandles(1, 0);
    if (doc.addNode(1, 0, {52, 32}, T::G2)) return false;  // on a node: no curve
    if (!doc.addNode(1, 0, {46.1, 46.1}, T::G4)) return false;
    auto n = doc.nodes(1, 0);
    auto k = doc.knots(1, 0);
    auto after = doc.knotHandles(1, 0);
    bool nodes = n.size() == 4 && n[2].type == T::G4 && std::hypot(n[2].x - 46.1, n[2].y - 46.1) < 0.5 && n[1].x == 52 && n[3].y == 52;
    bool knots = k.size() == 3 && k[0].t == 0 && std::abs(k[1].t - doc.projectKnot(1, 0, {52, 32})->t) < 1e-3;
    for (int i = 0; i < 3; i++) knots = knots && QLineF(handles[i], after[i]).length() < 0.5;
    return nodes && knots && doc.undo() && doc.nodes(1, 0).size() == 3 && doc.knots(1, 0)[1].t == mid;
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

// A path paints by its mode (#298): Stroke the Power Stroke in color, Fill only the inside in fillColor, Both fill under stroke. An
// open path's fill closes with a straight line from its last node to its first. Rasterize keeps the pixels.
static bool pathsPaintByMode() {
    using T = SpiroType;
    using M = Document::PaintMode;
    QColor red(255, 0, 0), blue(0, 0, 255);
    auto draw = [&](bool closed, M mode) {
        Document doc(QSize(64, 64));
        doc.addVectorLayer();
        // A square when closed; open, the L along its top and right whose closing diagonal runs (16,16)-(48,48)
        std::vector<SpiroNode> nodes{{16, 16, T::Corner}, {48, 16, T::Corner}, {48, 48, T::Corner}};
        if (closed) nodes.push_back({16, 48, T::Corner});
        doc.addPath(nodes, closed, red, 4);
        auto paths = doc.paths(1);
        paths[0].mode = mode;
        paths[0].fillColor = blue;
        doc.setPaths(1, paths);
        return doc;
    };
    auto at = [](Document& d, int x, int y) { return d.image(1).pixelColor(x, y); };
    bool ok = true;
    for (bool closed : {true, false}) {
        Document s = draw(closed, M::Stroke), f = draw(closed, M::Fill), b = draw(closed, M::Both);
        // (40,24) is inside both shapes; (32,16) is on the top edge's stroke
        ok = ok && at(s, 40, 24).alpha() == 0 && at(s, 32, 16) == red;
        ok = ok && at(f, 40, 24) == blue && at(f, 32, 14).alpha() == 0;
        ok = ok && at(b, 40, 24) == blue && at(b, 32, 16) == red;
        QImage before = b.image(1);
        ok = ok && b.rasterizeLayer(1) && b.image(1) == before;
    }
    // Open: (24,40) is below the closing diagonal, so outside the fill
    Document open = draw(false, M::Fill);
    return ok && at(open, 24, 40).alpha() == 0;
}

// Hit tests follow the mode (#299): inside a closed square, a Stroke path misses and Fill and Both hit, in pathAt and pathsIn
// alike; the stroke band hits in every mode. An open Fill path hits inside its closing diagonal.
static bool hitsFollowMode() {
    using T = SpiroType;
    using M = Document::PaintMode;
    auto make = [](bool closed, M mode) {
        Document doc(QSize(64, 64));
        doc.addVectorLayer();
        std::vector<SpiroNode> nodes{{16, 16, T::Corner}, {48, 16, T::Corner}, {48, 48, T::Corner}};
        if (closed) nodes.push_back({16, 48, T::Corner});
        doc.addPath(nodes, closed, Qt::black, 4);
        auto paths = doc.paths(1);
        paths[0].mode = mode;
        doc.setPaths(1, paths);
        return doc;
    };
    QRectF inside(38, 22, 4, 4);
    bool ok = true;
    for (M m : {M::Stroke, M::Fill, M::Both}) {
        Document d = make(true, m);
        bool fill = m != M::Stroke;
        ok = ok && (d.pathAt(1, {40, 24}, 3) == 0) == fill && d.pathsIn(1, inside).empty() != fill && d.pathAt(1, {32, 16}, 3) == 0;
    }
    Document open = make(false, M::Fill);
    return ok && open.pathAt(1, {40, 24}, 3) == 0 && open.pathsIn(1, inside).size() == 1 && open.pathAt(1, {24, 40}, 3) == -1;
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

// A Bezier path's skeleton is its node-to-node cubics (#306): a two-node S-curve with knots 4, 10, 4 at t 0, 0.5, 1 paints
// across 10 px at the middle of the cubic but not 4 px off near its start, ends flat (nothing past an end along the tangent),
// and pathAt hits it.
static bool bezierPathRendersAsPowerStroke() {
    QPointF p0(12, 48), c1(28, 0), c2(36, 64), p3(52, 16);
    Document::VPath v{{}, false, Qt::black, 4, {{0, 4}, {0.5, 10}, {1, 4}}, Document::PathKind::Bezier};
    v.bezier = {{p0, p0, c1, BezierType::Cusp}, {p3, c2, p3, BezierType::Cusp}};
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    bool set = doc.setPaths(1, {v});
    auto at = [&](double t) { double u = 1 - t; return u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p3; };
    auto normal = [&](double t) {
        QPointF d = at(t + 0.001) - at(t - 0.001);
        return QPointF(-d.y(), d.x()) / std::hypot(d.x(), d.y());
    };
    auto alpha = [&](QPointF p) { return doc.image(1).pixelColor(p.toPoint()).alpha(); };
    bool mid = alpha(at(0.5) + 3.5 * normal(0.5)) > 128 && alpha(at(0.5) - 3.5 * normal(0.5)) > 128;
    bool thin = alpha(at(0.1) + 4 * normal(0.1)) == 0 && alpha(at(0.1)) > 128;
    QPointF back = p0 - c1;
    bool flat = alpha(p0 + 3 * back / std::hypot(back.x(), back.y())) == 0;
    return set && mid && thin && flat && doc.pathAt(1, at(0.5), 1) == 0;
}

// The Pen in Bezier (#308): four clicks and an open finish give one Bezier path of Cusp nodes with handles on their points,
// end knots at t 0 and 3; a closed finish gets one knot; the picker type is ignored and Spiro stays the default.
static bool bezierPenPlacesCornersAndFinishes() {
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    SpiroPen pen;
    bool spiroDefault = !pen.bezier;
    pen.bezier = true;
    for (QPointF p : {QPointF(8, 8), QPointF(56, 8), QPointF(56, 56), QPointF(8, 56)}) pen.click(p);
    bool open = pen.finish(doc, false, Qt::black, 6) && pen.nodes.empty();
    const auto& v = doc.paths(1)[0];
    bool corners = v.kind == Document::PathKind::Bezier && v.nodes.empty() && v.bezier.size() == 4 && !v.closed;
    for (const auto& n : v.bezier) corners = corners && n.in == n.p && n.out == n.p && n.type == BezierType::Cusp;
    bool knots = v.knots.size() == 2 && v.knots[1].t == 3 && doc.image(1).pixelColor(32, 8).alpha() > 128;
    for (QPointF p : {QPointF(8, 8), QPointF(56, 8), QPointF(56, 56)}) pen.click(p);
    bool closed = pen.finish(doc, true, Qt::black, 6) && doc.paths(1).size() == 2 && doc.paths(1)[1].closed && doc.paths(1)[1].knots.size() == 1;
    bool oneStep = doc.undo() && doc.paths(1).size() == 1;
    return spiroDefault && open && corners && knots && closed && oneStep;
}

// Dragging after a Bezier click pulls a symmetric node (#309): drag to (20,8) from the click at (8,8) puts out at (20,8) and in at (-4,8);
// an undragged click stays a Cusp with handles on the point; the preview nodes carry the same handles.
static bool bezierPenDragMakesSymmetricNode() {
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    SpiroPen pen;
    pen.bezier = true;
    pen.click({8, 8});
    pen.drag({20, 8});
    pen.click({56, 56});
    auto live = pen.bezierNodes();
    bool preview = live.size() == 2 && live[0].out == QPointF(20, 8) && live[0].in == QPointF(-4, 8) && live[1].type == BezierType::Cusp;
    bool done = pen.finish(doc, false, Qt::black, 6);
    const auto& b = doc.paths(1)[0].bezier;
    return preview && done && b.size() == 2 && b[0].type == BezierType::Symmetric && b[0].out == QPointF(20, 8) && b[0].in == QPointF(-4, 8) && b[1].type == BezierType::Cusp && b[1].in == b[1].p;
}

// transformNodes on a Bezier path maps handles as well as points (#315): the same scale+rotate (x' = -3y + 5, y' = 2x + 7) sends
// point (10,20), in (1,0), out (0,1) to (-55,27), (5,9), (2,7); types and knots stay, and a flip about x = 50 mirrors a handle's x.
static bool transformNodesMapsBezierHandles() {
    Document::VPath p{{}, false, Qt::red, 6, {{0, 3}}, Document::PathKind::Bezier};
    p.bezier = {{{10, 20}, {1, 0}, {0, 1}, BezierType::Smooth}};
    auto q = transformNodes(p, QTransform(0, 2, -3, 0, 5, 7));
    const auto& n = q.bezier[0];
    bool mapped = n.p == QPointF(-55, 27) && n.in == QPointF(5, 9) && n.out == QPointF(2, 7);
    bool kept = n.type == BezierType::Smooth && q.knots.size() == 1 && q.knots[0].width == 3 && q.nodes.empty();
    auto f = transformNodes(p, flipMatrix({50, 25}, true)).bezier[0];
    return mapped && kept && f.p == QPointF(90, 20) && f.in == QPointF(99, 0) && f.out == QPointF(100, 1);
}

// Spiro -> Bezier (#316): a path with a Corner converts to a Bezier path with the same outline and the same knots; the Corner
// node is a Cusp, a G2 node in the middle of the arc is Smooth.
static bool toBezierKeepsOutlineAndKnots() {
    Document::VPath v{{{10, 50, SpiroType::Corner}, {30, 10, SpiroType::G2}, {50, 50, SpiroType::G2}, {70, 10, SpiroType::Corner}}, false, Qt::black, 4, {{0, 4}, {1.5, 9}, {6, 4}}};
    Document::VPath b = Document::toBezier(v);
    bool same = b.kind == Document::PathKind::Bezier && b.nodes.empty() && Document::outline(b) == Document::outline(v) && Document::curve(b) == Document::curve(v);
    bool knots = b.knots.size() == 3 && b.knots[1].t == 1.5 && b.knots[1].width == 9 && b.knots[2].t == 6;
    bool types = !b.bezier.empty() && b.bezier.front().type == BezierType::Cusp && b.bezier.back().type == BezierType::Cusp;
    int cusps = 0;
    for (const auto& n : b.bezier) cusps += n.type == BezierType::Cusp;
    return same && knots && types && cusps >= 2 && cusps < int(b.bezier.size());
}

// Node/Knot edit on a Bezier path (#310): nodes() are its points, so the Node drag finds them; moving node 1 by (5,-3) through
// previewNodes then (restored) setNodes moves its in/out handles by (5,-3) too, node 0 stays, one undo step puts it back, and a knot still adds.
static bool bezierNodesDragWithHandles() {
    Document doc(QSize(100, 100));
    int l = doc.addVectorLayer();
    Document::VPath v{{}, false, Qt::black, 4, {{0, 4}, {1, 4}}, Document::PathKind::Bezier};
    v.bezier = {{{10, 50}, {10, 50}, {30, 30}, BezierType::Cusp}, {{70, 50}, {60, 30}, {80, 70}, BezierType::Symmetric}};
    doc.restorePaths(l, {v});
    auto n = doc.nodes(l, 0);
    if (n.size() != 2 || doc.nodeAt(l, 0, {70, 51}, 3) != 1) return false;
    auto moved = moveNodes(n, {1}, {5, -3});
    doc.previewNodes(l, 0, moved, false);
    doc.previewNodes(l, 0, n, false);  // as endNodeDrag: back to the start, then one step
    if (!doc.setNodes(l, 0, moved, false)) return false;
    const auto& b = doc.paths(l)[0];
    bool after = b.nodes.empty() && b.bezier[1].p == QPointF(75, 47) && b.bezier[1].in == QPointF(65, 27) && b.bezier[1].out == QPointF(85, 67)
        && b.bezier[0].p == QPointF(10, 50) && b.bezier[0].out == QPointF(30, 30);
    doc.undo();
    bool undone = doc.paths(l)[0].bezier[1].p == QPointF(70, 50) && doc.paths(l)[0].bezier[1].in == QPointF(60, 30);
    return after && undone && doc.addKnot(l, 0, {40, 45}) && doc.knots(l, 0).size() == 3;
}

static bool bezierHandleMovesPerType() {
    BezierNode c{{50, 50}, {40, 50}, {60, 50}, BezierType::Cusp};
    auto a = moveHandle(c, true, {60, 40});
    bool cusp = a.out == QPointF(60, 40) && a.in == QPointF(40, 50) && a.p == QPointF(50, 50);
    BezierNode s{{50, 50}, {30, 50}, {60, 50}, BezierType::Smooth};  // in is 20 long, out 10
    auto b = moveHandle(s, true, {50, 60});
    bool smooth = b.out == QPointF(50, 60) && std::abs(b.in.x() - 50) < 1e-9 && std::abs(b.in.y() - 30) < 1e-9;
    BezierNode y{{50, 50}, {40, 50}, {60, 50}, BezierType::Symmetric};
    auto d = moveHandle(y, false, {45, 70});
    bool sym = d.in == QPointF(45, 70) && d.out == QPointF(55, 30);
    Document doc(QSize(100, 100));
    int l = doc.addVectorLayer();
    Document::VPath v{{}, false, Qt::black, 4, {{0, 4}, {1, 4}}, Document::PathKind::Bezier};
    v.bezier = {{{10, 50}, {10, 50}, {30, 30}, BezierType::Cusp}, {{70, 50}, {60, 30}, {80, 70}, BezierType::Symmetric}};
    doc.restorePaths(l, {v});
    auto w = v;
    w.bezier[1] = moveHandle(w.bezier[1], false, {70, 20});
    doc.previewPath(l, 0, w);
    bool live = doc.paths(l)[0].bezier[1].out == QPointF(70, 80) && !doc.undo();
    return cusp && smooth && sym && live;
}

// Retyping Bezier nodes keeps the out handle (or the in, when out sits on the point) and turns the other to match (#314).
static bool bezierRetypeAlignsHandles() {
    std::vector<BezierNode> n = {{{50, 50}, {40, 40}, {60, 50}, BezierType::Cusp}, {{90, 50}, {80, 50}, {90, 50}, BezierType::Cusp}};
    auto s = retypeBezier(n, {0}, BezierType::Smooth);
    bool smooth = s[0].type == BezierType::Smooth && s[0].out == QPointF(60, 50) && std::abs(s[0].in.x() - (50 - std::sqrt(200.0))) < 1e-9
        && std::abs(s[0].in.y() - 50) < 1e-9 && s[1].type == BezierType::Cusp;
    auto y = retypeBezier(n, {0, 1}, BezierType::Symmetric);
    bool sym = y[0].in == QPointF(40, 50) && y[1].in == QPointF(80, 50) && y[1].out == QPointF(100, 50) && y[1].type == BezierType::Symmetric;
    auto c = retypeBezier(n, {0}, BezierType::Cusp);
    return smooth && sym && c[0].in == QPointF(40, 40);
}

// Closing a Bezier path with a press-drag on the first node (#368): drag to (20,8) pulls node 0's out to (20,8), in to (-4,8),
// Symmetric; the path finishes closed with those handles. Without a drag node 0 stays a Cusp.
static bool bezierPenCloseDragPullsFirstNode() {
    Document doc(QSize(64, 64));
    doc.addVectorLayer();
    SpiroPen pen;
    pen.bezier = true;
    for (QPointF p : {QPointF(8, 8), QPointF(56, 8), QPointF(56, 56)}) pen.click(p);
    pen.closing = true;
    pen.drag({20, 8});
    bool done = pen.finish(doc, true, Qt::black, 6);
    const auto& b = doc.paths(1)[0].bezier;
    return done && doc.paths(1)[0].closed && b.size() == 3 && b[0].type == BezierType::Symmetric && b[0].out == QPointF(20, 8) && b[0].in == QPointF(-4, 8)
        && b[2].type == BezierType::Cusp && !pen.closing;
}

// Retyping a node whose handles both sit on its point (an undragged pen click, as node 0 usually is) pulls them out along the line
// from the previous to the next node, a third of the way to each (#368). Square (0,0) (30,0) (30,30) (0,30), closed: node 0's
// neighbours are (0,30) and (30,0), so Smooth gives handles 10 long on the diagonal: out (7.07,-7.07), in (-7.07,7.07). Open, node 0 has only (30,0): out (10,0), in (-10,0).
static bool bezierRetypePullsCollapsedHandles() {
    std::vector<BezierNode> n;
    for (QPointF p : {QPointF(0, 0), QPointF(30, 0), QPointF(30, 30), QPointF(0, 30)}) n.push_back({p, p, p, BezierType::Cusp});
    auto c = retypeBezier(n, {0}, BezierType::Smooth, true);
    double h = 10 / std::sqrt(2.0);
    bool closed = c[0].type == BezierType::Smooth && QLineF(c[0].out, QPointF(h, -h)).length() < 1e-9 && QLineF(c[0].in, QPointF(-h, h)).length() < 1e-9;
    auto o = retypeBezier(n, {0}, BezierType::Symmetric, false);
    bool open = o[0].out == QPointF(10, 0) && o[0].in == QPointF(-10, 0);
    auto k = retypeBezier(n, {0}, BezierType::Cusp, true);
    return closed && open && k[0].out == QPointF(0, 0) && k[0].in == QPointF(0, 0);
}

// A Width Knot drag moves its point by an eighth of the pointer's motion, with no jump on press (#374).
static bool knotDragIsScaled() {
    if (knotDragTarget({10, 10}, {100, 50}, {100, 50}) != QPointF(10, 10)) return false;
    return knotDragTarget({10, 10}, {100, 50}, {140, 30}) == QPointF(15, 7.5);
}

// Deleting Bezier nodes (#311): removing the middle of three joins its neighbours (their handles stay) and a knot past the new end
// moves to it; removing down to one node is refused.
static bool removeBezierJoinsNeighbours() {
    Document::VPath v{{}, false, Qt::black, 4, {{0, 4}, {1.5, 6}, {2, 4}}, Document::PathKind::Bezier};
    v.bezier = {{{10, 50}, {10, 50}, {20, 20}, BezierType::Cusp}, {{40, 20}, {30, 20}, {50, 20}, BezierType::Smooth}, {{70, 50}, {60, 20}, {70, 50}, BezierType::Cusp}};
    auto r = removeBezier(v, {1});
    if (!r || r->bezier.size() != 2 || r->bezier[0].out != QPointF(20, 20) || r->bezier[1].in != QPointF(60, 20)) return false;
    if (r->knots.size() != 3 || r->knots[1].t != 1 || r->knots[2].t != 1) return false;
    return !removeBezier(v, {0, 1}) && !removeBezier(v, {0, 1, 2});
}

// Splitting a Bezier path (#312): the arch (10,50) (30,10) (50,10) (70,50) split where it passes (40,20) gets a Smooth node there with
// handles (30,20) and (50,20) (the halves' tangents), the neighbours' handles shorten to (20,30) and (60,30), the path still passes
// through every point it did, and each Width Knot keeps its handle point (knots 0, .25, .75, 1 move to 0, .5, 1.5, 2).
static bool splitBezierKeepsTheShape() {
    Document doc(QSize(100, 100));
    int l = doc.addVectorLayer();
    Document::VPath v{{}, false, Qt::black, 4, {{0, 4}, {0.25, 6}, {0.75, 8}, {1, 4}}, Document::PathKind::Bezier};
    v.bezier = {{{10, 50}, {10, 50}, {30, 10}, BezierType::Cusp}, {{70, 50}, {50, 10}, {70, 50}, BezierType::Cusp}};
    doc.restorePaths(l, {v});
    auto before = doc.knotHandles(l, 0);
    auto k = doc.projectKnot(l, 0, {40, 20});
    auto s = k ? splitBezier(v, k->t) : std::nullopt;
    if (!s || s->bezier.size() != 3) return false;
    const auto& b = s->bezier;
    if (QLineF(b[1].p, {40, 20}).length() > 1e-3 || QLineF(b[1].in, {30, 20}).length() > 1e-3 || QLineF(b[1].out, {50, 20}).length() > 1e-3 || b[1].type != BezierType::Smooth) return false;
    if (QLineF(b[0].out, {20, 30}).length() > 1e-3 || QLineF(b[2].in, {60, 30}).length() > 1e-3) return false;
    auto at = [](const QPainterPath& c, int i) { return c.pointAtPercent(i / 40.0); };
    QPainterPath was = Document::curve(v), now = Document::curve(*s);
    for (int i = 0; i <= 40; i++) {
        double best = 1e9;
        for (int j = 0; j <= 400; j++) best = std::min(best, QLineF(at(was, i), now.pointAtPercent(j / 400.0)).length());
        if (best > 0.1) return false;
    }
    doc.restorePaths(l, std::vector<Document::VPath>{*s});
    auto after = doc.knotHandles(l, 0);
    if (after.size() != before.size() || std::abs(s->knots[1].t - 0.5) > 1e-3 || std::abs(s->knots[2].t - 1.5) > 1e-3 || s->knots[3].t != 2) return false;
    for (size_t i = 0; i < before.size(); i++)
        if (QLineF(before[i], after[i]).length() > 0.1) return false;
    return !splitBezier(v, 0) && !splitBezier(v, 1);  // on a node: refused
}

// Alpha Lock (#427): set, undo/redo, a same-value call and a Vector Layer push no step, and a locked layer deleted then undone is locked.
static bool alphaLockFlagIsOneUndoStep() {
    Document doc(QSize(16, 16));
    doc.addLayer();
    int v = doc.addVectorLayer();
    if (doc.alphaLock(1) || !doc.setAlphaLock(1, true) || !doc.alphaLock(1)) return false;
    int id = doc.stepId();
    doc.setAlphaLock(1, true);
    if (doc.stepId() != id || doc.setAlphaLock(v, true) || doc.alphaLock(v) || doc.stepId() != id) return false;
    doc.undo();
    if (doc.alphaLock(1)) return false;
    doc.redo();
    if (!doc.alphaLock(1)) return false;
    doc.deleteLayer(1);
    doc.undo();
    return doc.alphaLock(1);
}

// Alpha Lock (#427): stroke, fill and erase on a locked layer recolor but keep every alpha; Delete still clears.
static bool alphaLockHoldsAlphaUnderPaint() {
    auto pencil = loadBrush(TEST_PENCIL);
    if (!pencil) return false;
    std::vector<PenSample> samples;
    for (int i = 0; i < 16; i++) samples.push_back({4.f + 3.8f * i, 32.f + 12.f * std::sin(i * 0.5f), 1.f, 0, 0, 0.01});
    const std::vector<QPointF> across = {{20, 10}, {44, 10}, {44, 54}, {20, 54}};  // straddles the opaque edge at x=32
    auto alphas = [](const QImage& im) {
        QImage a(im.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < im.height(); y++)
            for (int x = 0; x < im.width(); x++) a.scanLine(y)[x] = im.constScanLine(y)[4 * x + 3];
        return a;
    };
    Document doc(QSize(64, 64));
    doc.addLayer();
    doc.fill(Qt::red, {{0, 0}, {32, 0}, {32, 64}, {0, 64}});  // opaque left half, transparent right
    QImage before = alphas(doc.image(1));
    doc.setAlphaLock(1, true);
    doc.stroke(pencil, samples);
    bool strokeOk = alphas(doc.image(1)) == before && doc.image(1).pixelColor(30, 27) != QColor(255, 0, 0);
    doc.fill(Qt::blue, across);
    bool fillOk = alphas(doc.image(1)) == before && doc.image(1).pixelColor(24, 30).blue() > 200 && doc.image(1).pixelColor(40, 30).alpha() == 0;
    doc.fill(Qt::black, across, true);
    bool eraseOk = alphas(doc.image(1)) == before && doc.image(1).pixelColor(24, 30).red() > 250;
    bool deleteOk = doc.clearRect(QRect(0, 0, 64, 64)) && doc.image(1).pixelColor(10, 10).alpha() == 0;
    return strokeOk && fillOk && eraseOk && deleteOk;
}

// Inherit Alpha (#429): an item's alpha is cut by everything visible below it (opacity included); the Background ignores the flag; one undo step.
static bool inheritAlphaCutsByWhatIsBelow() {
    const std::vector<QPointF> all = {{0, 0}, {64, 0}, {64, 64}, {0, 64}};
    auto a = [](const Document& d, int x) { return d.render().pixelColor(x, 10).alpha(); };
    Document doc(QSize(64, 64));
    doc.addLayer();
    doc.fill(Qt::red, {{0, 0}, {32, 0}, {32, 64}, {0, 64}});  // opaque left half
    doc.addLayer();
    doc.fill(Qt::blue, all);
    doc.setVisible(0, false);
    doc.setInheritAlpha(2, true);
    if (!doc.inheritAlpha(2) || a(doc, 10) != 255 || a(doc, 50) != 0) return false;  // 1: shows only inside the shape
    doc.setOpacity(1, 0.5);
    if (std::abs(a(doc, 10) - 128) > 1 || a(doc, 50) != 0) return false;  // 2: base at 50%
    doc.setVisible(1, false);
    if (a(doc, 10) != 0) return false;  // 2: hidden base adds nothing
    doc.setVisible(1, true);
    doc.setOpacity(1, 1);
    doc.setVisible(0, true);
    QImage on = doc.render();
    doc.setInheritAlpha(2, false);
    if (doc.render() != on) return false;  // 3: Background visible, the flag changes nothing
    int id = doc.stepId();
    doc.setInheritAlpha(2, false);
    if (doc.stepId() != id) return false;
    doc.setInheritAlpha(0, true);  // 5: the Background ignores it, one undo step
    if (doc.render() != on || doc.stepId() != id + 1) return false;
    doc.undo();
    if (doc.inheritAlpha(0)) return false;
    // 4: a Vector Layer clips and is clipped, its paths untouched
    using T = SpiroType;
    Document v(QSize(64, 64));
    v.setVisible(0, false);
    v.addVectorLayer();
    v.addPath({{10, 10, T::G2}, {30, 10, T::G2}, {30, 25, T::G2}, {10, 25, T::G2}}, true, QColor(10, 20, 30), 4, Document::PaintMode::Fill, QColor(10, 20, 30));
    auto paths = v.paths(1);
    v.addLayer();
    v.fill(Qt::blue, all);
    v.setInheritAlpha(2, true);  // clipped by the vector shape
    if (a(v, 20) != 255 || a(v, 50) != 0) return false;
    v.setInheritAlpha(2, false);
    v.setInheritAlpha(1, true);  // the vector layer clipped by the raster layer's full coverage: unchanged
    v.setVisible(2, false);
    if (a(v, 20) != 0) return false;  // nothing below it
    return v.paths(1).size() == paths.size() && v.paths(1)[0].nodes.size() == paths[0].nodes.size();
}

// #536: references live beside the layers; add and replace are one undo step each, preview none, and render() never sees them.
static bool referencesAreOneUndoStepEachAndNeverRendered() {
    Document doc(QSize(32, 32));
    QImage before = doc.render();
    QImage pic(4, 4, QImage::Format_ARGB32_Premultiplied);
    pic.fill(Qt::red);
    Reference r{pic, QTransform::fromTranslate(2, 3), QRectF(0, 0, 4, 4), 0.5};
    doc.addReference(r);
    if (doc.references().size() != 1 || doc.references()[0].opacity != 0.5 || doc.render() != before) return false;
    QTemporaryDir dir;
    QString png = dir.filePath("a.png");
    if (!exportPng(doc, png) || QImage(png).convertToFormat(before.format()) != before) return false;
    Reference moved = r;
    moved.transform = QTransform::fromTranslate(9, 9);
    int id = doc.stepId();
    doc.previewReferences({moved});
    if (doc.stepId() != id || doc.references()[0].transform != moved.transform) return false;
    doc.setReferences({r});  // preview ends by restoring, then one replace
    doc.setReferences({moved});
    if (doc.stepId() != id + 2 || doc.layerCount() != 1) return false;
    doc.undo();
    if (doc.references()[0].transform != r.transform) return false;
    doc.undo();
    doc.undo();
    return doc.references().empty();
}

// #537: references draw from their original's pixels at the on-screen scale, hard past 1:1, cropped, at their opacity.
static bool referencesDrawFromTheOriginal() {
    QImage board(2000, 2000, QImage::Format_ARGB32_Premultiplied);  // one-pixel checkerboard
    for (int y = 0; y < 2000; y++)
        for (int x = 0; x < 2000; x++) board.setPixel(x, y, (x + y) % 2 ? 0xffffffff : 0xff000000);
    auto draw = [](const Reference& r, const QTransform& view) {
        QImage out(40, 40, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        drawReferences(p, {r}, view);
        return out;
    };
    // 1: placed at 0.1, viewed at 10x around document (50,50): every pixel alternates, as the original does
    Reference r{board, QTransform::fromScale(0.1, 0.1), QRectF(0, 0, 2000, 2000), 1};
    QTransform view = QTransform::fromTranslate(-50, -50) * QTransform::fromScale(10, 10);
    QImage a = draw(r, view);
    for (int x = 0; x < 39; x++)
        if (qGray(a.pixel(x, 20)) == qGray(a.pixel(x + 1, 20)) || (qGray(a.pixel(x, 20)) != 0 && qGray(a.pixel(x, 20)) != 255)) return false;
    // 2: enlarged 4x past 1:1, only pure black and white, in 4-pixel runs
    QImage b = draw(r, QTransform::fromScale(40, 40));
    for (int x = 0; x < 40; x++)
        if (qGray(b.pixel(x, 0)) != ((x / 4) % 2 ? 255 : 0)) return false;
    // 3: cropped to image x 0..2: columns past 8 px stay empty; 4: opacity 0.5 halves alpha
    Reference c{board, QTransform(), QRectF(0, 0, 2, 2000), 0.5};
    QImage d = draw(c, QTransform::fromScale(4, 4));
    return qAlpha(d.pixel(20, 0)) == 0 && std::abs(qAlpha(d.pixel(2, 0)) - 128) <= 1;
}

int main() {
    int failed = 0;
    auto run = [&](const char* name, bool (*fn)()) {
        bool ok = fn();
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    run("newDocumentRendersWhite", newDocumentRendersWhite);
    run("referencesDrawFromTheOriginal", referencesDrawFromTheOriginal);
    run("referencesAreOneUndoStepEachAndNeverRendered", referencesAreOneUndoStepEachAndNeverRendered);
    run("alphaLockFlagIsOneUndoStep", alphaLockFlagIsOneUndoStep);
    run("inheritAlphaCutsByWhatIsBelow", inheritAlphaCutsByWhatIsBelow);
    run("alphaLockHoldsAlphaUnderPaint", alphaLockHoldsAlphaUnderPaint);
    run("bezierNodesDragWithHandles", bezierNodesDragWithHandles);
    run("bezierHandleMovesPerType", bezierHandleMovesPerType);
    run("bezierRetypeAlignsHandles", bezierRetypeAlignsHandles);
    run("bezierPenCloseDragPullsFirstNode", bezierPenCloseDragPullsFirstNode);
    run("bezierRetypePullsCollapsedHandles", bezierRetypePullsCollapsedHandles);
    run("penPaintModeGoesOnNextPath", penPaintModeGoesOnNextPath);
    run("penFillSwatchGoesOnNextPath", penFillSwatchGoesOnNextPath);
    run("viewMapsAndRoundTrips", viewMapsAndRoundTrips);
    run("viewOpsKeepAnchorFixed", viewOpsKeepAnchorFixed);
    run("viewScreenRadiusFollowsZoomOnly", viewScreenRadiusFollowsZoomOnly);
    run("stabilizerAveragesAndFinishesAtThePen", stabilizerAveragesAndFinishesAtThePen);
    run("viewReportsAndSetsZoomRotation", viewReportsAndSetsZoomRotation);
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
    run("stepIdsTrackTheHistory", stepIdsTrackTheHistory);
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
    run("oraRoundTripsAlphaFlags", oraRoundTripsAlphaFlags);
    run("oraRoundTripsVectorLayer", oraRoundTripsVectorLayer);
    run("oraRoundTripsPaintMode", oraRoundTripsPaintMode);
    run("oraRoundTripsBezier", oraRoundTripsBezier);
    run("svgFollowsPaintMode", svgFollowsPaintMode);
    run("exportedPngEqualsRender", exportedPngEqualsRender);
    run("vectorPathRendersAsPowerStroke", vectorPathRendersAsPowerStroke);
    run("bezierPathRendersAsPowerStroke", bezierPathRendersAsPowerStroke);
    run("closedPathIsARing", closedPathIsARing);
    run("setKnotsRedrawsAndUndoes", setKnotsRedrawsAndUndoes);
    run("setNodesRedrawsAndUndoes", setNodesRedrawsAndUndoes);
    run("selectingAPathShowsItsKnotHandles", selectingAPathShowsItsKnotHandles);
    run("wideStrokeIsHitInsideItsBody", wideStrokeIsHitInsideItsBody);
    run("closedPathIsHitInsideAndThinPathStaysHittable", closedPathIsHitInsideAndThinPathStaysHittable);
    run("nodeAtFindsTheNodeUnderAPoint", nodeAtFindsTheNodeUnderAPoint);
    run("draggingAKnotProjectsAndUndoesOnce", draggingAKnotProjectsAndUndoesOnce);
    run("addNodeSplitsTheSegmentUnderTheClick", addNodeSplitsTheSegmentUnderTheClick);
    run("addingAKnotKeepsTheStrokeAndUndoesOnce", addingAKnotKeepsTheStrokeAndUndoesOnce);
    run("duplicatingAndDeletingKnots", duplicatingAndDeletingKnots);
    run("draggingNodesMovesThemTogetherAndUndoesOnce", draggingNodesMovesThemTogetherAndUndoesOnce);
    run("deletingAndRetypingNodes", deletingAndRetypingNodes);
    run("transformNodesMapsNodesAndKeepsTheRest", transformNodesMapsNodesAndKeepsTheRest);
    run("transformNodesMapsBezierHandles", transformNodesMapsBezierHandles);
    run("transformBoxRotatesAndScalesAlongItsAxes", transformBoxRotatesAndScalesAlongItsAxes);
    run("flipMirrorsAndScaleWidthsIsOptIn", flipMirrorsAndScaleWidthsIsOptIn);
    run("rewidthPathScalesKnots", rewidthPathScalesKnots);
    run("transformRectMovesPixelsAndUndoesOnce", transformRectMovesPixelsAndUndoesOnce);
    run("spiroPenPlacesNodesAndFinishes", spiroPenPlacesNodesAndFinishes);
    run("bezierPenPlacesCornersAndFinishes", bezierPenPlacesCornersAndFinishes);
    run("bezierPenDragMakesSymmetricNode", bezierPenDragMakesSymmetricNode);
    run("toBezierKeepsOutlineAndKnots", toBezierKeepsOutlineAndKnots);
    run("pickerRowsSetTheNextNodeType", pickerRowsSetTheNextNodeType);
    run("cornerClickIgnoresThePickerType", cornerClickIgnoresThePickerType);
    run("toolFollowsLayerKind", toolFollowsLayerKind);
    run("settingPathsUndoesOnce", settingPathsUndoesOnce);
    run("rasterizingAVectorLayerUndoesOnce", rasterizingAVectorLayerUndoesOnce);
    run("pathsPaintByMode", pathsPaintByMode);
    run("hitsFollowMode", hitsFollowMode);
    run("boxPicksThePathsItTouches", boxPicksThePathsItTouches);
    run("clearingARectangleUndoesOnce", clearingARectangleUndoesOnce);
    run("transformBoxBoundsAndMovesPickedPaths", transformBoxBoundsAndMovesPickedPaths);
    run("transformBoxScalesAboutOppositeHandle", transformBoxScalesAboutOppositeHandle);
    run("knotDragIsScaled", knotDragIsScaled);
    run("removeBezierJoinsNeighbours", removeBezierJoinsNeighbours);
    run("splitBezierKeepsTheShape", splitBezierKeepsTheShape);
    return failed != 0;
}
