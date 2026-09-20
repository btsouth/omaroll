// Generates the PDFs this repository ships: the two test fixtures and the demo
// library's document. A build image without fonts draws no text layer, so the
// tests read committed files instead of writing their own.
//
//   g++ -std=c++20 -fPIC -no-pie generate.cpp -o /tmp/mkpdf \
//       $(pkg-config --cflags --libs Qt6Gui Qt6Core)
//   QT_QPA_PLATFORM=offscreen /tmp/mkpdf tests/fixtures/pdf resources/demo
#include <QFont>
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStringList>
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

// The demo library's document: a heading, body text and a second page, so the
// page list, the find row and the page controls have something real to show.
void demoGuide(const QString& path) {
  QPdfWriter writer(path);
  writer.setResolution(96);
  writer.setPageSize(QPageSize(QPageSize::A4));
  QPainter painter(&writer);
  if (!painter.isActive()) {
    std::fprintf(stderr, "painter inactive\n");
    return;
  }

  QFont heading = painter.font();
  heading.setPointSize(20);
  heading.setBold(true);
  painter.setFont(heading);
  painter.drawText(QPoint(84, 140), QStringLiteral("Quiet Horizons"));

  QFont body = painter.font();
  body.setPointSize(11);
  body.setBold(false);
  painter.setFont(body);
  painter.drawText(QPoint(84, 176), QStringLiteral("A guide to the demo library"));

  const QStringList lines{
      QStringLiteral("Omaroll reads the folders Omarchy already writes captures to."),
      QStringLiteral("Pictures, screen recordings, downloads: whatever is there is"),
      QStringLiteral("listed by day, newest first, and nothing is imported or copied."),
      QString(),
      QStringLiteral("Open a picture and move through its folder with the arrows."),
      QStringLiteral("Select several files and they open in the order you picked."),
      QStringLiteral("Search matches names, captions and the text inside pictures."),
      QString(),
      QStringLiteral("Favourites, ratings, captions and albums are marks, not edits:"),
      QStringLiteral("they never touch the file, and Ctrl+Z steps back through them."),
  };
  int y = 236;
  for (const QString& line : lines) {
    if (!line.isEmpty()) {
      painter.drawText(QPoint(84, y), line);
    }
    y += 26;
  }

  writer.newPage();
  painter.drawText(QPoint(84, 140), QStringLiteral("Documents"));
  y = 176;
  const QStringList second{
      QStringLiteral("PDFs scroll at the window width, or fit a whole page."),
      QStringLiteral("Find searches the document, and Copy page text takes one page."),
      QStringLiteral("Select text takes just the words you drag over."),
      QStringLiteral("Every page is read locally through Poppler.")};
  for (const QString& line : second) {
    painter.drawText(QPoint(84, y), line);
    y += 26;
  }
  painter.end();
  std::printf("wrote %s\n", qPrintable(path));
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  if (argc < 2) {
    std::fprintf(stderr, "usage: mkpdf <fixture directory> [demo media directory]\n");
    return 2;
  }
  const QString fixtures = QString::fromLocal8Bit(argv[1]);
  textPage(fixtures + QStringLiteral("/text-page.pdf"));
  letterPages(fixtures + QStringLiteral("/letter-pages.pdf"));
  if (argc > 2) {
    demoGuide(QString::fromLocal8Bit(argv[2]) + QStringLiteral("/quiet-horizons-guide.pdf"));
  }
  return 0;
}
