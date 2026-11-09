#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

// Which file each thread is decoding, kept where a crash leaves it behind.
//
// Image decoders run in the process, and a decoder bug in a system plugin can
// take Omaroll down. Without a record the same file is read again on the next
// launch, by the grid or the viewer's preload, and crashes it again. Each
// decoding thread owns one slot of a small memory-mapped file in the cache
// folder and writes the path it is reading there; a fatal signal on that thread
// marks the slot. The next launch blocks that file until it changes (size or
// modification time) and says once which file it was.
//
// Recording is a memcpy into shared memory, not a write and unlink per decode,
// so it costs nothing measurable against even a small thumbnail. The file's
// space is reserved first, so no store into it can fail.
namespace DecodeJournal {

// Starts journaling for this process: takes what the last process left in
// |journalDirectory| (|directory| when empty), blocks the file it crashed on
// in |directory|'s list, and installs the crash handler. The journal is best
// on a tmpfs such as $XDG_RUNTIME_DIR, the list where it survives a reboot.
// False when another running Omaroll holds the journal, or there is no room
// for it; decoding then goes on unrecorded, and files blocked before are
// still skipped.
bool open(const QString& directory, const QString& journalDirectory = QString());

// Makes the next open() find no room for the journal, as on a full disk.
// For tests.
void failNextReservation();

// Stops journaling. Nothing may be decoding. For tests.
void close();

[[nodiscard]] bool active();

// A file that crashed Omaroll while it was being read and has not changed
// since. Costs nothing while nothing is blocked.
[[nodiscard]] bool isBlocked(const QString& path);

// Every blocked file, for tests and diagnostics.
[[nodiscard]] QStringList blocked();

// The file the last crash happened on, until its notice has been shown.
[[nodiscard]] QString pendingNotice();
void noticeShown();

// Marks the current thread's file as the one a crash happened on, as the
// fatal signal handler does. For tests.
void simulateCrash();

// Puts the crash handler first in line again, before whatever handles fatal
// signals now; open() does this. For tests that replace handlers.
void installCrashHandler();

// Records |path| as being decoded on this thread for the scope's lifetime.
// Scopes nest: the outer file is recorded again when an inner one ends.
class Scope {
public:
  explicit Scope(const QString& path);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  QByteArray m_encoded;
  Scope* m_previous = nullptr;
  int m_slot = -1;
};

} // namespace DecodeJournal

// The notice the library shows after a crash: which file, once.
class DecodeCrashNotice final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(QString fileName READ fileName NOTIFY changed)

public:
  explicit DecodeCrashNotice(QObject* parent = nullptr);

  [[nodiscard]] QString path() const { return m_path; }
  [[nodiscard]] QString fileName() const;

  // The notice is on screen: later launches do not show it again.
  Q_INVOKABLE void shown();
  Q_INVOKABLE void dismiss();
  // Reads the journal's pending notice again. For tests.
  void reload();

signals:
  void changed();

private:
  QString m_path;
};
