#include <array>
#include <cmath>
#include <cstdio>
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
    return failed != 0;
}
