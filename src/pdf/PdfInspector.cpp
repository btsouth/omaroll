#include "pdf/PdfInspector.h"

#include "edit/ClipboardText.h"
#include "pdf/PdfSupport.h"
#include "sources/FileVersion.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QStandardPaths>

PdfInspector::PdfInspector(QObject* parent) : QObject(parent) {
  boundProcess(m_process, m_timeout, m_infoOutput, 64 * 1024);
  boundProcess(m_searchProcess, m_searchTimeout, m_searchOutput, PdfSupport::kTextByteLimit);
  boundProcess(m_textProcess, m_textTimeout, m_textOutput, PdfSupport::kPageByteLimit);
  boundProcess(m_wordsProcess, m_wordsTimeout, m_wordsOutput, PdfSupport::kPageByteLimit);
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(10'000);
  connect(&m_process, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_timeout.stop();
            m_loading = false;
            if (!currentVersion()) return;
            if (status != QProcess::NormalExit || exitCode != 0 || !m_infoOutput.failure.isEmpty()) {
              m_error = m_infoOutput.failure.isEmpty()
                            ? QString::fromLocal8Bit(m_infoOutput.diagnostics).trimmed()
                            : m_infoOutput.failure;
              if (m_error.isEmpty()) {
                m_error = QStringLiteral("Could not read PDF details");
              }
              emit changed();
              return;
            }
            const QString output = QString::fromLocal8Bit(m_infoOutput.bytes);
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
  connect(&m_searchProcess, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_searchTimeout.stop();
            if (!currentVersion()) return;
            if (status != QProcess::NormalExit || exitCode != 0 || !m_searchOutput.failure.isEmpty()) {
              m_extracting = false;
              emit searchFailed(m_searchOutput.failure.isEmpty()
                  ? QStringLiteral("Could not search this PDF: %1").arg(
                      QString::fromLocal8Bit(m_searchOutput.diagnostics).trimmed())
                  : m_searchOutput.failure);
              return;
            }
            const QByteArray bytes = std::move(m_searchOutput.bytes);
            const quint64 generation = m_generation;
            const quint64 extraction = m_extractionRequest;
            auto* watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation, extraction] {
              const QString text = watcher->result();
              watcher->deleteLater();
              if (generation != m_generation || extraction != m_extractionRequest || !currentVersion()) return;
              m_extracting = false;
              m_documentText = text;
              m_textLoaded = true;
              searchText();
            });
            watcher->setFuture(QtConcurrent::run([bytes] { return QString::fromUtf8(bytes); }));
          });
  connect(&m_searchProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      m_searchTimeout.stop();
      m_extracting = false;
      emit searchFailed(QStringLiteral("Could not start PDF text extraction"));
    }
  });

  m_textTimeout.setSingleShot(true);
  m_textTimeout.setInterval(20'000);
  connect(&m_textProcess, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_textTimeout.stop();
            if (!currentVersion()) return;
            if (status != QProcess::NormalExit || exitCode != 0 || !m_textOutput.failure.isEmpty()) {
              emit textCopyFailed(m_textOutput.failure.isEmpty() ? QStringLiteral("Could not read this page")
                                                                : m_textOutput.failure);
              return;
            }
            const QByteArray bytes = std::move(m_textOutput.bytes);
            const quint64 generation = m_generation;
            const int page = m_textPage;
            const quint64 request = m_textRequest;
            auto* watcher = new QFutureWatcher<QString>(this);
            connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation, request, page] {
              const QString text = watcher->result();
              watcher->deleteLater();
              if (generation != m_generation || request != m_textRequest || !currentVersion()) return;
              if (text.isEmpty()) {
                emit textCopyFailed(QStringLiteral("No text on this page"));
              } else if (!ClipboardText::offer(text)) {
                emit textCopyFailed(QStringLiteral("The clipboard is not reachable"));
              } else {
                emit textCopied(page);
              }
            });
            watcher->setFuture(QtConcurrent::run([bytes] { return QString::fromUtf8(bytes).trimmed(); }));
          });
  connect(&m_textProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      m_textTimeout.stop();
      emit textCopyFailed(QStringLiteral("Could not start PDF text extraction"));
    }
  });

  m_wordsTimeout.setSingleShot(true);
  m_wordsTimeout.setInterval(20'000);
  connect(&m_wordsProcess, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            m_wordsTimeout.stop();
            const int page = m_wordsPage;
            if (!currentVersion()) return;
            if (status != QProcess::NormalExit || exitCode != 0 || !m_wordsOutput.failure.isEmpty()) {
              m_wordsPage = 0;
              // A cancelled read (the selection was dropped, the document
              // changed) leaves the page at zero and is not a failure.
              if (page > 0 && m_pendingPage == page) {
                emit selectionFailed(m_wordsOutput.failure.isEmpty() ? QStringLiteral("Could not read this page's text")
                                                                   : m_wordsOutput.failure);
              }
              return;
            }
            const QByteArray bytes = std::move(m_wordsOutput.bytes);
            const quint64 generation = m_generation;
            const quint64 request = m_wordsRequest;
            auto* watcher = new QFutureWatcher<PdfSupport::PdfPageText>(this);
            connect(watcher, &QFutureWatcher<PdfSupport::PdfPageText>::finished, this,
                    [this, watcher, generation, request, page] {
              const auto words = watcher->result();
              watcher->deleteLater();
              if (generation != m_generation || request != m_wordsRequest || !currentVersion()) return;
              m_wordsPage = 0;
              if (words.pageSize.width() > 0) {
                m_pageWords = words;
                m_wordsLoadedPage = page;
              } else if (m_pendingPage == page) {
                emit selectionFailed(QStringLiteral("Malformed PDF text coordinates"));
              }
              applyPendingSelection();
            });
            watcher->setFuture(QtConcurrent::run([bytes] { return PdfSupport::parsePageWords(bytes); }));
          });
  connect(&m_wordsProcess, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) {
              return;
            }
            m_wordsTimeout.stop();
            // finished() never arrives for a process that did not start, so the
            // page has to be released here or it would never be read again.
            const int page = m_wordsPage;
            m_wordsPage = 0;
            if (page > 0 && m_pendingPage == page) {
              emit selectionFailed(QStringLiteral("PDF support needs Poppler"));
            }
          });
}

