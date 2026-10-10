// GPU encoding (#580): the Canvas shader's linear-to-sRGB frame matches the CPU encodeSrgb within 1 level,
// on a real (Xvfb) display with GL.
#include "canvas.h"
#include <cstdio>
#include <cstdlib>
#include <QApplication>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Largest per-channel difference between the Canvas's frame and encodeSrgb(render()), at view identity.
static int worstDiff(bool floatPoint)
{
  QSize size(400, 300);
  Document doc(size, qsizetype(1) << 30, floatPoint);
  QImage ramp(size, QImage::Format_RGB32);
  for (int y = 0; y < size.height(); ++y)
    for (int x = 0; x < size.width(); ++x)
      ramp.setPixel(x, y, qRgb(x * 255 / 399, y * 255 / 299, 128));
  doc.pasteImage(ramp, QTransform());
  doc.fill(QColor(128, 128, 128, 128), {{50, 50}, {200, 50}, {200, 200}, {50, 200}}); // 50% grey dab
  QImage want = encodeSrgb(doc.render());
  Canvas canvas;
  canvas.setDocument(std::move(doc));
  static_cast<QWidget &>(canvas).resize(size);
  canvas.show();
  QImage got = canvas.grabFramebuffer().convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (got.size() != size)
    return 255;
  int worst = 0;
  for (int y = 0; y < size.height(); ++y)
    for (int x = 0; x < size.width(); ++x)
    {
      QRgb a = got.pixel(x, y), b = want.pixel(x, y);
      for (int s : {0, 8, 16})
        worst = std::max(worst, std::abs(int((a >> s) & 255) - int((b >> s) & 255)));
    }
  return worst;
}

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  CHECK(worstDiff(false) <= 1); // Integer
  CHECK(worstDiff(true) <= 1);  // Float-point
  std::printf(failures ? "FAILED\n" : "ok\n");
  return failures;
}
