#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Hands a capture to whoever already owns the job.
//
// omaroll performs almost no work itself: this class is the seam where the
// library stops and Omarchy's existing tools take over. Everything runs with an
// explicit argv, never through a shell, so a filename containing a quote or a
// semicolon is data rather than syntax.
class ActionLauncher final : public QObject {
  Q_OBJECT

public:
  explicit ActionLauncher(QObject* parent = nullptr);

  // Open in whatever the desktop has registered for the type.
  Q_INVOKABLE bool open(const QString& path);

  // True when a program is on PATH. QML uses this to grey out an action rather
  // than failing after the click.
  Q_INVOKABLE bool handlerAvailable(const QString& program) const;

  // XDG trash, never unlink. A capture the user deletes by accident has to be
  // recoverable from their file manager like anything else they delete.
  Q_INVOKABLE bool moveToTrash(const QString& path);

  // Launch and forget. Names the package in the failure when the program is
  // absent, so the message says what to install.
  bool runDetached(const QString& program, const QStringList& arguments,
                   const QString& packageHint = {}, const QString& confirmation = {});

  // Launch and follow a tool that writes a new file beside the original.
  // A transcode can run for minutes with its output sitting at zero bytes, so
  // fire-and-forget left the user guessing whether anything happened. The
  // output is announced as pending so the library holds the half-written file
  // back, and settled once the tool exits: saved on success, or cleaned up and
  // explained on failure. The tool's own stderr is quoted when it has one.
  bool runTracked(const QString& program, const QStringList& arguments,
                  const QString& packageHint, const QString& outputPath);

  // True while a tracked run is still writing this path.
  [[nodiscard]] bool isPending(const QString& outputPath) const;

  // Run to completion off the GUI thread's event loop and put stdout on the
  // clipboard. Used by the recognisers, whose whole output is text the user
  // wants to paste. A sensitive result is marked so clipboard history does not
  // retain it, which matters for the otpauth:// URIs QR codes so often carry.
  bool captureTextToClipboard(const QString& program, const QStringList& arguments,
                              const QString& packageHint, bool sensitive,
                              const QString& confirmation, const QString& nothingFound);

  // One file on the clipboard as its own data, so it pastes as an image.
  Q_INVOKABLE bool copyFile(const QString& path);

  // Several files on the clipboard at once, as text/uri-list, which is what a
  // paste into Nautilus or a browser upload expects. One file goes through the
  // house helper instead so it pastes as image data.
  Q_INVOKABLE bool copyUris(const QStringList& paths);

  [[nodiscard]] static QString mimeTypeFor(const QString& path);

  // For the registry to put a note in the status line without launching.
  void report(const QString& message) { emit reported(message); }

signals:
  // Something the user should be told about, in their words rather than a
  // process exit code.
  void failed(const QString& message);
  // Something worth confirming happened, for the same status line.
  void reported(const QString& message);
  // A tracked run started writing this path; the library holds it back.
  void outputPending(const QString& path);
  // A tracked run ended. saved means the finished file is on disk.
  void outputSettled(const QString& path, bool saved);

private:
  [[nodiscard]] QString locate(const QString& program, const QString& packageHint);
  bool copyText(const QString& text, bool sensitive, const QString& mimeType = {});

  QSet<QString> m_pendingOutputs;
};
