#include <cmath>
#include <cstdio>
#include "document.h"
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
    return failed != 0;
}
