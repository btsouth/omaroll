#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QStringList>

// The one thing omaroll builds itself, because nothing on Omarchy does it.
//
// Click a screenshot, get six finished backgrounds derived from the image's own
// dominant hue, pick one, and the composite is on the clipboard and saved beside
// the original. No editor in the flow: choosing between finished results
// replaces tweaking one.
//
// Non-destructive without exception. The source file is opened read-only and a
// composite is written as a new file next to it.
class MatteComposer final : public QObject {
  Q_OBJECT

public:
  enum Matte {
    Adaptive,  // light gradient from the extracted hue
    Deep,      // dark gradient from the same hue
    Aurora,    // soft mesh blobs
    Slate,     // neutral dark
    Paper,     // neutral light
    Pop,       // the complementary hue
    None,      // the raw capture, so the picker is never a tax
    MatteCount
  };
  Q_ENUM(Matte)

  enum Aspect {
    Original,
    Square,
    Wide,      // 16:9
    Social,    // 1.91:1
    AspectCount
  };
  Q_ENUM(Aspect)

  // Matches the budget matteshot settled on: a forced aspect on a very tall
  // capture must not be allowed to allocate an enormous canvas.
  static constexpr int kMaxOutputPixels = 9'400'000;

  explicit MatteComposer(QObject* parent = nullptr);

  Q_INVOKABLE QStringList matteNames() const;
  Q_INVOKABLE QStringList aspectNames() const;

  // Composes and writes "<name>-matte.png" beside the original, putting the
  // result on the clipboard too. Emits composed() or failed().
  Q_INVOKABLE void composeAndSave(const QString& path, int matte, int aspect, qreal paddingFraction);

  // Pure: used by the preview provider and by composeAndSave.
  [[nodiscard]] static QImage compose(const QImage& source, Matte matte, Aspect aspect,
                                      qreal paddingFraction);

signals:
  void composed(const QString& outputPath);
  void failed(const QString& message);

private:
  [[nodiscard]] static QImage paintBackground(const QSize& size, Matte matte,
                                              const QColor& seed);
  [[nodiscard]] static QSize canvasFor(const QSize& content, Aspect aspect, int padding);
};
