#include "actions/ActionRegistry.h"

#include "actions/ActionLauncher.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace {

QString ocrLanguages() {
  // Same variable omarchy-capture-text honours.
  const QString configured = qEnvironmentVariable("OMARCHY_OCR_LANGS");
  return configured.isEmpty() ? u"eng"_s : configured;
}

} // namespace

namespace {

ActionRegistry::Definition annotateRow() {
  using Media = ActionRegistry::Media;
  const QString configured = qEnvironmentVariable("OMARCHY_SCREENSHOT_EDITOR");
  if (!configured.isEmpty() && configured != u"tensaku-edit"_s) {
    return {.id = u"annotate"_s,
            .label = u"Annotate"_s,
            .program = configured,
            .arguments = {u"{path}"_s},
            .shortcut = u"A"_s,
            .media = Media::Still};
  }
  return {.id = u"annotate"_s,
          .label = u"Annotate"_s,
          .program = u"tensaku"_s,
          .arguments = {u"--filename"_s, u"{path}"_s, u"--output-filename"_s,
                        u"{dir}/{stem}-annotated.png"_s, u"--actions-on-enter"_s,
                        u"save-to-clipboard"_s, u"--save-after-copy"_s, u"--copy-command"_s,
                        u"wl-copy"_s},
          .shortcut = u"A"_s,
          .packageHint = u"tensaku"_s,
          .media = Media::Still};
}

ActionRegistry::Definition sendRow() {
  using Media = ActionRegistry::Media;
  if (!QStandardPaths::findExecutable(u"omarchy-menu-share"_s).isEmpty()) {
    return {.id = u"send"_s,
            .label = u"Send with LocalSend"_s,
            .program = u"omarchy-menu-share"_s,
            .arguments = {u"file"_s, u"{path}"_s},
            .shortcut = u"S"_s,
            .packageHint = u"localsend"_s,
            .media = Media::Any,
            .batch = true};
  }
  return {.id = u"send"_s,
          .label = u"Send with LocalSend"_s,
          .program = u"localsend"_s,
          .arguments = {u"--headless"_s, u"send"_s, u"{path}"_s},
          .shortcut = u"S"_s,
          .packageHint = u"localsend"_s,
          .media = Media::Any,
          .batch = true};
}

// omarchy-tailscale-send needs a machine first; TailscalePeers supplies the
// list and QML the choice, then {machine} is filled in here. Without Omarchy,
// the same Taildrop copy the house script performs.
ActionRegistry::Definition tailscaleRow() {
  using Media = ActionRegistry::Media;
  if (!QStandardPaths::findExecutable(u"omarchy-tailscale-send"_s).isEmpty()) {
    return {.id = u"tailscale"_s,
            .label = u"Send to a machine"_s,
            .program = u"omarchy-tailscale-send"_s,
            .arguments = {u"{machine}"_s, u"{path}"_s},
            .packageHint = u"tailscale"_s,
            .media = Media::Any,
            .batch = true};
  }
  return {.id = u"tailscale"_s,
          .label = u"Send to a machine"_s,
          .program = u"tailscale"_s,
          .arguments = {u"file"_s, u"cp"_s, u"--update-interval=0"_s, u"--"_s, u"{path}"_s,
                        u"{machine}:"_s},
          .packageHint = u"tailscale"_s,
          .media = Media::Any,
          .batch = true};
}

} // namespace

