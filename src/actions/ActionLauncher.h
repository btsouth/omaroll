#pragma once

#include <QObject>
#include <QDBusConnection>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

// Hands a capture to whoever already owns the job.
//
// omaroll performs almost no work itself: this class is the seam where the
// library stops and Omarchy's existing tools take over. Everything runs with an
// explicit argv, never through a shell, so a filename containing a quote or a
// semicolon is data rather than syntax.
class ActionLauncher final : public QObject {
  Q_OBJECT
  // Full paths of tracked outputs still being written. The footer pins a
  // "Making ..." line on this, because a transcode can run for minutes with
  // nothing else on screen saying anything is happening.
  Q_PROPERTY(QStringList pendingOutputs READ pendingOutputs NOTIFY pendingOutputsChanged)

public:
  explicit ActionLauncher(QObject* parent = nullptr,
                          std::optional<QDBusConnection> bus = std::nullopt);

  // Open in whatever the desktop has registered for the type.
  Q_INVOKABLE bool open(const QString& path);

  // Ask the desktop to select this file, falling back to its default folder handler.
  bool showInFolder(const QString& path);
  // Always show the desktop's compatible-application chooser, without changing MIME defaults.
  bool openWith(const QString& path);

  // True when a program is on PATH. QML uses this to grey out an action rather
  // than failing after the click.
  Q_INVOKABLE bool handlerAvailable(const QString& program) const;

  // Ordinary Delete uses recoverable XDG Trash.
  Q_INVOKABLE bool moveToTrash(const QString& path);
  Q_INVOKABLE QVariantMap captureFileAction(const QString& path,
                                            const QString& expectedMediaPath = {});
  Q_INVOKABLE bool moveCapturedToTrash(const QVariantMap& target);
  Q_INVOKABLE QVariantMap renameCapturedFile(const QVariantMap& target, const QString& baseName);
  Q_INVOKABLE void moveDuplicateCopiesToTrash(const QVariantMap& keep, const QVariantList& copies);
  // Explicit permanent deletion removes only this file or symlink, never a directory tree.
  Q_INVOKABLE bool deletePermanently(const QString& path);
  // Bind confirmation to the parent directory and entry, preserving a final symlink.
  Q_INVOKABLE QVariantMap capturePermanentDelete(const QString& path,
                                                 const QString& expectedMediaPath = {});
  Q_INVOKABLE bool deleteCapturedPermanently(const QVariantMap& target);

  // Rename in place without letting the user accidentally change the media
  // extension or overwrite another file. The map contains ok, path, fileName,
  // and an inline error suitable for the rename sheet.
  Q_INVOKABLE QVariantMap renameFile(const QString& path, const QString& baseName);

  // Launch and forget. Names the package in the failure when the program is
  // absent, so the message says what to install.
  bool runDetached(const QString& program, const QStringList& arguments,
                   const QString& packageHint = {}, const QString& confirmation = {});

  // Follow a submission with no output file (for example lp). True means
  // accepted for launch, not completed. Only a normal zero exit confirms it;
  // stderr explains failures. A hung submission is stopped after 30 seconds.
  bool runSubmission(const QString& program, const QStringList& arguments,
                     const QString& packageHint = {}, const QString& confirmation = {});

  // Launch and follow a tool that writes a new file beside the original.
  // A transcode can run for minutes with its output sitting at zero bytes, so
  // fire-and-forget left the user guessing whether anything happened. The
  // output is announced as pending so the library holds the half-written file
  // back, and settled once the tool exits: saved on success, or retained and
  // explained on failure. The tool's own stderr is quoted when it has one.
  bool runTracked(const QString& program, const QStringList& arguments,
                  const QString& packageHint, const QString& outputPath);

  // True while a tracked run is still writing this path.
  [[nodiscard]] bool isPending(const QString& outputPath) const;

  [[nodiscard]] QStringList pendingOutputs() const;

  // A finished output already sits on disk from an earlier run: say so and
  // hand the path to the viewer, so pressing the action again always shows
  // the file rather than flashing a footer line and doing nothing visible.
  void revealExisting(const QString& path);

  // Run to completion off the GUI thread's event loop and put stdout on the
  // clipboard. Used by the recognisers, whose whole output is text the user
  // wants to paste. A sensitive result is marked so clipboard history does not
  // retain it, which matters for the otpauth:// URIs QR codes so often carry.
  bool captureTextToClipboard(const QString& program, const QStringList& arguments,
                              const QString& packageHint, bool sensitive,
                              const QString& confirmation, const QString& nothingFound);

  // One file on the clipboard as its own data, so it pastes as an image.
  // PNG/JPEG copying is asynchronous; true means accepted. Completion is
  // reported only after the clipboard tool exits successfully.
  Q_INVOKABLE bool copyFile(const QString& path);

  // Several files on the clipboard at once, as text/uri-list, which is what a
  // paste into Nautilus or a browser upload expects. copyFile uses image data
  // for supported pictures instead.
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
  // The requested output already existed, finished, from an earlier run. The
  // viewer opens on it; a footer flash alone reads as nothing happening.
  void outputAlreadyDone(const QString& path);
  void pendingOutputsChanged();
  void submissionFinished(const QString& program, bool success, const QString& detail);
  void duplicateCleanupFinished(int moved, const QStringList& skipped);

private:
  [[nodiscard]] static bool capturedFileMatches(const QVariantMap& target);
  [[nodiscard]] QString locate(const QString& program, const QString& packageHint);
  bool copyText(const QString& text, bool sensitive, const QString& mimeType = {});
  bool startSubmission(const QString& program, const QStringList& arguments,
                       const QString& packageHint, const QString& confirmation,
                       const QString& inputPath, int timeoutMs);

  QSet<QString> m_pendingOutputs;
  QSet<QString> m_pendingOpenWith;
  QDBusConnection m_bus;
};
