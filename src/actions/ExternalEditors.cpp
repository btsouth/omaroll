#include "actions/ExternalEditors.h"

#include "actions/ActionLauncher.h"
#include "sources/CameraRaw.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace {

constexpr auto kPreferredId = "editors/preferredId";
constexpr auto kCustomCommand = "editors/customCommand";
constexpr auto kAlwaysChoose = "editors/alwaysChoose";
constexpr auto kCustomId = "custom";

} // namespace

ExternalEditors::ExternalEditors(ActionLauncher* launcher, QObject* parent)
    : QObject(parent),
      m_launcher(launcher),
      m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("omaroll"),
                 QStringLiteral("omaroll")),
      m_preferredId(m_settings.value(kPreferredId).toString()),
      m_customCommand(m_settings.value(kCustomCommand).toString()),
      m_alwaysChoose(m_settings.value(kAlwaysChoose, false).toBool()) {
  if (!editableId(m_preferredId)) {
    m_preferredId.clear();
  }
  if (m_launcher != nullptr) {
    connect(m_launcher, &ActionLauncher::failed, this,
            [this](const QString& failure) { setMessage(failure); });
  }
}

QList<ExternalEditors::Editor> ExternalEditors::knownEditors() {
  return {
      {u"darktable"_s, u"darktable"_s, u"darktable"_s, u"darktable"_s},
      {u"rawtherapee"_s, u"RawTherapee"_s, u"rawtherapee"_s, u"rawtherapee"_s},
      {u"art"_s, u"ART"_s, u"ART"_s, u"art"_s},
      {u"digikam"_s, u"digiKam"_s, u"digikam"_s, u"digikam"_s},
      {u"gimp"_s, u"GIMP"_s, u"gimp"_s, u"gimp"_s},
  };
}

QString ExternalEditors::executableFor(const QString& program) {
  const QString found = QStandardPaths::findExecutable(program);
  if (!found.isEmpty()) {
    return found;
  }
  const QFileInfo file(program);
  return program.contains(QLatin1Char('/')) && file.isFile() && file.isExecutable()
             ? file.absoluteFilePath()
             : QString();
}

bool ExternalEditors::editableId(const QString& id) const {
  if (id == kCustomId) {
    return true;
  }
  for (const Editor& editor : knownEditors()) {
    if (editor.id == id) {
      return true;
    }
  }
  return false;
}

bool ExternalEditors::parseCustomCommand(const QString& command, QString* program,
                                         QStringList* arguments) const {
  QStringList tokens = QProcess::splitCommand(command);
  if (tokens.isEmpty()) {
    return false;
  }

  const QString executable = tokens.takeFirst();
  if (executable.trimmed().isEmpty()) {
    return false;
  }

  // A shell parses its command again. Never interpolate filenames into one,
  // including a shell invoked through env or another command wrapper. Use a
  // script executable that receives the path as an ordinary argument instead.
  const QStringList shells{u"sh"_s, u"bash"_s, u"dash"_s, u"zsh"_s, u"fish"_s,
                           u"ksh"_s, u"csh"_s, u"tcsh"_s, u"env"_s};
  for (const QString& token : QProcess::splitCommand(command)) {
    if (shells.contains(QFileInfo(token).fileName())) return false;
  }

  if (program != nullptr) {
    *program = executable;
  }
  if (arguments != nullptr) {
    *arguments = tokens;
  }
  return true;
}

bool ExternalEditors::customCommandAvailable(const QString& command) const {
  QString program;
  return parseCustomCommand(command, &program, nullptr) && !executableFor(program).isEmpty();
}

bool ExternalEditors::available(const QString& id) const {
  if (id == kCustomId) {
    return customCommandAvailable(m_customCommand);
  }
  for (const Editor& editor : knownEditors()) {
    if (editor.id == id) {
      return !executableFor(editor.program).isEmpty();
    }
  }
  return false;
}

QVariantList ExternalEditors::candidates() const {
  QVariantList rows;
  for (const Editor& editor : knownEditors()) {
    rows.append(QVariantMap{
        {u"id"_s, editor.id},
        {u"label"_s, editor.label},
        {u"available"_s, available(editor.id)},
        {u"preferred"_s, editor.id == m_preferredId},
    });
  }
  rows.append(QVariantMap{
      {u"id"_s, QString::fromLatin1(kCustomId)},
      {u"label"_s, u"Custom command"_s},
      {u"available"_s, available(QString::fromLatin1(kCustomId))},
      {u"preferred"_s, m_preferredId == QLatin1String(kCustomId)},
  });
  return rows;
}

QString ExternalEditors::firstInstalledId() const {
  for (const Editor& editor : knownEditors()) {
    if (available(editor.id)) {
      return editor.id;
    }
  }
  return {};
}

