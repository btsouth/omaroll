#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>

// The files the quick viewer steps through, and which one is showing.
//
// One opened file means its folder: every picture and video beside it, in the
// natural name order a file manager shows. Several opened files are an explicit
// selection, kept in the order they were handed over. Nothing here touches the
// library; a file opened from the file manager is shown without scanning a
// single watched root.
//
// The folder is listed on a worker thread. Until it lands the opened file is
// the whole sequence, so the picture is on screen before the directory has
// been read.
class ViewerSession final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY currentChanged)
  Q_PROPERTY(QUrl url READ url NOTIFY currentChanged)
  Q_PROPERTY(QString fileName READ fileName NOTIFY currentChanged)
  Q_PROPERTY(QString folder READ folder NOTIFY currentChanged)
  Q_PROPERTY(bool isVideo READ isVideo NOTIFY currentChanged)
  Q_PROPERTY(bool isAnimated READ isAnimated NOTIFY currentChanged)
  Q_PROPERTY(double stamp READ stamp NOTIFY currentChanged)
  Q_PROPERTY(QString sizeLabel READ sizeLabel NOTIFY currentChanged)
  Q_PROPERTY(QString dateLabel READ dateLabel NOTIFY currentChanged)
  Q_PROPERTY(int index READ index NOTIFY currentChanged)
  Q_PROPERTY(int count READ count NOTIFY sequenceChanged)
  Q_PROPERTY(bool selection READ selection NOTIFY sequenceChanged)

public:
  explicit ViewerSession(QObject* parent = nullptr);
  ~ViewerSession() override;

  [[nodiscard]] QString path() const { return m_paths.value(m_index); }
  [[nodiscard]] QUrl url() const;
  [[nodiscard]] QString fileName() const;
  [[nodiscard]] QString folder() const;
  [[nodiscard]] bool isVideo() const;
  [[nodiscard]] bool isAnimated() const;
  [[nodiscard]] double stamp() const { return m_stamp; }
  [[nodiscard]] QString sizeLabel() const { return m_sizeLabel; }
  [[nodiscard]] QString dateLabel() const { return m_dateLabel; }
  [[nodiscard]] int index() const { return m_index; }
  [[nodiscard]] int count() const { return int(m_paths.size()); }
  [[nodiscard]] bool selection() const { return m_selection; }

  // Paths must already be canonical, as OpenRequest makes them.
  Q_INVOKABLE void open(const QStringList& paths);
  // Forget the sequence, so a closed window does not flash the last picture
  // when it is opened onto another one.
  Q_INVOKABLE void clear();

  // Moves through the sequence, wrapping at either end. False when there is
  // nowhere else to go.
  Q_INVOKABLE bool step(int direction);
  Q_INVOKABLE bool jump(int index);

  // The file |offset| steps away, for preloading the next picture.
  Q_INVOKABLE QUrl neighbourUrl(int offset) const;
  Q_INVOKABLE bool neighbourIsVideo(int offset) const;
  Q_INVOKABLE bool neighbourIsAnimated(int offset) const;

  // A file this window just moved to the Trash. The next one takes its place,
  // or the previous one at the end; an empty sequence emits emptied().
  Q_INVOKABLE void forget(const QString& path);

  // Hands the current file to the library window.
  Q_INVOKABLE void openInLibrary();

  // True when every file can be shown here. PDFs and folders belong to the
  // library, which has the document controls and the grid.
  [[nodiscard]] static bool canOpen(const QStringList& files);
  [[nodiscard]] static bool isViewable(const QString& path);

  // Pictures and videos in |folder| in natural name order. Dotfiles are left
  // out, except |keep|, which is always listed: it is the file the user opened.
  [[nodiscard]] static QStringList siblings(const QString& folder, const QString& keep = {});

  // The viewer's window in logical pixels: most of |available|, landscape,
  // the same for every file, with each picture fitted inside it.
  [[nodiscard]] static QSize preferredWindowSize(const QSize& available);

signals:
  void currentChanged();
  void sequenceChanged();
  void emptied();
  void libraryRequested(const QString& path);

private:
  void setSequence(const QStringList& paths, int index);
  void setIndex(int index);
  void refreshDetails();
  void startListing();
  void applyListing(const QStringList& listed);
  void watchFolder(const QString& folder);

  QStringList m_paths;
  int m_index = -1;
  bool m_selection = false;
  double m_stamp = 0;
  QString m_sizeLabel;
  QString m_dateLabel;
  QString m_mediaSuffix;
  // The folder being shown and the opened file, for relisting after a change.
  QString m_folder;
  QString m_opened;
  // A listing that finishes after another open() describes the wrong folder.
  quint64 m_generation = 0;
  quint64 m_listedGeneration = 0;
  QFutureWatcher<QStringList> m_listing;
  QFileSystemWatcher m_watcher;
  QTimer m_relist;
};
