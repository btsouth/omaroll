#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

class PdfInspector final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
  Q_PROPERTY(bool loading READ loading NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(bool available READ available CONSTANT)

public:
  explicit PdfInspector(QObject* parent = nullptr);
  ~PdfInspector() override;
  [[nodiscard]] QString path() const { return m_path; }
  [[nodiscard]] int pageCount() const { return m_pageCount; }
  [[nodiscard]] bool loading() const { return m_loading; }
  [[nodiscard]] QString error() const { return m_error; }
  [[nodiscard]] bool available() const;
  Q_INVOKABLE void inspect(const QString& path);
  Q_INVOKABLE void clear();

signals:
  void changed();

private:
  QProcess m_process;
  QTimer m_timeout;
  QString m_path;
  int m_pageCount = 0;
  bool m_loading = false;
  QString m_error;
};
