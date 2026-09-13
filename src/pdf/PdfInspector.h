#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVariantList>

class PdfInspector final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
  Q_PROPERTY(bool loading READ loading NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(bool available READ available CONSTANT)
  // Search needs pdftotext, which is a separate tool from the renderer.
  Q_PROPERTY(bool textSearchAvailable READ textSearchAvailable CONSTANT)
  // 1-based pages that contain the current search query.
  Q_PROPERTY(QVariantList matches READ matches NOTIFY matchesChanged)
  Q_PROPERTY(int matchCount READ matchCount NOTIFY matchesChanged)

public:
  explicit PdfInspector(QObject* parent = nullptr);
  ~PdfInspector() override;
  [[nodiscard]] QString path() const { return m_path; }
  [[nodiscard]] int pageCount() const { return m_pageCount; }
  [[nodiscard]] bool loading() const { return m_loading; }
  [[nodiscard]] QString error() const { return m_error; }
  [[nodiscard]] bool available() const;
  [[nodiscard]] bool textSearchAvailable() const;
  [[nodiscard]] QVariantList matches() const { return m_matches; }
  [[nodiscard]] int matchCount() const { return static_cast<int>(m_matches.size()); }
  Q_INVOKABLE void inspect(const QString& path);
  Q_INVOKABLE void clear();
  // Searches the inspected document with pdftotext, asynchronously.
  Q_INVOKABLE void find(const QString& query);
  Q_INVOKABLE void clearSearch();

signals:
  void changed();
  void matchesChanged();

private:
  QProcess m_process;
  QTimer m_timeout;
  QString m_path;
  int m_pageCount = 0;
  bool m_loading = false;
  QString m_error;

  QProcess m_searchProcess;
  QTimer m_searchTimeout;
  QVariantList m_matches;
  QString m_query;
};
