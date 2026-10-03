#pragma once

#include <QFuture>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QStringList>

// A dependency-free background tool for images already in the library.
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
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString lastOutputPath READ lastOutputPath NOTIFY lastOutputPathChanged)

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

  [[nodiscard]] bool busy() const { return m_busy; }
  [[nodiscard]] QString lastOutputPath() const { return m_lastOutputPath; }

  Q_INVOKABLE QStringList matteNames() const;
  Q_INVOKABLE QStringList aspectNames() const;

  // Composes and writes "<name>-matte.png" beside the original, putting the
  // result on the clipboard too, off the GUI thread. saved() reports the disk
  // copy independently; composed() retains its meaning of saved AND copied.
  // finished() reports both outcomes, including partial success.
  Q_INVOKABLE void composeAndSave(const QString& path, int matte, int aspect, qreal paddingFraction);

  // Retry only the clipboard offer for lastOutputPath, without recomposing or
  // creating another numbered copy. Emits copied(), failed() and finished().
  Q_INVOKABLE void retryCopy();

  // Pure: used by the preview provider and by composeAndSave.
  [[nodiscard]] static QImage compose(const QImage& source, Matte matte, Aspect aspect,
                                      qreal paddingFraction);

signals:
  void busyChanged();
  void lastOutputPathChanged();
  void saved(const QString& outputPath);
  void copied(const QString& outputPath);
  void finished(const QString& outputPath, bool saved, bool copied);
  void composed(const QString& outputPath);
  void failed(const QString& message);

private:
  struct Result {
    QString outputPath;
    QString error;
    QImage fallbackImage;
    bool saved = false;
    bool copied = false;
  };
  static Result saveAndCopy(const QString& path, Matte matte, Aspect aspect, qreal paddingFraction,
                            bool qtFallback);
  static Result copySaved(const QString& path, bool saved, bool qtFallback);
  void watch(QFuture<Result> future, bool retry);
  bool m_busy = false;
  QString m_lastOutputPath;

  [[nodiscard]] static QImage paintBackground(const QSize& size, Matte matte,
                                              const QColor& seed);
  // Sizes the canvas around the padded content. When the pixel budget shrinks
  // the canvas, padding is shrunk with it so the proportions survive.
  [[nodiscard]] static QSize canvasFor(const QSize& content, Aspect aspect, int& padding);
};
