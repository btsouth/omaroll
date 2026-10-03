#include "actions/ActionLauncher.h"
#include "actions/ExternalEditors.h"
#include "app/AppSettings.h"
#include "sources/CameraRaw.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

constexpr auto kSettingsOrganization = "omaroll";
constexpr auto kSettingsApplication = "omaroll";

bool clearOmarollSettings() {
  QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromLatin1(kSettingsOrganization),
                     QString::fromLatin1(kSettingsApplication));
  settings.clear();
  settings.sync();
  return settings.status() == QSettings::NoError;
}

QString shellQuote(QString text) {
  text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + text + QLatin1Char('\'');
}

bool writeExecutable(const QString& path, const QByteArray& contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(contents) != contents.size()) {
    return false;
  }
  file.close();
  return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

QByteArray recorderScript(const QString& logPath) {
  const QString script =
      QStringLiteral("#!/bin/sh\n"
                     "{\n"
                     "  printf '%s\\0' \"$0\"\n"
                     "  printf '%s\\0' \"$#\"\n"
                     "  for argument in \"$@\"; do\n"
                     "    printf '%s\\0' \"$argument\"\n"
                     "  done\n"
                     "} > ") +
      shellQuote(logPath) + QLatin1Char('\n');
  return script.toUtf8();
}

struct RecordedLaunch {
  bool complete = false;
  QByteArray program;
  QList<QByteArray> arguments;
};

RecordedLaunch recordedLaunch(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QByteArray bytes = file.readAll();
  if (bytes.isEmpty() || !bytes.endsWith('\0')) {
    return {};
  }

  QList<QByteArray> fields = bytes.split('\0');
  fields.removeLast();
  if (fields.size() < 2) {
    return {};
  }

  bool countOkay = false;
  const int argumentCount = fields.at(1).toInt(&countOkay);
  if (!countOkay || argumentCount < 0 || fields.size() != argumentCount + 2) {
    return {};
  }

  RecordedLaunch launch;
  launch.program = fields.at(0);
  for (int index = 0; index < argumentCount; ++index) {
    launch.arguments.append(fields.at(index + 2));
  }
  launch.complete = true;
  return launch;
}

void restoreEnvironment(const char* name, const QByteArray& value) {
  if (value.isNull()) {
    qunsetenv(name);
  } else {
    qputenv(name, value);
  }
}

} // namespace

class EditorContractsTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    QVERIFY(m_scratch.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_scratch.filePath(QStringLiteral("config")));
  }

  void init() {
    QVERIFY(clearOmarollSettings());
    m_oldPath = qgetenv("PATH");

    m_caseRoot = m_scratch.filePath(QStringLiteral("case-%1").arg(++m_caseNumber));
    m_bin = m_caseRoot + QStringLiteral("/bin");
    m_log = m_caseRoot + QStringLiteral("/argv.bin");
    QVERIFY(QDir().mkpath(m_bin));
    // Keep host-installed editors out of the contract. Recorder scripts use
    // their absolute /bin/sh shebang, so no other PATH entries are needed.
    QVERIFY(qputenv("PATH", m_bin.toUtf8()));
  }

  void cleanup() {
    QVERIFY(clearOmarollSettings());
    restoreEnvironment("PATH", m_oldPath);
    if (!m_caseRoot.isEmpty()) {
      QDir(m_caseRoot).removeRecursively();
    }
  }

  void firstInstalledEditorIsUsed() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    const QString darktable = makeRecorder(m_bin, QStringLiteral("darktable"));
    const QString rawtherapee = makeRecorder(m_bin, QStringLiteral("rawtherapee"));
    QVERIFY(!darktable.isEmpty() && !rawtherapee.isEmpty());

    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    QVERIFY(editors.requestOpen(raw));
    QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);

    const RecordedLaunch launch = recordedLaunch(m_log);
    QCOMPARE(launch.program, QFile::encodeName(darktable));
    QCOMPARE(launch.arguments.size(), 1);
    QCOMPARE(launch.arguments.at(0), QFile::encodeName(raw));
  }

  void externalAndCustomSelectionPersist() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    QVERIFY(!makeRecorder(m_bin, QStringLiteral("darktable")).isEmpty());
    const QString rawtherapee = makeRecorder(m_bin, QStringLiteral("rawtherapee"));
    QVERIFY(!rawtherapee.isEmpty());
    ActionLauncher launcher;

    {
      ExternalEditors editors(&launcher);
      QVERIFY(editors.setPreferred(QStringLiteral("rawtherapee")));
      QCOMPARE(editors.preferredId(), QStringLiteral("rawtherapee"));
    }
    {
      ExternalEditors editors(&launcher);
      QCOMPARE(editors.preferredId(), QStringLiteral("rawtherapee"));
      QVERIFY(editors.requestOpen(raw));
      QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);
      const RecordedLaunch launch = recordedLaunch(m_log);
      QCOMPARE(launch.program, QFile::encodeName(rawtherapee));
      QCOMPARE(launch.arguments.size(), 1);
      QCOMPARE(launch.arguments.at(0), QFile::encodeName(raw));
    }

    QVERIFY(QFile::remove(m_log));
    const QString custom = makeRecorder(
        m_caseRoot + QStringLiteral("/custom editor"), QStringLiteral("external tool"));
    QVERIFY(!custom.isEmpty());
    const QString command =
        QLatin1Char('"') + custom + QStringLiteral("\" --custom");

    {
      ExternalEditors editors(&launcher);
      editors.setCustomCommand(command);
      QVERIFY(editors.setPreferred(QStringLiteral("custom")));
    }
    {
      ExternalEditors editors(&launcher);
      QCOMPARE(editors.customCommand(), command);
      QCOMPARE(editors.preferredId(), QStringLiteral("custom"));
      QVERIFY(editors.requestOpen(raw));
      QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);
      const RecordedLaunch launch = recordedLaunch(m_log);
      QCOMPARE(launch.program, QFile::encodeName(custom));
      QCOMPARE(launch.arguments.size(), 2);
      QCOMPARE(launch.arguments.at(0), QByteArray("--custom"));
      QCOMPARE(launch.arguments.at(1), QFile::encodeName(raw));
    }
  }

  void alwaysChoosePersistsAndNeverLaunches() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    ActionLauncher launcher;
    {
      ExternalEditors editors(&launcher);
      QVERIFY(!editors.alwaysChoose());
      editors.setAlwaysChoose(true);
    }

    ExternalEditors editors(&launcher);
    QVERIFY(editors.alwaysChoose());
    QSignalSpy chooser(&editors, &ExternalEditors::chooseRequested);
    QVERIFY(editors.requestOpen(raw));
    QCOMPARE(chooser.count(), 1);
    QCOMPARE(editors.requestedPath(), raw);
    QVERIFY(!QFileInfo::exists(m_log));
  }

  void chooserRequestIsSharedByDirectAndRequestedOpen() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    QSignalSpy chooser(&editors, &ExternalEditors::chooseRequested);

    editors.showChooser(raw);
    QCOMPARE(chooser.count(), 1);
    QCOMPARE(editors.requestedPath(), raw);

    QVERIFY(editors.requestOpen(raw));
    QCOMPARE(chooser.count(), 2);
    QCOMPARE(editors.requestedPath(), raw);
    QVERIFY(editors.message().contains(QStringLiteral("No RAW editor is installed")));
  }

  void unavailablePreferenceFallsBackToAnotherInstalledEditor() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    {
      ActionLauncher launcher;
      ExternalEditors editors(&launcher);
      QVERIFY(!editors.setPreferred(QStringLiteral("darktable")));
      QVERIFY(editors.message().contains(QStringLiteral("not installed")));
    }

    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromLatin1(kSettingsOrganization),
                       QString::fromLatin1(kSettingsApplication));
    settings.setValue(QStringLiteral("editors/preferredId"), QStringLiteral("darktable"));
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);

    const QString rawtherapee = makeRecorder(m_bin, QStringLiteral("rawtherapee"));
    QVERIFY(!rawtherapee.isEmpty());

    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    QCOMPARE(editors.preferredId(), QStringLiteral("darktable"));
    QVERIFY(editors.requestOpen(raw));
    QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);
    const RecordedLaunch launch = recordedLaunch(m_log);
    QCOMPARE(launch.program, QFile::encodeName(rawtherapee));
    QCOMPARE(launch.arguments.size(), 1);
    QCOMPARE(launch.arguments.at(0), QFile::encodeName(raw));
  }

  void invalidLaunchAndChooserRequestsAreRefused() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    const QString nonRaw = m_caseRoot + QStringLiteral("/not raw.txt");
    QFile text(nonRaw);
    QVERIFY(text.open(QIODevice::WriteOnly));
    QVERIFY(text.write("plain text") == 10);
    text.close();
    const QString missingRaw = m_caseRoot + QStringLiteral("/missing.dng");

    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    QSignalSpy chooser(&editors, &ExternalEditors::chooseRequested);

    QVERIFY(!editors.launch(QStringLiteral("darktable"), nonRaw));
    QVERIFY(editors.message().contains(QStringLiteral("camera RAW files only")));

    QVERIFY(!editors.launch(QStringLiteral("darktable"), missingRaw));
    QCOMPARE(editors.message(), QStringLiteral("That file is no longer there"));

    QVERIFY(!editors.launch(QStringLiteral("unknown"), raw));
    QCOMPARE(editors.message(), QStringLiteral("Unknown external editor"));

    QVERIFY(!editors.requestOpen(nonRaw));
    editors.showChooser(nonRaw);
    editors.showChooser(missingRaw);
    QCOMPARE(chooser.count(), 0);
    QVERIFY(editors.requestedPath().isEmpty());
    QVERIFY(!QFileInfo::exists(m_log));
  }

  void customCommandPreservesExactDangerousArgv() {
    const QString raw = rawFixture();
    if (!rawSupported()) {
      QSKIP("Qt RAW support is unavailable");
    }
    QVERIFY2(!raw.isEmpty(), "RAW fixture is missing");

    const QString rawDirectory = m_caseRoot + QStringLiteral("/dangerous RAW");
    QVERIFY(QDir().mkpath(rawDirectory));
    const QString dangerousPath =
        rawDirectory + QStringLiteral("/camera \"double\" 'single' ; $(touch injected) ") +
        QChar(0x00fc) + QStringLiteral("nicode.dng");
    QVERIFY(QFile::copy(raw, dangerousPath));

    const QString custom = makeRecorder(
        m_caseRoot + QStringLiteral("/custom editor"), QStringLiteral("external tool"));
    QVERIFY(!custom.isEmpty());

    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    const QString embeddedCommand =
        QLatin1Char('"') + custom +
        QStringLiteral("\" --file={path} --quoted \"two words\"");
    editors.setCustomCommand(embeddedCommand);

    QVERIFY(editors.launch(QStringLiteral("custom"), dangerousPath));
    QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);
    RecordedLaunch launch = recordedLaunch(m_log);
    QCOMPARE(launch.program, QFile::encodeName(custom));
    QCOMPARE(launch.arguments.size(), 3);
    QCOMPARE(launch.arguments.at(0),
             QByteArray("--file=") + QFile::encodeName(dangerousPath));
    QCOMPARE(launch.arguments.at(1), QByteArray("--quoted"));
    QCOMPARE(launch.arguments.at(2), QByteArray("two words"));

    QVERIFY(QFile::remove(m_log));
    const QString appendCommand =
        QLatin1Char('"') + custom + QStringLiteral("\" --fixed");
    editors.setCustomCommand(appendCommand);
    QVERIFY(editors.launch(QStringLiteral("custom"), dangerousPath));
    QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);
    launch = recordedLaunch(m_log);
    QCOMPARE(launch.program, QFile::encodeName(custom));
    QCOMPARE(launch.arguments.size(), 2);
    QCOMPARE(launch.arguments.at(0), QByteArray("--fixed"));
    QCOMPARE(launch.arguments.at(1), QFile::encodeName(dangerousPath));
  }

  void failedCustomLaunchPreservesSavedCommand() {
    if (!rawSupported()) QSKIP("Qt RAW support is unavailable");
    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    const QString custom = makeRecorder(m_caseRoot, QStringLiteral("custom-editor"));
    QVERIFY(!custom.isEmpty());
    const QString saved = custom + QStringLiteral(" {path}");
    editors.setCustomCommand(saved);
    QSignalSpy preferences(&editors, &ExternalEditors::preferencesChanged);
    QVERIFY(!editors.launchCustom(QStringLiteral("missing-draft-editor {path}"), rawFixture()));
    QCOMPARE(editors.customCommand(), saved);
    QCOMPARE(preferences.count(), 0);
    const QString replacement = custom + QStringLiteral(" --draft {path}");
    QVERIFY(editors.launchCustom(replacement, rawFixture()));
    QCOMPARE(editors.customCommand(), replacement);
    QCOMPARE(preferences.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(recordedLaunch(m_log).complete, 3000);
  }

  void shellPlaceholdersAreRefused() {
    if (!rawSupported()) QSKIP("Qt RAW support is unavailable");
    ActionLauncher launcher;
    ExternalEditors editors(&launcher);
    QSignalSpy messages(&launcher, &ActionLauncher::reported);
    for (const QString& command : {QStringLiteral("sh -c \"editor {path}\""),
                                   QStringLiteral("/bin/bash -lc \"editor {path}\""),
                                   QStringLiteral("env sh -c \"editor {path}\""),
                                   QStringLiteral("env -S \"sh -c 'editor {path}'\""),
                                   QStringLiteral("env --split-string=\"sh -c 'editor {path}'\""),
                                   QStringLiteral("env -vS \"sh -c 'editor {path}'\""),
                                   QStringLiteral("sh -c")}) {
      QVERIFY(!editors.customCommandAvailable(command));
      QVERIFY(!editors.launchCustom(command, rawFixture()));
    }
    QCOMPARE(messages.count(), 7);
    for (const auto& message : messages)
      QCOMPARE(message.first().toString(), QStringLiteral("Use a direct editor command or wrapper script"));
    QVERIFY(!QFileInfo::exists(m_log));
    QCOMPARE(editors.customCommand(), QString());
  }

  void pinCanonicalizesDeduplicatesAndKeepsLibrarySource() {
    const QString folder = m_caseRoot + QStringLiteral("/library");
    QVERIFY(QDir().mkpath(folder));
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    QVERIFY(!canonical.isEmpty());
    const QString alias = m_caseRoot + QStringLiteral("/library alias");
    QVERIFY(QFile::link(canonical, alias));

    AppSettings settings;
    QVERIFY(settings.pinFolderPath(folder));
    QCOMPARE(settings.pinnedFolders(), QStringList{canonical});
    QVERIFY(settings.libraryFolders().contains(canonical));
    QVERIFY(settings.isFolderPinned(alias));

    QVERIFY(!settings.pinFolderPath(alias));
    QCOMPARE(settings.pinnedFolders(), QStringList{canonical});
    QCOMPARE(settings.libraryFolders().count(canonical), 1);

    settings.unpinFolder(alias);
    QVERIFY(settings.pinnedFolders().isEmpty());
    QVERIFY(settings.libraryFolders().contains(canonical));
  }

  void offlinePinPersistsUntilExplicitlyRemoved() {
    const QString folder = m_caseRoot + QStringLiteral("/offline");
    QVERIFY(QDir().mkpath(folder));
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    QVERIFY(!canonical.isEmpty());
    {
      AppSettings settings;
      QVERIFY(settings.pinFolderPath(folder));
    }
    QVERIFY(QDir(folder).removeRecursively());

    {
      AppSettings settings;
      QCOMPARE(settings.pinnedFolders(), QStringList{canonical});
      QVERIFY(settings.isFolderPinned(folder));
      QVERIFY(!settings.pinFolderPath(folder));
      settings.unpinFolder(folder);
      QVERIFY(settings.pinnedFolders().isEmpty());
    }
    {
      AppSettings settings;
      QVERIFY(settings.pinnedFolders().isEmpty());
    }
  }

  void pairRawJpegDefaultsTrueAndPersists() {
    {
      AppSettings settings;
      QVERIFY(settings.pairRawJpeg());
      QSignalSpy changed(&settings, &AppSettings::pairRawJpegChanged);
      settings.setPairRawJpeg(false);
      QCOMPARE(changed.count(), 1);
      QVERIFY(!settings.pairRawJpeg());
    }
    {
      AppSettings settings;
      QVERIFY(!settings.pairRawJpeg());
      settings.setPairRawJpeg(true);
    }
    {
      AppSettings settings;
      QVERIFY(settings.pairRawJpeg());
    }
  }

private:
  bool rawSupported() const { return CameraRaw::isRaw(QStringLiteral("dng")); }

  QString rawFixture() const {
    return QFINDTESTDATA(QStringLiteral("fixtures/raw/camera.dng"));
  }

  QString makeRecorder(const QString& directory, const QString& name) const {
    if (!QDir().mkpath(directory)) {
      return {};
    }
    const QString path = directory + QLatin1Char('/') + name;
    return writeExecutable(path, recorderScript(m_log)) ? path : QString();
  }

  QTemporaryDir m_scratch;
  QString m_caseRoot;
  QString m_bin;
  QString m_log;
  QByteArray m_oldPath;
  int m_caseNumber = 0;
};

int main(int argc, char** argv) {
  QTemporaryDir config;
  if (!config.isValid()) {
    return 1;
  }
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  QCoreApplication application(argc, argv);
  EditorContractsTest test;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&test, argc, argv);
}

#include "tst_editors.moc"