void PdfInspector::boundProcess(QProcess& process, QTimer& timer, Output& output, qsizetype limit) {
  PdfSupport::limitProcess(process);
  const auto drain = [&process, &output, limit] {
    const QByteArray bytes = process.readAllStandardOutput();
    if (output.bytes.size() + bytes.size() > limit) {
      output.failure = QStringLiteral("PDF text output is too large");
      process.kill();
    } else if (output.failure.isEmpty()) {
      output.bytes += bytes;
    }
    output.diagnostics = (output.diagnostics + process.readAllStandardError()).right(4096);
  };
  connect(&process, &QProcess::readyReadStandardOutput, this, drain);
  connect(&process, &QProcess::readyReadStandardError, this, drain);
  // Drain the final bytes before the operation's completion handler.
  connect(&process, &QProcess::finished, this, drain);
  connect(&timer, &QTimer::timeout, this, [&process, &output] {
    output.failure = QStringLiteral("Reading this PDF timed out");
    process.kill();
  });
}

void PdfInspector::stopProcess(QProcess& process, QTimer& timer) {
  timer.stop();
  const QSignalBlocker blocked(process);
  if (process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished(1000);
  }
}

bool PdfInspector::currentVersion() {
  if (m_path.isEmpty()) return false;
  if (FileVersion::key(m_path) == m_version) return true;
  inspect(m_path);
  return false;
}

void PdfInspector::searchText() {
  if (m_query.isEmpty()) return;
  const quint64 generation = m_generation;
  const quint64 request = ++m_searchRequest;
  const QString text = m_documentText;
  const QString query = m_query;
  auto* watcher = new QFutureWatcher<QList<int>>(this);
  connect(watcher, &QFutureWatcher<QList<int>>::finished, this, [this, watcher, generation, request] {
    const auto pages = watcher->result();
    watcher->deleteLater();
    if (generation != m_generation || request != m_searchRequest || !currentVersion()) return;
    m_matches.clear();
    for (const int page : pages) m_matches.append(page);
    emit matchesChanged();
  });
  watcher->setFuture(QtConcurrent::run([text, query] { return PdfSupport::findPages(text, query); }));
}

PdfInspector::~PdfInspector() {
  stopProcess(m_process, m_timeout);
  stopProcess(m_searchProcess, m_searchTimeout);
  stopProcess(m_textProcess, m_textTimeout);
  stopProcess(m_wordsProcess, m_wordsTimeout);
}

bool PdfInspector::available() const {
  return !QStandardPaths::findExecutable(QStringLiteral("pdfinfo")).isEmpty();
}

bool PdfInspector::textSearchAvailable() const {
  return PdfSupport::textAvailable();
}

