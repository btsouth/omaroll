#pragma once

#include "pdf/PdfSupport.h"

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
  // The page the last selection was made on, its text, and one normalized
  // rectangle per selected line for the highlight.
  Q_PROPERTY(int selectionPage READ selectionPage NOTIFY selectionChanged)
  Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
  Q_PROPERTY(QVariantList selectionRects READ selectionRects NOTIFY selectionChanged)

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
  [[nodiscard]] int selectionPage() const { return m_selectionPage; }
  [[nodiscard]] bool hasSelection() const { return !m_selectionText.isEmpty(); }
  [[nodiscard]] QVariantList selectionRects() const { return m_selectionRects; }
  [[nodiscard]] QString selectionText() const { return m_selectionText; }
  Q_INVOKABLE void inspect(const QString& path);
  Q_INVOKABLE void clear();
  // Searches the inspected document with pdftotext, asynchronously.
  Q_INVOKABLE void find(const QString& query);
  Q_INVOKABLE void clearSearch();
  // Extracts one page's text and copies it to the clipboard.
  Q_INVOKABLE void copyPageText(int page);
  // Marks the part of a page the reader dragged over, in page coordinates
  // (0..1 within the page, so any zoom or rotation can send it). The words
  // under it arrive asynchronously on first use of a page.
  Q_INVOKABLE void updateSelection(int page, qreal left, qreal top, qreal right, qreal bottom);
  Q_INVOKABLE void clearSelection();
  // Puts the selected words on the clipboard.
  Q_INVOKABLE void copySelection();

signals:
  void changed();
  void matchesChanged();
  void textCopied(int page);
  void textCopyFailed(const QString& message);
  void selectionChanged();
  void selectionCopied(int words);
  void selectionFailed(const QString& message);

private:
  void startPageWords(int page);
  void applyPendingSelection();
  void resetPageWords();

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

  QProcess m_textProcess;
  QTimer m_textTimeout;
  int m_textPage = 0;

  QProcess m_wordsProcess;
  QTimer m_wordsTimeout;
  int m_wordsPage = 0;       // Page being read, 0 when idle.
  int m_wordsLoadedPage = 0; // Page whose words are cached below.
  PdfSupport::PdfPageText m_pageWords;
  int m_pendingPage = 0;
  QRectF m_pendingArea;
  int m_selectionPage = 0;
  QList<int> m_selectionIndexes;
  QVariantList m_selectionRects;
  QString m_selectionText;
};
