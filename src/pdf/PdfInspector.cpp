#include "pdf/PdfInspector.h"

#include "edit/ClipboardText.h"
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

  m_textTimeout.setSingleShot(true);
  m_textTimeout.setInterval(20'000);
  connect(&m_textTimeout, &QTimer::timeout, &m_textProcess, &QProcess::kill);
  connect(&m_textProcess, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_textTimeout.stop();
            if (status != QProcess::NormalExit || exitCode != 0) {
              emit textCopyFailed(QStringLiteral("Could not read this page"));
              return;
            }
            const QString text = QString::fromUtf8(m_textProcess.readAllStandardOutput()).trimmed();
            if (text.isEmpty()) {
              emit textCopyFailed(QStringLiteral("No text on this page"));
            } else if (!ClipboardText::offer(text)) {
              emit textCopyFailed(QStringLiteral("The clipboard is not reachable"));
            } else {
              emit textCopied(m_textPage);
            }
          });

  m_wordsTimeout.setSingleShot(true);
  m_wordsTimeout.setInterval(20'000);
  connect(&m_wordsTimeout, &QTimer::timeout, &m_wordsProcess, &QProcess::kill);
  connect(&m_wordsProcess, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_wordsTimeout.stop();
            const int page = m_wordsPage;
            m_wordsPage = 0;
            if (status != QProcess::NormalExit || exitCode != 0) {
              if (m_pendingPage == page) {
                emit selectionFailed(QStringLiteral("Could not read this page's text"));
              }
              return;
            }
            m_pageWords = PdfSupport::parsePageWords(m_wordsProcess.readAllStandardOutput());
            m_wordsLoadedPage = page;
            applyPendingSelection();
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
  if (m_textProcess.state() != QProcess::NotRunning) {
    m_textProcess.kill();
    m_textProcess.waitForFinished(1000);
  }
  if (m_wordsProcess.state() != QProcess::NotRunning) {
    m_wordsProcess.kill();
    m_wordsProcess.waitForFinished(1000);
  }
}

bool PdfInspector::available() const {
  return !QStandardPaths::findExecutable(QStringLiteral("pdfinfo")).isEmpty();
}

bool PdfInspector::textSearchAvailable() const {
  return PdfSupport::textAvailable();
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
  clearSelection();
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
  m_textTimeout.stop();
  if (m_textProcess.state() != QProcess::NotRunning) {
    m_textProcess.kill();
  }
  clearSearch();
  clearSelection();
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

void PdfInspector::copyPageText(int page) {
  if (m_path.isEmpty() || page < 1 || (m_pageCount > 0 && page > m_pageCount) ||
      !PdfSupport::textAvailable()) {
    emit textCopyFailed(QStringLiteral("No text to copy"));
    return;
  }
  if (m_textProcess.state() != QProcess::NotRunning) {
    m_textProcess.kill();
    m_textProcess.waitForFinished(500);
  }
  m_textPage = page;
  m_textProcess.start(QStandardPaths::findExecutable(QStringLiteral("pdftotext")),
                      {QStringLiteral("-f"), QString::number(page), QStringLiteral("-l"),
                       QString::number(page), m_path, QStringLiteral("-")});
  m_textTimeout.start();
}

void PdfInspector::startPageWords(int page) {
  if (m_wordsProcess.state() != QProcess::NotRunning) {
    m_wordsProcess.kill();
    m_wordsProcess.waitForFinished(500);
  }
  m_wordsLoadedPage = 0;
  m_pageWords = {};
  m_wordsPage = page;
  m_wordsProcess.start(QStandardPaths::findExecutable(QStringLiteral("pdftotext")),
                       {QStringLiteral("-bbox"), QStringLiteral("-f"), QString::number(page),
                        QStringLiteral("-l"), QString::number(page), m_path,
                        QStringLiteral("-")});
  m_wordsTimeout.start();
}

void PdfInspector::applyPendingSelection() {
  if (m_pendingPage < 1 || m_pendingArea.isEmpty() || m_wordsLoadedPage != m_pendingPage) {
    return;
  }
  const QList<int> indexes = PdfSupport::wordsTouched(m_pageWords, m_pendingArea);
  m_selectionPage = m_pendingPage;
  m_selectionIndexes = indexes;
  m_selectionRects.clear();
  m_selectionText.clear();
  if (!indexes.isEmpty()) {
    m_selectionText = PdfSupport::wordsText(m_pageWords, indexes);
    const QList<QRectF> lines = PdfSupport::wordLines(m_pageWords, indexes);
    for (const QRectF& line : lines) {
      m_selectionRects.append(line);
    }
  }
  emit selectionChanged();
  if (indexes.isEmpty()) {
    // An image-only page and a drag that missed the words read differently to
    // someone dragging, so they are told apart.
    emit selectionFailed(m_pageWords.words.isEmpty()
                             ? QStringLiteral("This page has no selectable text")
                             : QStringLiteral("No text under that selection"));
  }
}

void PdfInspector::updateSelection(int page, qreal left, qreal top, qreal right, qreal bottom) {
  if (m_path.isEmpty() || page < 1 || (m_pageCount > 0 && page > m_pageCount) ||
      !PdfSupport::textAvailable()) {
    emit selectionFailed(QStringLiteral("No text to select"));
    return;
  }
  const QRectF area(QPointF(left, top), QPointF(right, bottom));
  const QRectF wanted = area.normalized().intersected(QRectF(0, 0, 1, 1));
  if (wanted.isEmpty()) {
    clearSelection();
    return;
  }
  m_pendingPage = page;
  m_pendingArea = wanted;
  if (m_wordsLoadedPage == page) {
    applyPendingSelection();
    return;
  }
  if (m_wordsPage != page) {
    startPageWords(page);
  }
}

void PdfInspector::clearSelection() {
  m_wordsTimeout.stop();
  if (m_wordsProcess.state() != QProcess::NotRunning) {
    m_wordsProcess.kill();
  }
  m_wordsPage = 0;
  m_wordsLoadedPage = 0;
  m_pageWords = {};
  m_pendingPage = 0;
  m_pendingArea = QRectF();
  const bool had = !m_selectionText.isEmpty() || !m_selectionRects.isEmpty() ||
                   m_selectionPage != 0;
  m_selectionPage = 0;
  m_selectionIndexes.clear();
  m_selectionRects.clear();
  m_selectionText.clear();
  if (had) {
    emit selectionChanged();
  }
}

void PdfInspector::copySelection() {
  if (m_selectionText.isEmpty()) {
    emit selectionFailed(QStringLiteral("Select some text first"));
    return;
  }
  if (!ClipboardText::offer(m_selectionText)) {
    emit selectionFailed(QStringLiteral("The clipboard is not reachable"));
    return;
  }
  emit selectionCopied(static_cast<int>(m_selectionIndexes.size()));
}
