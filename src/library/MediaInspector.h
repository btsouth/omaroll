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

public:
  explicit MediaInspector(QObject* parent = nullptr);
  ~MediaInspector() override;

  [[nodiscard]] QString path() const { return m_path; }
  [[nodiscard]] QStringList lines() const { return m_lines; }
  [[nodiscard]] bool loading() const { return m_loading; }

  Q_INVOKABLE void inspect(const QString& path, bool video);

  // Kept pure so unusual producer output can be covered without starting a
  // process in every parser test.
  [[nodiscard]] static QStringList parseImage(const QByteArray& output);
  [[nodiscard]] static QStringList parseVideo(const QByteArray& output,
                                              const QString& suffix);

signals:
  void detailsChanged();
  void loadingChanged();

private:
  void cancelCurrent();
  void setLoading(bool loading);

  QString m_path;
  QStringList m_lines;
  QPointer<QProcess> m_process;
  quint64 m_generation = 0;
  bool m_loading = false;
};