QList<ActionRegistry::Definition> ActionRegistry::buildTable() {
  using enum Media;
  using enum Result;

  return {
      // --- Recordings and videos ------------------------------------------
      {.id = u"trim"_s,
       .label = u"Trim"_s,
       .program = u"omacut"_s,
       .arguments = {u"{path}"_s},
       .shortcut = u"T"_s,
       .packageHint = u"omacut"_s,
       .media = Moving,
       .primary = true},

      {.id = u"play"_s,
       .label = u"Play"_s,
       .program = u"mpv"_s,
       .arguments = {u"{path}"_s},
       .shortcut = u"P"_s,
       .packageHint = u"mpv"_s,
       .media = Moving},

      // omarchy-transcode already does mp4 and gif at fixed rungs, so omaroll
      // writes no ffmpeg invocations of its own. It notifies on completion.
      {.id = u"gif"_s,
       .label = u"Clip to GIF"_s,
       .program = u"omarchy-transcode"_s,
       .arguments = {u"{path}"_s, u"gif"_s, u"720p"_s},
       .packageHint = u"Omarchy"_s,
       .media = Moving,
       .output = u"{stem}-720p.gif"_s},

      {.id = u"shrink"_s,
       .label = u"Resize to 1080p"_s,
       .program = u"omarchy-transcode"_s,
       .arguments = {u"{path}"_s, u"mp4"_s, u"1080p"_s},
       .packageHint = u"Omarchy"_s,
       .media = Moving,
       .output = u"{stem}-1080p.mp4"_s},

      // --- Screenshots and pictures ---------------------------------------
      // The matte is the one thing omaroll builds itself, because nothing on
      // Omarchy does it. No program, so QML handles it.
      {.id = u"matte"_s,
       .label = u"Make it postable"_s,
       .shortcut = u"M"_s,
       .media = Still,
       .primary = true},

      // tensaku-edit, Omarchy's wrapper, saves over the input. Fine for the
      // screenshot it was written for, not for a photo omaroll also offers
      // this on, so the default calls tensaku itself with the wrapper's own
      // flags and a new output name. A user who set their own editor gets it
      // with the bare path, as omarchy-capture-screenshot would hand it over.
      annotateRow(),

      // Same tesseract invocation omarchy-capture-text uses, minus its
      // single-block page mode: a whole screenshot has many blocks.
      {.id = u"ocr"_s,
       .label = u"Copy the text"_s,
       .program = u"tesseract"_s,
       .arguments = {u"{path}"_s, u"stdout"_s, u"--oem"_s, u"1"_s, u"-l"_s, ocrLanguages(),
                     u"--dpi"_s, u"300"_s, u"-c"_s, u"preserve_interword_spaces=1"_s},
       .shortcut = u"C"_s,
       .packageHint = u"tesseract"_s,
       .media = Still,
       .result = TextToClipboard,
       .nothingFound = u"No text found"_s},

      // QR codes only, as omarchy-capture-qr does: dense screen content
      // false-positives as a barcode otherwise. Decoded values are routinely
      // secrets (otpauth:// setup codes), so they go to the clipboard marked
      // sensitive and nowhere else.
      {.id = u"qr"_s,
       .label = u"Scan QR code"_s,
       .program = u"zbarimg"_s,
       .arguments = {u"-q"_s, u"--raw"_s, u"-Sdisable"_s, u"-Sqrcode.enable"_s, u"{path}"_s},
       .packageHint = u"zbar"_s,
       .media = Still,
       .result = SecretToClipboard,
       .confirmation = u"QR code copied to clipboard"_s,
       .nothingFound = u"No QR code found"_s},

      {.id = u"edit"_s,
       .label = u"Edit in Pinta"_s,
       .program = u"pinta"_s,
       .arguments = {u"{path}"_s},
       .packageHint = u"pinta"_s,
       .media = Still},

      {.id = u"view"_s,
       .label = u"View full size"_s,
       .program = u"imv"_s,
       .arguments = {u"{path}"_s},
       .packageHint = u"imv"_s,
       .media = Still},

      {.id = u"convert"_s,
       .label = u"Convert to JPEG"_s,
       .program = u"omarchy-transcode"_s,
       .arguments = {u"{path}"_s, u"jpg"_s, u"medium"_s},
       .packageHint = u"Omarchy"_s,
       .media = Still,
       .output = u"{stem}-medium.jpg"_s},

      // --- Anything -------------------------------------------------------
      // The launcher handles this one: the house helper on Omarchy, plain
      // wl-copy elsewhere, so the row is available on both.
      {.id = u"copy"_s,
       .label = u"Copy to clipboard"_s,
       .program = u"wl-copy"_s,
       .shortcut = u"Y"_s,
       .packageHint = u"wl-clipboard"_s,
       .result = CopyFile},

      // The same path the Share menu and the Nautilus extension take: LocalSend
      // in headless send mode, which opens straight onto the device picker.
      sendRow(),

      // Taildrop to one of the user's own machines. No shortcut: the sheet
      // that picks the machine is the step, and S stays LocalSend.
      tailscaleRow(),

      {.id = u"files"_s,
       .label = u"Show in files"_s,
       .program = u"nautilus"_s,
       .arguments = {u"--select"_s, u"{path}"_s},
       .shortcut = u"F"_s,
       .packageHint = u"nautilus"_s},

      // Native: QML owns these.
      {.id = u"favorite"_s, .label = u"Favourite"_s, .shortcut = u"V"_s},
      {.id = u"hide"_s, .label = u"Hide"_s, .shortcut = u"Ctrl+H"_s},
      {.id = u"trash"_s, .label = u"Move to Trash"_s, .shortcut = u"Del"_s},
  };
}

