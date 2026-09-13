#include "pdf/PdfInspector.h"

#include "pdf/PdfSupport.h"

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

  m_searchTimeout.setSingleShot(true);
  m_searchTimeout.setInterval(20'000);
  connect(&m_searchTimeout, &QTimer::timeout, &m_searchProcess, &QProcess::kill);
  connect(&m_searchProcess, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_searchTimeout.stop();
            if (status != QProcess::NormalExit || exitCode != 0) {
              return;
            }
            const QString text = QString::fromUtf8(m_searchProcess.readAllStandardOutput());
            const QList<int> pages = PdfSupport::findPages(text, m_query);
            m_matches.clear();
            for (const int page : pages) {
              m_matches.append(page);
            }
            emit matchesChanged();
          });
}

PdfInspector::~PdfInspector() {
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
  if (m_searchProcess.state() != QProcess::NotRunning) {
    m_searchProcess.kill();
    m_searchProcess.waitForFinished(1000);
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
  clearSearch();
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
  clearSearch();
  m_path.clear();
  m_pageCount = 0;
  m_loading = false;
  m_error.clear();
  emit changed();
}

void PdfInspector::find(const QString& query) {
  m_query = query.simplified();
  m_matches.clear();
  emit matchesChanged();
  if (m_path.isEmpty() || m_query.isEmpty() || m_pageCount <= 0 ||
      !PdfSupport::textAvailable()) {
    return;
  }
  if (m_searchProcess.state() != QProcess::NotRunning) {
    m_searchProcess.kill();
    m_searchProcess.waitForFinished(500);
  }
  m_searchProcess.start(QStandardPaths::findExecutable(QStringLiteral("pdftotext")),
                        {QStringLiteral("-layout"), m_path, QStringLiteral("-")});
  m_searchTimeout.start();
}

void PdfInspector::clearSearch() {
  m_searchTimeout.stop();
  if (m_searchProcess.state() != QProcess::NotRunning) {
    m_searchProcess.kill();
  }
  m_query.clear();
  if (!m_matches.isEmpty()) {
    m_matches.clear();
    emit matchesChanged();
  }
}
