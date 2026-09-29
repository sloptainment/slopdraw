#include <array>
#include <cmath>
#include <cstdio>
#include "brush.h"
#include "document.h"
#include "spiro.h"
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
    run("spiroMatchesReference", spiroMatchesReference);
    run("spiroFailsInsteadOfGarbage", spiroFailsInsteadOfGarbage);
    run("bundledPencilLoads", bundledPencilLoads);
    run("replayedPencilStrokeMatchesGolden", replayedPencilStrokeMatchesGolden);
    run("liveFedPencilStrokeMatchesGolden", liveFedPencilStrokeMatchesGolden);
    run("undoRestoresAndRedoReapplies", undoRestoresAndRedoReapplies);
    run("newStrokeAfterUndoDiscardsRedo", newStrokeAfterUndoDiscardsRedo);
    run("liveFedStrokeIsOneUndoStep", liveFedStrokeIsOneUndoStep);
    run("budgetDropsOldestKeepsNewest", budgetDropsOldestKeepsNewest);
    return failed != 0;
}