ActionRegistry::ActionRegistry(ActionLauncher* launcher, QObject* parent)
    : QObject(parent), m_launcher(launcher), m_definitions(buildTable()) {}

void ActionRegistry::probeThenLaunch(const Definition& definition, const QStringList& arguments,
                                     const QString& output) {
  // A >0-byte file is not proof the transcode finished: the fire-and-forget
  // era could die mid-write and leave a truncated mp4 that then blocked every
  // retry as "already done". ffprobe reads the container the way any player
  // would; it ships with the ffmpeg omarchy-transcode needs anyway. When it
  // is somehow absent, trust the file rather than re-transcoding on a guess.
  const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
  if (ffprobe.isEmpty()) {
    m_launcher->revealExisting(output);
    return;
  }

  // Off the event loop: a probe of a large file on a slow disk can take
  // seconds, and the window has to stay live while it decides.
  auto* probe = new QProcess(this);
  auto* timeout = new QTimer(probe);
  timeout->setSingleShot(true);
  timeout->setInterval(4000);
  connect(timeout, &QTimer::timeout, probe, &QProcess::kill);

  connect(probe, &QProcess::finished, this,
          [this, probe, definition, arguments, output](int exitCode, QProcess::ExitStatus status) {
            probe->deleteLater();
            // A probe that had to be killed says nothing about the file; trust
            // it, as a missing ffprobe does, rather than throw away a good one.
            const bool killed = status != QProcess::NormalExit;
            const bool complete =
                killed || (exitCode == 0 && probe->readAllStandardError().trimmed().isEmpty());
            if (complete) {
              // Shown, not just mentioned: the viewer opens on the file,
              // because "already done" with nothing to look at reads as
              // nothing happening.
              m_launcher->revealExisting(output);
              return;
            }
            // A truncated file with this exact name is the corpse of a
            // transcode that died before runs were tracked. Left in place it
            // blocks every retry forever, because ffmpeg refuses to overwrite.
            QFile::remove(output);
            launch(definition, arguments, output);
          });
  connect(probe, &QProcess::errorOccurred, this, [this, probe, output](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) {
      return;
    }
    probe->deleteLater();
    m_launcher->revealExisting(output);
  });

  probe->start(ffprobe, {u"-v"_s, u"error"_s, output});
  timeout->start();
}

bool ActionRegistry::applies(const Definition& definition, bool video) {
  switch (definition.media) {
  case Media::Still:
    return !video;
  case Media::Moving:
    return video;
  case Media::Any:
    return true;
  }
  return false;
}

const ActionRegistry::Definition* ActionRegistry::find(const QString& id) const {
  for (const Definition& definition : m_definitions) {
    if (definition.id == id) {
      return &definition;
    }
  }
  return nullptr;
}

bool ActionRegistry::isNative(const QString& id) const {
  const Definition* definition = find(id);
  return definition && definition->program.isEmpty();
}

bool ActionRegistry::appliesTo(const QString& id, bool video) const {
  const Definition* definition = find(id);
  return definition && applies(*definition, video);
}

QString ActionRegistry::primaryActionFor(bool video) const {
  for (const Definition& definition : m_definitions) {
    if (definition.primary && applies(definition, video)) {
      return definition.id;
    }
  }
  return u"open"_s;
}

