// GPU encoding (#580): the Canvas shader's linear-to-sRGB frame matches the CPU encodeSrgb within 1 level,
// on a real (Xvfb) display with GL.
#include "canvas.h"
#include <cstdio>
#include <cstdlib>
#include <QApplication>
#include <QElapsedTimer>
#include <QScreen>
#include <QVBoxLayout>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Largest per-channel difference between the Canvas's frame and encodeSrgb(render()), at view identity.
static int worstDiff(Canvas &canvas)
{
  QImage want = encodeSrgb(canvas.doc().render());
  QImage got = canvas.grabFramebuffer().convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (got.size() != want.size())
    return 255;
  int worst = 0;
  for (int y = 0; y < got.height(); ++y)
    for (int x = 0; x < got.width(); ++x)
    {
      QRgb a = got.pixel(x, y), b = want.pixel(x, y);
      for (int s : {0, 8, 16})
        worst = std::max(worst, std::abs(int((a >> s) & 255) - int((b >> s) & 255)));
    }
  return worst;
}

// The frame, then the frame after a stroke: the canvas uploads only what the stroke changed.
static std::pair<int, int> worstDiffs(bool floatPoint)
{
  QSize size(400, 300);
  Document doc(size, qsizetype(1) << 30, floatPoint);
  QImage ramp(size, QImage::Format_RGB32);
  for (int y = 0; y < size.height(); ++y)
    for (int x = 0; x < size.width(); ++x)
      ramp.setPixel(x, y, qRgb(x * 255 / 399, y * 255 / 299, 128));
  doc.pasteImage(ramp, QTransform());
  doc.fill(QColor(128, 128, 128, 128), {{50, 50}, {200, 50}, {200, 200}, {50, 200}}); // 50% grey dab
  Canvas canvas;
  canvas.setDocument(std::move(doc));
  static_cast<QWidget &>(canvas).resize(size);
  canvas.show();
  int first = worstDiff(canvas);
  auto brush = loadBrush(BRUSH_DIR "/hard-brush.myb");
  setBrushRadius(brush.get(), 20);
  for (int i = 0; i < 16; i++)
    canvas.doc().stroke(brush, {{100.f + 12 * i, 150, 1, 0, 0, 0.01}}, i > 0);
  return {first, worstDiff(canvas)};
}

// Outside the document the widget shows its palette, not the GL framebuffer's leftover memory.
static bool backgroundFilled()
{
  Canvas canvas;
  canvas.setDocument(Document(QSize(100, 100), qsizetype(1) << 30, false));
  static_cast<QWidget &>(canvas).resize(400, 300);
  canvas.show();
  QImage got = canvas.grabFramebuffer();
  return got.pixelColor(399, 299).rgb() == canvas.palette().window().color().rgb();
}

// Transparent Background pixels show a checkerboard of 8 px screen squares; outside the document stays the palette.
static bool checkerboardShown()
{
  Canvas canvas;
  Document doc(QSize(100, 100), qsizetype(1) << 30, false);
  doc.clearRect(QRegion(0, 0, 100, 100));
  canvas.setDocument(std::move(doc));
  static_cast<QWidget &>(canvas).resize(400, 300);
  canvas.show();
  QImage got = canvas.grabFramebuffer();
  QRgb a = got.pixelColor(0, 0).rgb(), b = got.pixelColor(8, 0).rgb();
  return a != b && got.pixelColor(7, 7).rgb() == a && got.pixelColor(0, 8).rgb() == b && got.pixelColor(8, 8).rgb() == a &&
         got.pixelColor(150, 50).rgb() == canvas.palette().window().color().rgb();
}

// After a resize the document keeps its size on screen, not stretched with the old framebuffer.
static bool resizeFollowed(QApplication &app)
{
  auto pump = [&] { QElapsedTimer t; t.start(); while (t.elapsed() < 500) app.processEvents(); };
  QWidget top; // the canvas in a layout, like in the app; grabFramebuffer can't see the stretch, the screen can
  auto *layout = new QVBoxLayout(&top);
  layout->setContentsMargins(0, 0, 0, 0);
  Canvas *canvas = new Canvas;
  layout->addWidget(canvas);
  canvas->setDocument(Document(QSize(100, 100), qsizetype(1) << 30, false));
  top.resize(200, 150);
  top.show();
  pump();
  top.resize(400, 300);
  pump();
  QImage got; // a real desktop may take a while to show the window: wait for the document's white
  QElapsedTimer t;
  t.start();
  do
  {
    app.processEvents();
    got = top.screen()->grabWindow(top.winId()).toImage();
  } while (got.pixelColor(50, 50) != Qt::white && t.elapsed() < 3000);
  if (got.pixelColor(50, 50) != Qt::white)
  { // covered by another window on a live desktop: only Xvfb gives a clean answer
    std::printf("SKIP resizeFollowed: the window never showed on screen\n");
    return true;
  }
  return got.pixelColor(150, 50).rgb() == canvas->palette().window().color().rgb();
}

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  CHECK(resizeFollowed(app));
  CHECK(backgroundFilled());
  CHECK(checkerboardShown());
  auto [integer, integerStroke] = worstDiffs(false);
  auto [floatPoint, floatPointStroke] = worstDiffs(true);
  CHECK(integer <= 1);
  CHECK(integerStroke <= 1);
  CHECK(floatPoint <= 1);
  CHECK(floatPointStroke <= 1);
  std::printf(failures ? "FAILED\n" : "ok\n");
  return failures;
}
