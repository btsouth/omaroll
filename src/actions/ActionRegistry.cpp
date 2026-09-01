#include "actions/ActionRegistry.h"

#include "actions/ActionLauncher.h"

#include <QVariantMap>

namespace {

using Kind = CaptureRecord::Kind;

const QList<Kind> kEverything = {Kind::Screenshot, Kind::Recording, Kind::Picture, Kind::Video,
                                 Kind::Download};
const QList<Kind> kStills = {Kind::Screenshot, Kind::Picture, Kind::Download};
const QList<Kind> kMoving = {Kind::Recording, Kind::Video};

} // namespace

QString ActionRegistry::screenshotEditor() {
  // Omarchy's own capture script reads the same variable and defaults the same
  // way, so a user who pointed it elsewhere gets their editor here too.
  const QString configured = qEnvironmentVariable("OMARCHY_SCREENSHOT_EDITOR");
  return configured.isEmpty() ? QStringLiteral("tensaku-edit") : configured;
}

QList<ActionRegistry::Definition> ActionRegistry::buildTable() {
  const QString editor = screenshotEditor();

  return {
      // --- Recordings -----------------------------------------------------
      {QStringLiteral("trim"), QStringLiteral("Trim"), QStringLiteral("omacut"),
       {QStringLiteral("{path}")}, QStringLiteral("T"), QStringLiteral("omacut"), kMoving, true},

      {QStringLiteral("play"), QStringLiteral("Play"), QStringLiteral("mpv"),
       {QStringLiteral("{path}")}, QStringLiteral("P"), QStringLiteral("mpv"), kMoving, false},

      // omarchy-transcode already does mp4 and gif at fixed rungs, so omaroll
      // writes no ffmpeg invocations of its own.
      {QStringLiteral("gif"), QStringLiteral("Clip to GIF"), QStringLiteral("omarchy-transcode"),
       {QStringLiteral("{path}"), QStringLiteral("gif"), QStringLiteral("720p")},
       {}, QStringLiteral("omarchy"), kMoving, false},

      {QStringLiteral("shrink"), QStringLiteral("Resize to 1080p"),
       QStringLiteral("omarchy-transcode"),
       {QStringLiteral("{path}"), QStringLiteral("mp4"), QStringLiteral("1080p")},
       {}, QStringLiteral("omarchy"), kMoving, false},

      // --- Stills ---------------------------------------------------------
      // The matte is the one thing omaroll builds itself, because nothing on
      // Omarchy does it. No program, so QML handles it.
      {QStringLiteral("matte"), QStringLiteral("Make it postable"), {}, {}, QStringLiteral("M"),
       {}, kStills, true},

      {QStringLiteral("annotate"), QStringLiteral("Annotate"), editor,
       {QStringLiteral("{path}")}, QStringLiteral("A"), QStringLiteral("tensaku"), kStills, false},

      {QStringLiteral("ocr"), QStringLiteral("Copy the text"), QStringLiteral("tesseract"),
       {QStringLiteral("{path}"), QStringLiteral("stdout")}, QStringLiteral("C"),
       QStringLiteral("tesseract"), kStills, false},

      {QStringLiteral("qr"), QStringLiteral("Scan QR code"), QStringLiteral("zbarimg"),
       {QStringLiteral("--quiet"), QStringLiteral("{path}")}, {}, QStringLiteral("zbar"), kStills,
       false},

      {QStringLiteral("edit"), QStringLiteral("Edit in Pinta"), QStringLiteral("pinta"),
       {QStringLiteral("{path}")}, {}, QStringLiteral("pinta"), kStills, false},

      {QStringLiteral("view"), QStringLiteral("View full size"), QStringLiteral("imv"),
       {QStringLiteral("{path}")}, {}, QStringLiteral("imv"), kStills, false},

      {QStringLiteral("convert"), QStringLiteral("Convert to JPEG"),
       QStringLiteral("omarchy-transcode"),
       {QStringLiteral("{path}"), QStringLiteral("jpg"), QStringLiteral("medium")},
       {}, QStringLiteral("omarchy"), kStills, false},

      // --- Anything -------------------------------------------------------
      {QStringLiteral("copy"), QStringLiteral("Copy to clipboard"),
       QStringLiteral("omarchy-clipboard-paste-file"),
       {QStringLiteral("--copy-only"), QStringLiteral("{mime}"), QStringLiteral("{path}")},
       QStringLiteral("Y"), QStringLiteral("omarchy"), kEverything, false},

      {QStringLiteral("send"), QStringLiteral("Send to a device"), QStringLiteral("localsend"),
       {QStringLiteral("{path}")}, QStringLiteral("S"), QStringLiteral("localsend"), kEverything,
       false},

      {QStringLiteral("tailscale"), QStringLiteral("Send with Tailscale"),
       QStringLiteral("omarchy-tailscale-send"), {QStringLiteral("{path}")}, {},
       QStringLiteral("omarchy"), kEverything, false},

      {QStringLiteral("files"), QStringLiteral("Show in files"), QStringLiteral("nautilus"),
       {QStringLiteral("--select"), QStringLiteral("{path}")}, QStringLiteral("F"),
       QStringLiteral("nautilus"), kEverything, false},

      // Native: QML owns these.
      {QStringLiteral("favorite"), QStringLiteral("Favourite"), {}, {}, QStringLiteral("V"), {},
       kEverything, false},
      {QStringLiteral("hide"), QStringLiteral("Hide"), {}, {}, QStringLiteral("H"), {},
       kEverything, false},
      {QStringLiteral("trash"), QStringLiteral("Move to Trash"), {}, {}, QStringLiteral("Del"), {},
       kEverything, false},
  };
}