QVariantList ActionRegistry::actionsFor(bool video) const {
  QVariantList rows;
  for (const Definition& definition : m_definitions) {
    if (!applies(definition, video)) {
      continue;
    }

    const bool native = definition.program.isEmpty();
    QVariantMap row;
    row[u"id"_s] = definition.id;
    row[u"label"_s] = definition.label;
    row[u"shortcut"_s] = definition.shortcut;
    row[u"primary"_s] = definition.primary;
    row[u"native"_s] = native;
    row[u"available"_s] =
        native || (m_launcher && m_launcher->handlerAvailable(definition.program));
    row[u"hint"_s] = definition.packageHint;
    rows.append(row);
  }
  return rows;
}

bool ActionRegistry::runBatch(const QString& id, const QStringList& paths) {
  return runBatchWith(id, {}, paths);
}

bool ActionRegistry::runBatchWith(const QString& id, const QVariantMap& placeholders,
                                  const QStringList& paths) {
  if (paths.isEmpty()) {
    return false;
  }
  const Definition* definition = find(id);
  if (paths.size() > 1 && !(definition && definition->batch)) {
    bool all = true;
    for (const QString& path : paths) {
      all = run(id, QStringList{path}, placeholders) && all;
    }
    return all;
  }
  return run(id, paths, placeholders);
}

bool ActionRegistry::available(const QString& id) const {
  const Definition* definition = find(id);
  return definition && (definition->program.isEmpty() ||
                        (m_launcher && m_launcher->handlerAvailable(definition->program)));
}

bool ActionRegistry::run(const QString& id, const QString& path) {
  return run(id, QStringList{path});
}

bool ActionRegistry::run(const QString& id, const QStringList& paths,
                         const QVariantMap& placeholders) {
  const Definition* definition = find(id);
  if (!definition || definition->program.isEmpty() || !m_launcher || paths.isEmpty()) {
    return false;
  }

  const QFileInfo first(paths.first());
  const auto expand = [&first, &placeholders](QString argument) {
    argument.replace(u"{dir}"_s, first.absolutePath());
    argument.replace(u"{stem}"_s, first.completeBaseName());
    for (auto it = placeholders.cbegin(); it != placeholders.cend(); ++it) {
      argument.replace(QLatin1Char('{') + it.key() + QLatin1Char('}'), it.value().toString());
    }
    return argument;
  };

  QStringList arguments;
  arguments.reserve(definition->arguments.size() + paths.size());
  for (const QString& argument : definition->arguments) {
    if (argument == u"{path}"_s) {
      arguments.append(paths);
    } else if (argument == u"{mime}"_s) {
      arguments.append(ActionLauncher::mimeTypeFor(paths.first()));
    } else {
      arguments.append(expand(argument));
    }
  }

  switch (definition->result) {
  case Result::CopyFile:
    return m_launcher->copyFile(paths.first());
  case Result::TextToClipboard:
  case Result::SecretToClipboard:
    return m_launcher->captureTextToClipboard(
        definition->program, arguments, definition->packageHint,
        definition->result == Result::SecretToClipboard, definition->confirmation,
        definition->nothingFound);
  case Result::Launch:
    break;
  }

  QString output;
  if (!definition->output.isEmpty()) {
    output = first.absolutePath() + QLatin1Char('/') + expand(definition->output);
    if (m_launcher->isPending(output)) {
      m_launcher->report(u"Still working on %1"_s.arg(QFileInfo(output).fileName()));
      return true;
    }
    const QFileInfo existing(output);
    if (existing.exists()) {
      if (existing.size() > 0) {
        // Finished, or a truncated leftover: ffprobe decides, off the GUI
        // thread, and the launch follows from there.
        probeThenLaunch(*definition, arguments, output);
        return true;
      }
      // An empty file with this exact name is the corpse of a transcode that
      // died before runs were tracked. Left in place it blocks every retry
      // forever, because ffmpeg refuses to overwrite.
      QFile::remove(output);
    }
  }

  return launch(*definition, arguments, output);
}

bool ActionRegistry::launch(const Definition& definition, const QStringList& arguments,
                            const QString& output) {
  if (!output.isEmpty()) {
    return m_launcher->runTracked(definition.program, arguments, definition.packageHint, output);
  }
  return m_launcher->runDetached(definition.program, arguments, definition.packageHint,
                                 definition.confirmation);
}
