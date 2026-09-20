// Generates the checked-in PDF fixtures in this directory. A build image
// without fonts draws no text layer, so the tests read these committed files
// instead of writing their own.
//
//   g++ -std=c++20 -fPIC -no-pie generate.cpp -o /tmp/mkpdf \
//       $(pkg-config --cflags --libs Qt6Gui Qt6Core)
//   QT_QPA_PLATFORM=offscreen /tmp/mkpdf tests/fixtures/pdf
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <cstdio>

namespace {

// Letter, one text line per page, for the viewer tests: two pages give the
// continuous list something to scroll.
void letterPages(const QString& path) {
  QPdfWriter writer(path);
  writer.setResolution(96);
  writer.setPageSize(QPageSize(QPageSize::Letter));
  QPainter painter(&writer);
  if (!painter.isActive()) {
    std::fprintf(stderr, "painter inactive\n");
    return;
  }
  painter.drawText(QPoint(100, 140), QStringLiteral("Omaroll selection"));
  writer.newPage();
  painter.drawText(QPoint(100, 140), QStringLiteral("Second page"));
  painter.end();
  std::printf("wrote %s\n", qPrintable(path));
}

// A4, one page, two text lines far enough apart to select one of them.
void textPage(const QString& path) {
  QPdfWriter writer(path);
  writer.setResolution(96);
  writer.setPageSize(QPageSize(QPageSize::A4));
  QPainter painter(&writer);
  if (!painter.isActive()) {
    std::fprintf(stderr, "painter inactive\n");
    return;
  }
  painter.drawText(QPoint(100, 140), QStringLiteral("Omaroll selection"));
  painter.drawText(QPoint(100, 200), QStringLiteral("Second line"));
  painter.end();
  std::printf("wrote %s\n", qPrintable(path));
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  if (argc < 2) {
    std::fprintf(stderr, "usage: mkpdf <output directory>\n");
    return 2;
  }
  const QString dir = QString::fromLocal8Bit(argv[1]);
  textPage(dir + QStringLiteral("/text-page.pdf"));
  letterPages(dir + QStringLiteral("/letter-pages.pdf"));
  return 0;
}