ActionRegistry::ActionRegistry(ActionLauncher* launcher, QObject* parent)
    : QObject(parent), m_launcher(launcher), m_definitions(buildTable()) {}

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

QString ActionRegistry::primaryActionFor(int kind) const {
  for (const Definition& definition : m_definitions) {
    if (definition.primary && definition.kinds.contains(static_cast<Kind>(kind))) {
      return definition.id;
    }
  }
  return QStringLiteral("open");
}

QVariantList ActionRegistry::actionsFor(int kind) const {
  QVariantList rows;
  for (const Definition& definition : m_definitions) {
    if (!definition.kinds.contains(static_cast<Kind>(kind))) {
      continue;
    }

    const bool native = definition.program.isEmpty();
    QVariantMap row;
    row[QStringLiteral("id")] = definition.id;
    row[QStringLiteral("label")] = definition.label;
    row[QStringLiteral("shortcut")] = definition.shortcut;
    row[QStringLiteral("primary")] = definition.primary;
    row[QStringLiteral("native")] = native;
    row[QStringLiteral("available")] =
        native || (m_launcher && m_launcher->handlerAvailable(definition.program));
    row[QStringLiteral("hint")] = definition.packageHint;
    rows.append(row);
  }
  return rows;
}

bool ActionRegistry::run(const QString& id, const QString& path) {
  const Definition* definition = find(id);
  if (!definition || definition->program.isEmpty() || !m_launcher) {
    return false;
  }

  QStringList arguments;
  arguments.reserve(definition->arguments.size());
  for (const QString& argument : definition->arguments) {
    if (argument == QStringLiteral("{path}")) {
      arguments.append(path);
    } else if (argument == QStringLiteral("{mime}")) {
      arguments.append(ActionLauncher::mimeTypeFor(path));
    } else {
      arguments.append(argument);
    }
  }

  // OCR writes to stdout rather than doing something visible, so it is the one
  // row whose result has to be captured and put somewhere useful.
  if (id == QStringLiteral("ocr")) {
    return m_launcher->captureTextTo(definition->program, arguments);
  }

  return m_launcher->runDetached(definition->program, arguments, definition->packageHint);
}
