#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class ActionLauncher;

// The delegation matrix, as data.
//
// omaroll performs almost nothing itself. This table says, for each medium,
// which already-installed tool owns the job and how to hand the file over.
// Adding a handler is a row here plus an argv template, not new code.
//
// Rows are keyed on medium rather than on CaptureRecord::Kind because that is
// what actually decides them: a downloaded clip trims like a recording and a
// downloaded photo mattes like a screenshot.
//
// Nothing is run through a shell. Arguments are passed as a real argv, so a
// filename containing a quote, a space or a semicolon is data rather than
// syntax.
class ActionRegistry final : public QObject {
  Q_OBJECT

public:
  enum class Media { Still, Moving, Document, Visual, Any };

  // How the handler's result reaches the user. Most tools open a window or
  // send their own notification; the two recognisers only print to stdout, so
  // omaroll has to catch that and put it somewhere useful.
  enum class Result { Launch, TextToClipboard, SecretToClipboard, CopyFile };

  struct Definition {
    QString id = {};
    QString label = {};
    // Empty for actions omaroll performs itself; QML handles those and they are
    // always reported as available.
    QString program = {};
    // "{path}" is substituted with the file, "{mime}" with its MIME type.
    // Everything else is literal.
    QStringList arguments = {};
    QString shortcut = {};
    // Named in the error when the program is missing, so the message can say
    // what to install rather than just failing.
    QString packageHint = {};
    Media media = Media::Any;
    // Legacy or internal rows remain runnable without cluttering the action
    // list. This preserves stable ids while a richer UI replaces a preset.
    bool visible = true;
    // Shown first and bound to Enter for its medium.
    bool primary = false;
    Result result = Result::Launch;
    // Said in the status line after a successful launch, for a tool that
    // otherwise gives no sign it did anything.
    QString confirmation = {};
    // Said when a recogniser finds nothing. Not an error; an empty screenshot
    // is ordinary.
    QString nothingFound = {};
    // The handler takes any number of files at once, so "{path}" expands to
    // all of them in a batch run rather than launching once per file.
    bool batch = false;
    // Where the tool writes, with "{stem}" for the source's base name, for
    // tools that refuse to overwrite: omarchy-transcode runs ffmpeg without
    // -y, and a second run would hang on its prompt. An existing output is
    // reported instead of launched.
    QString output = {};
  };

  explicit ActionRegistry(ActionLauncher* launcher, QObject* parent = nullptr);

  // Rows for QML: id, label, shortcut, available, hint, primary, native.
  Q_INVOKABLE QVariantList actionsFor(bool video) const;
  Q_INVOKABLE QVariantList actionsForKind(bool video, bool document) const;

  // Runs a delegated action. Returns false for an unknown id, an action omaroll
  // handles itself, or a missing program; the launcher reports why.
  Q_INVOKABLE bool run(const QString& id, const QString& path);

  // The same action over a selection. A batch-capable handler gets every path
  // in one launch; anything else is run once per file.
  Q_INVOKABLE bool runBatch(const QString& id, const QStringList& paths);

  // A batch with extra placeholders filled in: {machine} for the Tailscale
  // row, chosen by the user in a sheet before the launch.
  Q_INVOKABLE bool runBatchWith(const QString& id, const QVariantMap& placeholders,
                                const QStringList& paths);

  // True when the action's program is installed, for chrome that should only
  // offer what can run.
  Q_INVOKABLE bool available(const QString& id) const;

  // True when the action is one QML performs in-app rather than delegating.
  Q_INVOKABLE bool isNative(const QString& id) const;

  Q_INVOKABLE QString primaryActionFor(bool video) const;
  Q_INVOKABLE QString primaryActionForKind(bool video, bool document) const;

  // One source for shortcut handlers, action rows and hover help.
  Q_INVOKABLE QString shortcutFor(const QString& id) const;

  // Whether an action makes sense for this medium, so a shortcut pressed on
  // the wrong kind of file is refused with a word rather than handed to a tool
  // that cannot open it.
  Q_INVOKABLE bool appliesTo(const QString& id, bool video) const;
  Q_INVOKABLE bool appliesToKind(const QString& id, bool video, bool document) const;

private:
  bool run(const QString& id, const QStringList& paths, const QVariantMap& placeholders = {});
  // The output already exists and is not empty: ask ffprobe whether it is
  // whole, then either reveal it or clear it and launch. Asynchronous, so a
  // slow probe never freezes the window.
  void probeThenLaunch(const Definition& definition, const QStringList& arguments,
                       const QString& output);
  bool launch(const Definition& definition, const QStringList& arguments, const QString& output);
  [[nodiscard]] static bool applies(const Definition& definition, bool video, bool document);
  [[nodiscard]] const Definition* find(const QString& id) const;
  [[nodiscard]] static QList<Definition> buildTable();

  ActionLauncher* m_launcher = nullptr;
  QList<Definition> m_definitions;
};