void PdfInspector::inspect(const QString& path) {
  const QString version = FileVersion::key(path);
  if (m_path == path && m_version == version && (m_loading || m_pageCount > 0)) {
    return;
  }
  ++m_generation;
  stopProcess(m_process, m_timeout);
  stopProcess(m_textProcess, m_textTimeout);
  clearSearch();
  m_documentText.clear();
  m_textLoaded = false;
  clearSelection();
  resetPageWords();
  m_path = path;
  m_version = version;
  m_infoOutput = {};
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
  ++m_generation;
  stopProcess(m_process, m_timeout);
  stopProcess(m_textProcess, m_textTimeout);
  m_documentText.clear();
  m_textLoaded = false;
  m_version.clear();
  clearSearch();
  clearSelection();
  resetPageWords();
  m_path.clear();
  m_pageCount = 0;
  m_loading = false;
  m_error.clear();
  emit changed();
}

void PdfInspector::find(const QString& query) {
  if (!currentVersion()) return;
  ++m_searchRequest;
  m_query = query.simplified();
  m_matches.clear();
  emit matchesChanged();
  if (m_query.isEmpty() || m_pageCount <= 0) return;
  if (!PdfSupport::textAvailable()) {
    emit searchFailed(QStringLiteral("PDF search needs Poppler"));
    return;
  }
  if (m_textLoaded) {
    searchText();
    return;
  }
  if (m_extracting) return;
  ++m_extractionRequest;
  m_searchOutput = {};
  m_extracting = true;
  m_searchProcess.start(QStandardPaths::findExecutable(QStringLiteral("pdftotext")),
                        {QStringLiteral("-layout"), m_path, QStringLiteral("-")});
  m_searchTimeout.start();
}

void PdfInspector::clearSearch() {
  ++m_extractionRequest;
  ++m_searchRequest;
  stopProcess(m_searchProcess, m_searchTimeout);
  m_extracting = false;
  m_query.clear();
  if (!m_matches.isEmpty()) {
    m_matches.clear();
    emit matchesChanged();
  }
}

void PdfInspector::copyPageText(int page) {
  if (!m_path.isEmpty() && !currentVersion()) return;
  if (m_path.isEmpty() || page < 1 || (m_pageCount > 0 && page > m_pageCount) ||
      !PdfSupport::textAvailable()) {
    emit textCopyFailed(QStringLiteral("No text to copy"));
    return;
  }
  stopProcess(m_textProcess, m_textTimeout);
  ++m_textRequest;
  m_textOutput = {};
  m_textPage = page;
  m_textProcess.start(QStandardPaths::findExecutable(QStringLiteral("pdftotext")),
                      {QStringLiteral("-f"), QString::number(page), QStringLiteral("-l"),
                       QString::number(page), m_path, QStringLiteral("-")});
  m_textTimeout.start();
}

void PdfInspector::startPageWords(int page) {
  stopProcess(m_wordsProcess, m_wordsTimeout);
  ++m_wordsRequest;
  m_wordsOutput = {};
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
  struct Selection {
    QList<int> indexes;
    QString text;
    QVariantList rects;
  };
  const quint64 generation = m_generation;
  const quint64 request = ++m_selectionRequest;
  const int page = m_pendingPage;
  const QRectF area = m_pendingArea;
  const auto words = m_pageWords;
  auto* watcher = new QFutureWatcher<Selection>(this);
  connect(watcher, &QFutureWatcher<Selection>::finished, this,
          [this, watcher, generation, request, page, empty = words.words.isEmpty()] {
    const auto selection = watcher->result();
    watcher->deleteLater();
    if (generation != m_generation || request != m_selectionRequest || !currentVersion()) return;
    m_selectionPage = page;
    m_selectionIndexes = selection.indexes;
    m_selectionText = selection.text;
    m_selectionRects = selection.rects;
    emit selectionChanged();
    if (selection.indexes.isEmpty()) {
      emit selectionFailed(empty ? QStringLiteral("This page has no selectable text")
                                 : QStringLiteral("No text under that selection"));
    }
  });
  watcher->setFuture(QtConcurrent::run([words, area] {
    Selection selection;
    selection.indexes = PdfSupport::wordsTouched(words, area);
    selection.text = PdfSupport::wordsText(words, selection.indexes);
    for (const auto& line : PdfSupport::wordLines(words, selection.indexes)) {
      selection.rects.append(line);
    }
    return selection;
  }));
}

void PdfInspector::updateSelection(int page, qreal left, qreal top, qreal right, qreal bottom) {
  if (!m_path.isEmpty() && !currentVersion()) return;
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
  ++m_wordsRequest;
  ++m_selectionRequest;
  stopProcess(m_wordsProcess, m_wordsTimeout);
  m_wordsPage = 0;
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

// Drops the page of words: they belong to one document and one page.
void PdfInspector::resetPageWords() {
  m_wordsLoadedPage = 0;
  m_pageWords = {};
}

void PdfInspector::copySelection() {
  if (!m_path.isEmpty() && !currentVersion()) {
    emit selectionFailed(QStringLiteral("The PDF changed. Select its text again"));
    return;
  }
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