void ExternalEditors::setPreferredId(const QString& id) { setPreferred(id); }

bool ExternalEditors::setPreferred(const QString& id) {
  if (!editableId(id)) {
    setMessage(u"Unknown external editor"_s);
    return false;
  }
  if (!available(id)) {
    setMessage(u"That external editor is not installed"_s);
    return false;
  }
  if (m_preferredId == id) {
    return true;
  }
  m_preferredId = id;
  m_settings.setValue(kPreferredId, m_preferredId);
  emit preferencesChanged();
  return true;
}

void ExternalEditors::setCustomCommand(const QString& command) {
  if (m_customCommand == command) {
    return;
  }
  m_customCommand = command;
  m_settings.setValue(kCustomCommand, m_customCommand);
  emit preferencesChanged();
}

void ExternalEditors::setAlwaysChoose(bool value) {
  if (m_alwaysChoose == value) {
    return;
  }
  m_alwaysChoose = value;
  m_settings.setValue(kAlwaysChoose, m_alwaysChoose);
  emit preferencesChanged();
}

void ExternalEditors::setRequestedPath(const QString& path) {
  if (m_requestedPath == path) {
    return;
  }
  m_requestedPath = path;
  emit requestedPathChanged();
}

void ExternalEditors::setMessage(const QString& message, bool missingEditors) {
  if (m_message == message && m_missingEditors == missingEditors) {
    return;
  }
  m_message = message;
  m_missingEditors = missingEditors;
  emit messageChanged();
}

void ExternalEditors::reject(const QString& message) {
  setMessage(message);
  if (m_launcher != nullptr) {
    m_launcher->report(message);
  }
}

void ExternalEditors::showChooser(const QString& path) {
  if (!QFileInfo(path).isFile() || !CameraRaw::isRawFile(path)) {
    reject(u"Choose an existing camera RAW to open in an external editor"_s);
    return;
  }
  setRequestedPath(path);
  if (firstInstalledId().isEmpty() && !available(QString::fromLatin1(kCustomId))) {
    setMessage(u"No RAW editor is installed. Install darktable or RawTherapee, or set a custom "
               u"command."_s, true);
  } else {
    setMessage({});
  }
  emit chooseRequested();
}

bool ExternalEditors::requestOpen(const QString& path) {
  const QFileInfo file(path);
  if (path.isEmpty() || !file.isFile()) {
    reject(u"That file is no longer there"_s);
    return false;
  }
  if (!CameraRaw::isRawFile(path)) {
    reject(u"Choose a camera RAW to open in an external editor"_s);
    return false;
  }

  if (!m_alwaysChoose) {
    if (!m_preferredId.isEmpty() && available(m_preferredId)) {
      return launch(m_preferredId, path);
    }
    const QString initial = firstInstalledId();
    if (!initial.isEmpty()) {
      return launch(initial, path);
    }
  }

  showChooser(path);
  return true;
}

bool ExternalEditors::launchCustom(const QString& command, const QString& path) {
  const bool started = launchImpl(QString::fromLatin1(kCustomId), path, command);
  if (started) setCustomCommand(command);
  return started;
}

bool ExternalEditors::launch(const QString& id, const QString& path) {
  return launchImpl(id, path, m_customCommand);
}

bool ExternalEditors::launchImpl(const QString& id, const QString& path,
                                 const QString& customCommand) {
  if (m_launcher == nullptr) {
    reject(u"External editor launching is unavailable"_s);
    return false;
  }
  const QFileInfo file(path);
  if (path.isEmpty() || !file.isFile()) {
    reject(u"That file is no longer there"_s);
    return false;
  }
  if (!CameraRaw::isRawFile(path)) {
    reject(u"External editors open camera RAW files only"_s);
    return false;
  }

  if (id == QLatin1String(kCustomId)) {
    QString program;
    QStringList arguments;
    if (!parseCustomCommand(customCommand, &program, &arguments)) {
      reject(u"Use a direct editor command or wrapper script"_s);
      return false;
    }
    bool substituted = false;
    for (QString& argument : arguments) {
      if (argument.contains(u"{path}"_s)) {
        argument.replace(u"{path}"_s, path);
        substituted = true;
      }
    }
    if (!substituted) {
      arguments.append(path);
    }
    setMessage({});
    return m_launcher->runDetached(program, arguments, {},
                                   u"Opening RAW in the custom editor"_s);
  }

  for (const Editor& editor : knownEditors()) {
    if (editor.id != id) {
      continue;
    }
    setMessage({});
    return m_launcher->runDetached(editor.program, {path}, editor.packageHint,
                                   u"Opening RAW in %1"_s.arg(editor.label));
  }

  reject(u"Unknown external editor"_s);
  return false;
}
