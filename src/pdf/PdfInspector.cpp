#include "pdf/PdfInspector.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

PdfInspector::PdfInspector(QObject* parent) : QObject(parent) {
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(10'000);
  connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
  connect(&m_process, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_timeout.stop();
            m_loading = false;
            if (status != QProcess::NormalExit || exitCode != 0) {
              m_error = QString::fromLocal8Bit(m_process.readAllStandardError()).trimmed();
              if (m_error.isEmpty()) {
                m_error = QStringLiteral("Could not read PDF details");
              }
              emit changed();
              return;
            }
            const QString output = QString::fromLocal8Bit(m_process.readAllStandardOutput());
            static const QRegularExpression pages(
                QStringLiteral(R"(^Pages:\s+(\d+)\s*$)"),
                QRegularExpression::MultilineOption);
            const auto match = pages.match(output);
            m_pageCount = match.hasMatch() ? match.captured(1).toInt() : 0;
            if (m_pageCount <= 0) {
              m_error = QStringLiteral("This PDF has no readable pages");
            }
            emit changed();
          });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      m_timeout.stop();
      m_loading = false;
      m_error = QStringLiteral("PDF support needs Poppler");
      emit changed();
    }
  });
}

PdfInspector::~PdfInspector() {
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
}

bool PdfInspector::available() const {
  return !QStandardPaths::findExecutable(QStringLiteral("pdfinfo")).isEmpty();
}

void PdfInspector::inspect(const QString& path) {
  if (m_path == path && (m_loading || m_pageCount > 0)) {
    return;
  }
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
  m_path = path;
  m_pageCount = 0;
  m_error.clear();
  if (!QFileInfo(path).isFile()) {
    m_loading = false;
    m_error = QStringLiteral("That PDF is no longer available");
    emit changed();
    return;
  }
  m_loading = true;
  emit changed();
  m_process.start(QStandardPaths::findExecutable(QStringLiteral("pdfinfo")), {path});
  m_timeout.start();
}

void PdfInspector::clear() {
  m_timeout.stop();
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
  }
  m_path.clear();
  m_pageCount = 0;
  m_loading = false;
  m_error.clear();
  emit changed();
}
