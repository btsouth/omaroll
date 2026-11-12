#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class QProcess;

// Reads technical details for the one file open in the viewer.
//
// General-library scans stay metadata-only and fast. ImageMagick or ffprobe is
// started only when a viewer opens, with an argv rather than a shell command.
// A newer inspection cancels the old one, so quick arrow navigation cannot
// show details from the previous file.
class MediaInspector final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY detailsChanged)
  Q_PROPERTY(QStringList lines READ lines NOTIFY detailsChanged)
  Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
  // Where it was taken, as decimal "latitude, longitude", or empty. Read
  // from the file only; nothing is looked up.
  Q_PROPERTY(QString coordinates READ coordinates NOTIFY detailsChanged)

public:
  explicit MediaInspector(QObject* parent = nullptr);
  ~MediaInspector() override;

  [[nodiscard]] QString path() const { return m_path; }
  [[nodiscard]] QStringList lines() const { return m_lines; }
  [[nodiscard]] bool loading() const { return m_loading; }
  [[nodiscard]] QString coordinates() const { return m_coordinates; }

  Q_INVOKABLE void inspect(const QString& path, bool video);
  // Whether inspect also reads the file's keywords, rating and description
  // (embeddedRead). Follows AppSettings::readEmbeddedMetadata.
  void setReadEmbedded(bool value) { m_readEmbedded = value; }

  // Kept pure so unusual producer output can be covered without starting a
  // process in every parser test.
  [[nodiscard]] static QStringList parseImage(const QByteArray& output);
  [[nodiscard]] static QStringList parseVideo(const QByteArray& output,
                                              const QString& suffix);
  // ImageMagick reports no EXIF for camera raws; their own headers are read
  // in-process instead, into the same lines.
  [[nodiscard]] static QStringList describeRaw(const QString& path);
  // The coordinates in ImageMagick's output for parseImage(), or in
  // ffprobe's for parseVideo(), as coordinates() gives them.
  [[nodiscard]] static QString imageCoordinates(const QByteArray& output);
  [[nodiscard]] static QString videoCoordinates(const QByteArray& output);
  // "47.6062, -122.3321": six places at most, trailing zeros dropped. Empty
  // outside the globe's range.
  [[nodiscard]] static QString coordinateText(double latitude, double longitude);

signals:
  void detailsChanged();
  void loadingChanged();
  // What the inspected file and its sidecar hold, read on a worker.
  void embeddedRead(const QString& path, const QStringList& keywords, int rating,
                    const QString& description);

private:
  void cancelCurrent();
  void setLoading(bool loading);

  QString m_path;
  QStringList m_lines;
  QString m_coordinates;
  QPointer<QProcess> m_process;
  quint64 m_generation = 0;
  bool m_loading = false;
  bool m_readEmbedded = true;
};
