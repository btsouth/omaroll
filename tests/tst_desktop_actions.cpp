#include "actions/ActionLauncher.h"
#include "actions/ActionRegistry.h"
#include "actions/OpenWithRequest.h"

#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class FakeFileManager : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
  QStringList items;
  QString startup;
public slots:
  void ShowItems(const QStringList& uris, const QString& startupId) {
    items = uris;
    startup = startupId;
  }
};

class FakeChooser : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.OpenURI")
  Q_PROPERTY(uint version READ version)
public:
  explicit FakeChooser(QDBusConnection bus) : bus(std::move(bus)) {}
  uint version() const { return apiVersion; }
  uint apiVersion = 3;
  uint result = 0;
  bool answerImmediately = true;
  int calls = 0;
  QVariantMap options;
  QByteArray contents;
  QString parentWindow;
  QString handle;
  QDBusConnection bus;
  void respond() {
    auto signal = QDBusMessage::createSignal(handle, QStringLiteral("org.freedesktop.portal.Request"),
                                            QStringLiteral("Response"));
    signal.setArguments({result, QVariantMap()});
    QVERIFY(bus.send(signal));
  }
public slots:
  QDBusObjectPath OpenFile(const QString& parent, const QDBusUnixFileDescriptor& fd,
                          const QVariantMap& requestedOptions) {
    ++calls;
    options = requestedOptions;
    parentWindow = parent;
    QFile file;
    if (file.open(fd.fileDescriptor(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
      contents = file.readAll();
    }
    QString sender = message().service().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    handle = QStringLiteral("/org/freedesktop/portal/desktop/request/") + sender + QLatin1Char('/')
        + options.value(QStringLiteral("handle_token")).toString();
    // Deliberately emit before the method reply to cover the subscription race.
    if (answerImmediately) respond();
    return QDBusObjectPath(handle);
  }
};

class DesktopActionTest : public QObject {
  Q_OBJECT
private:
  QProcess daemon;
  QString address;
  QString filePath;
  QTemporaryDir scratch;
  QMap<QByteArray, QByteArray> savedEnvironment;
  const QString defaultService = QStringLiteral("org.omaroll.TestFileManager");
  const QString genericService = QStringLiteral("org.freedesktop.FileManager1");
  QString writeFile(const QString& name, const QByteArray& contents) {
    const QString path = scratch.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) return {};
    return path;
  }
  QDBusConnection serviceBus() { return QDBusConnection(QStringLiteral("desktop-service-test")); }
  QDBusConnection clientBus() { return QDBusConnection(QStringLiteral("desktop-client-test")); }
private slots:
  void initTestCase() {
    QVERIFY(scratch.isValid());
    filePath = writeFile(QStringLiteral("image #1 % 雪.png"), "selected file contents");
    QVERIFY(!filePath.isEmpty());
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    QVERIFY2(!executable.isEmpty(), "Private D-Bus test requires dbus-daemon");
    daemon.start(executable, {QStringLiteral("--session"), QStringLiteral("--nofork"),
                             QStringLiteral("--nopidfile"), QStringLiteral("--print-address")});
    QVERIFY(daemon.waitForReadyRead(5000));
    address = QString::fromUtf8(daemon.readLine()).trimmed();
    QVERIFY(QDBusConnection::connectToBus(address, QStringLiteral("desktop-service-test")).isConnected());
    QVERIFY(QDBusConnection::connectToBus(address, QStringLiteral("desktop-client-test")).isConnected());
  }
  void init() {
    for (const auto& key : {QByteArray("PATH"), QByteArray("XDG_DATA_HOME"), QByteArray("OMAROLL_TEST_LOG")}) {
      savedEnvironment[key] = qgetenv(key.constData());
    }
    QVERIFY(QDir().mkpath(scratch.filePath(QStringLiteral("applications"))));
    const QString query = writeFile(QStringLiteral("xdg-mime"),
        "#!/bin/sh\nprintf '%s\\n' 'org.omaroll.TestFileManager.desktop'\n");
    const QString opener = writeFile(QStringLiteral("xdg-open"),
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$OMAROLL_TEST_LOG\"\n");
    for (const QString& script : {query, opener}) {
      QVERIFY(QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    QVERIFY(!writeFile(QStringLiteral("applications/org.omaroll.TestFileManager.desktop"),
        "[Desktop Entry]\nType=Application\nName=Test folder handler\nDBusActivatable=true\n").isEmpty());
    QFile::remove(scratch.filePath(QStringLiteral("folder.log")));
    qputenv("PATH", scratch.path().toUtf8());
    qputenv("XDG_DATA_HOME", scratch.path().toUtf8());
    qputenv("OMAROLL_TEST_LOG", scratch.filePath(QStringLiteral("folder.log")).toUtf8());
  }
  void cleanup() {
    for (auto it = savedEnvironment.cbegin(); it != savedEnvironment.cend(); ++it) {
      if (it.value().isNull()) qunsetenv(it.key().constData()); else qputenv(it.key().constData(), it.value());
    }
    serviceBus().unregisterService(defaultService);
    serviceBus().unregisterObject(QStringLiteral("/org/freedesktop/FileManager1"));
    serviceBus().unregisterService(QStringLiteral("org.freedesktop.FileManager1"));
    serviceBus().unregisterObject(QStringLiteral("/org/freedesktop/portal/desktop"));
    serviceBus().unregisterService(QStringLiteral("org.freedesktop.portal.Desktop"));
  }
  void cleanupTestCase() {
    QDBusConnection::disconnectFromBus(QStringLiteral("desktop-client-test"));
    QDBusConnection::disconnectFromBus(QStringLiteral("desktop-service-test"));
    daemon.terminate();
    if (!daemon.waitForFinished(3000)) { daemon.kill(); daemon.waitForFinished(); }
  }
  void revealSelectsTheCapturedFile() {
    FakeFileManager manager;
    QVERIFY(serviceBus().registerService(defaultService));
    QVERIFY(serviceBus().registerObject(QStringLiteral("/org/freedesktop/FileManager1"), &manager,
                                       QDBusConnection::ExportAllSlots));
    ActionLauncher launcher(nullptr, clientBus());
    ActionRegistry registry(&launcher);
    QSignalSpy errors(&launcher, &ActionLauncher::failed);
    QVERIFY(registry.run(QStringLiteral("files"), filePath));
    QTRY_COMPARE(manager.items.size(), 1);
    QCOMPARE(manager.items.first(), QUrl::fromLocalFile(filePath).toString(QUrl::FullyEncoded));
    QCOMPARE(QUrl(manager.items.first()).toLocalFile(), filePath);
    QVERIFY(manager.startup.isEmpty());
    QVERIFY(errors.isEmpty());
  }
  void revealFallsBackToTheDefaultFolderHandler() {
    const QString log = scratch.filePath(QStringLiteral("folder.log"));
    ActionLauncher launcher(nullptr, clientBus());
    QVERIFY(launcher.showInFolder(filePath)); // The configured handler has no service on this private bus.
    QTRY_VERIFY(QFileInfo::exists(log));
    QFile output(log);
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QFileInfo(filePath).absolutePath().toUtf8() + '\n');
  }
  void anUnrelatedFileManagerCannotOverrideTheFolderDefault() {
    FakeFileManager unrelated;
    QVERIFY(serviceBus().registerService(genericService));
    QVERIFY(serviceBus().registerObject(QStringLiteral("/org/freedesktop/FileManager1"), &unrelated,
                                       QDBusConnection::ExportAllSlots));
    QVERIFY(!writeFile(QStringLiteral("applications/org.omaroll.TestFileManager.desktop"),
        "[Desktop Entry]\nType=Application\nName=Default folder handler\nDBusActivatable=false\n").isEmpty());
    ActionLauncher launcher(nullptr, clientBus());
    QVERIFY(launcher.showInFolder(filePath));
    QTRY_VERIFY(QFileInfo::exists(scratch.filePath(QStringLiteral("folder.log"))));
    QVERIFY(unrelated.items.isEmpty());
  }
  void chooserAlwaysAsksAndReceivesTheCapturedFile() {
    FakeChooser chooser(serviceBus());
    QVERIFY(serviceBus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(serviceBus().registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &chooser,
                                       QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    OpenWithRequest request(clientBus());
    QSignalSpy done(&request, &OpenWithRequest::finished);
    QSignalSpy errors(&request, &OpenWithRequest::failed);
    QVERIFY(request.start(filePath));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(chooser.calls, 1);
    QCOMPARE(chooser.contents, QByteArray("selected file contents"));
    QCOMPARE(chooser.options.value(QStringLiteral("ask")).toBool(), true);
    QCOMPARE(chooser.options.value(QStringLiteral("writable")).toBool(), true);
    QVERIFY(errors.isEmpty());
  }
  void cancellationDoesNotOpenADefaultAndAllowsRetry() {
    FakeChooser chooser(serviceBus());
    chooser.answerImmediately = false;
    chooser.result = 1;
    QVERIFY(serviceBus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(serviceBus().registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &chooser,
                                       QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    ActionLauncher launcher(nullptr, clientBus());
    ActionRegistry registry(&launcher);
    QSignalSpy errors(&launcher, &ActionLauncher::failed);
    QVERIFY(registry.run(QStringLiteral("open-with"), filePath));
    QTRY_COMPARE(chooser.calls, 1);
    QVERIFY(registry.run(QStringLiteral("open-with"), filePath));
    QCOMPARE(chooser.calls, 1); // Duplicate click while the chooser is open.
    chooser.respond();
    QTRY_COMPARE(launcher.findChildren<OpenWithRequest*>().size(), 0);
    QVERIFY(registry.run(QStringLiteral("open-with"), filePath));
    QTRY_COMPARE(chooser.calls, 2);
    chooser.respond();
    QTRY_COMPARE(launcher.findChildren<OpenWithRequest*>().size(), 0);
    QVERIFY(errors.isEmpty());
  }
  void aStoppedPortalDoesNotBlockRetry() {
    FakeChooser chooser(serviceBus());
    chooser.answerImmediately = false;
    QVERIFY(serviceBus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(serviceBus().registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &chooser,
                                       QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    ActionLauncher launcher(nullptr, clientBus());
    QSignalSpy errors(&launcher, &ActionLauncher::failed);
    QVERIFY(launcher.openWith(filePath));
    QTRY_COMPARE(chooser.calls, 1);
    QVERIFY(serviceBus().unregisterService(QStringLiteral("org.freedesktop.portal.Desktop")));
    QTRY_COMPARE(errors.size(), 1);
    QTRY_COMPARE(launcher.findChildren<OpenWithRequest*>().size(), 0);
    QVERIFY(serviceBus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(launcher.openWith(filePath));
    QTRY_COMPARE(chooser.calls, 2);
    chooser.respond();
    QTRY_COMPARE(launcher.findChildren<OpenWithRequest*>().size(), 0);
    QCOMPARE(errors.size(), 1);
  }
  void oldPortalsCannotSilentlyOpenTheDefaultApplication() {
    FakeChooser chooser(serviceBus());
    chooser.apiVersion = 2;
    QVERIFY(serviceBus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
    QVERIFY(serviceBus().registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &chooser,
                                       QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    OpenWithRequest request(clientBus());
    QSignalSpy errors(&request, &OpenWithRequest::failed);
    QVERIFY(request.start(filePath));
    QTRY_COMPARE(errors.size(), 1);
    QCOMPARE(chooser.calls, 0);
  }
  void missingPortalAndMissingFileReportErrors() {
    OpenWithRequest request(clientBus());
    QSignalSpy done(&request, &OpenWithRequest::finished);
    QSignalSpy errors(&request, &OpenWithRequest::failed);
    QVERIFY(request.start(filePath));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(errors.size(), 1);
    ActionLauncher launcher(nullptr, QDBusConnection(QString()));
    QSignalSpy failures(&launcher, &ActionLauncher::failed);
    QVERIFY(!launcher.openWith(scratch.filePath(QStringLiteral("missing.png"))));
    QVERIFY(!launcher.showInFolder(scratch.filePath(QStringLiteral("missing.png"))));
    QCOMPARE(failures.size(), 2);
  }
};

QTEST_GUILESS_MAIN(DesktopActionTest)
#include "tst_desktop_actions.moc"
