#include "thumbs/ThumbnailCache.h"
#include "thumbs/ThumbnailProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QPointer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace {

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeFile(const QString& path, const QByteArray& contents) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

QString shellQuote(QString text) {
  text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + text + QLatin1Char('\'');
}

// Executed in place of ffmpegthumbnailer. No child processes, codecs, GUI or
// media services: killing the helper kills all its work. A mode file allows
// the tests to hold every pool slot before queuing obsolete requests.
int runHelper(const QStringList& arguments) {
  QFile log(qEnvironmentVariable("OMAROLL_TEST_THUMB_LOG"));
  if (!log.open(QIODevice::WriteOnly | QIODevice::Append) || log.write("started\n") != 8) {
    return 2;
  }
  log.close();
  const QString modePath = qEnvironmentVariable("OMAROLL_TEST_THUMB_MODE");
  QElapsedTimer elapsed;
  elapsed.start();
  while (readFile(modePath) == "wait" && elapsed.elapsed() < 30000) {
    QThread::msleep(10);
  }
  if (readFile(modePath) != "success") {
    return 1;
  }
  const int outputArgument = arguments.indexOf(QStringLiteral("-o"));
  if (outputArgument < 0 || outputArgument + 1 >= arguments.size()) {
    return 2;
  }
  QImage image(96, 64, QImage::Format_RGB32);
  image.fill(QColor(60, 140, 210));
  return image.save(arguments.at(outputArgument + 1), "PNG") ? 0 : 2;
}

} // namespace

class ThumbnailTest final : public QObject {
  Q_OBJECT

private slots:
  void init() {
    m_oldPath = qgetenv("PATH");
    m_oldCache = qgetenv("XDG_CACHE_HOME");
    m_oldLog = qgetenv("OMAROLL_TEST_THUMB_LOG");
    m_oldMode = qgetenv("OMAROLL_TEST_THUMB_MODE");
    m_scratch = std::make_unique<QTemporaryDir>();
    QVERIFY(m_scratch->isValid());
    const QString bin = m_scratch->filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(bin));
    const QString helper = bin + QStringLiteral("/ffmpegthumbnailer");
    const QByteArray script = (QStringLiteral("#!/bin/sh\nexec ")
        + shellQuote(QCoreApplication::applicationFilePath())
        + QStringLiteral(" --thumbnail-test-helper \"$@\"\n")).toUtf8();
    QVERIFY(writeFile(helper, script));
    QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(qputenv("PATH", bin.toUtf8() + ':' + m_oldPath));
    QVERIFY(qputenv("XDG_CACHE_HOME", m_scratch->filePath(QStringLiteral("cache")).toUtf8()));
    QVERIFY(qputenv("OMAROLL_TEST_THUMB_LOG", logPath().toUtf8()));
    QVERIFY(qputenv("OMAROLL_TEST_THUMB_MODE", modePath().toUtf8()));
    QVERIFY(writeFile(modePath(), "success"));
  }

  void cleanup() {
    restoreEnvironment("PATH", m_oldPath);
    restoreEnvironment("XDG_CACHE_HOME", m_oldCache);
    restoreEnvironment("OMAROLL_TEST_THUMB_LOG", m_oldLog);
    restoreEnvironment("OMAROLL_TEST_THUMB_MODE", m_oldMode);
    m_scratch.reset();
  }

  void cancelledRequestDoesNotCacheOrSuppressRetry() {
    const QString path = mediaPath(QStringLiteral("clip.mp4"));
    QVERIFY(writeFile(path, "fake video"));
    std::atomic_bool cancelled{true};
    QVERIFY(ThumbnailCache::thumbnail(path, QSize(32, 24), 1, 20, {&cancelled}).isNull());
    QCOMPARE(helperStarts(), 0);
    QVERIFY(cacheFiles().isEmpty());

    cancelled.store(false);
    QVERIFY(!ThumbnailCache::thumbnail(path, QSize(32, 24), 1, 20, {&cancelled}).isNull());
    QCOMPARE(helperStarts(), 1);
    QCOMPARE(cacheFiles().size(), 1);
    // Existing synchronous callers still receive the warm cached image.
    QVERIFY(!ThumbnailCache::thumbnail(path, QSize(32, 24), 1).isNull());
    QCOMPARE(helperStarts(), 1);
    cancelled.store(true);
    QVERIFY(ThumbnailCache::thumbnail(path, QSize(32, 24), 1, 20, {&cancelled}).isNull());
    QCOMPARE(cacheFiles().size(), 1);
  }

  void runningCancellationDoesNotCacheOrSuppressRetry_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("derived");
    QTest::newRow("video") << QStringLiteral("clip.mp4") << false;
    QTest::newRow("image-video-fallback") << QStringLiteral("clip.png") << false;
    QTest::newRow("derived-source-fallback") << QStringLiteral("clip-720p.gif") << true;
  }

  void runningCancellationDoesNotCacheOrSuppressRetry() {
    QFETCH(QString, name);
    QFETCH(bool, derived);
    const QString path = mediaPath(name);
    if (derived) {
      QImage image(32, 24, QImage::Format_RGB32);
      image.fill(Qt::red);
      QVERIFY(image.save(path, "PNG"));
      QVERIFY(writeFile(mediaPath(QStringLiteral("clip.mp4")), "fake source"));
    } else {
      QVERIFY(writeFile(path, "fake video"));
    }
    QVERIFY(writeFile(modePath(), "wait"));
    std::atomic_bool cancelled{false};
    auto result = std::async(std::launch::async, [&] {
      return ThumbnailCache::thumbnail(path, QSize(32, 24), 1, 20, {&cancelled});
    });
    // Also cancel on assertion failure before the future joins its worker.
    const auto cancelOnExit = qScopeGuard([&] { cancelled.store(true); });
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts(), 1, 3000);
    cancelled.store(true);
    QVERIFY(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    QVERIFY(result.get().isNull());
    QCOMPARE(helperStarts(), 1);
    QVERIFY(cacheFiles().isEmpty());

    QVERIFY(writeFile(modePath(), "success"));
    // Same file identity must retry immediately after cancellation, including
    // a derived file whose own image decode would otherwise have succeeded.
    QVERIFY(!ThumbnailCache::thumbnail(path, QSize(32, 24), 1).isNull());
    QCOMPARE(helperStarts(), 2);
    QCOMPARE(cacheFiles().size(), 1);
  }

  void decodeFailureStillUsesNegativeCache() {
    const QString path = mediaPath(QStringLiteral("broken.mp4"));
    QVERIFY(writeFile(path, "fake video"));
    QVERIFY(writeFile(modePath(), "fail"));
    QVERIFY(ThumbnailCache::thumbnail(path, QSize(32, 24), 1).isNull());
    QCOMPARE(helperStarts(), 1);
    QVERIFY(ThumbnailCache::thumbnail(path, QSize(32, 24), 1).isNull());
    QCOMPARE(helperStarts(), 1);
    QVERIFY(cacheFiles().isEmpty());
  }

  void queuedResponsesFinishOnTheirOwnThread_data() {
    QTest::addColumn<QString>("action");
    QTest::newRow("request-cancel") << QStringLiteral("cancel");
    QTest::newRow("shutdown") << QStringLiteral("shutdown");
    QTest::newRow("destructor") << QStringLiteral("destroy");
  }

  void queuedResponsesFinishOnTheirOwnThread() {
    QFETCH(QString, action);
    QVERIFY(writeFile(modePath(), "wait"));
    auto provider = std::make_unique<ThumbnailProvider>();
    const int workers = qBound(2, QThread::idealThreadCount() - 1, 4);
    const int queued = 12;
    const int total = workers + queued;
    int finished = 0;
    bool correctThread = true;
    bool correctResult = true;
    std::vector<int> completions(total, 0);
    std::vector<QPointer<QQuickImageResponse>> responses;
    const auto cleanupResponses = qScopeGuard([&] {
      if (provider) {
        provider->shutdown();
      }
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      for (const auto& response : responses) {
        delete response.data();
      }
    });
    const auto request = [&](int index) {
      const QString path = mediaPath(QStringLiteral("clip-%1.mp4").arg(index));
      if (!writeFile(path, "fake video")) {
        return false;
      }
      auto* response = provider->requestImageResponse(QStringLiteral("1") + path, QSize(32, 24));
      responses.emplace_back(response);
      connect(response, &QQuickImageResponse::finished, this, [&, response, index] {
        correctThread = correctThread && QThread::currentThread() == response->thread()
            && QThread::currentThread() == thread();
        const bool cancelled = action != QStringLiteral("cancel") || index >= workers;
        correctResult = correctResult && (cancelled
            ? response->errorString() == QStringLiteral("Cancelled")
            : response->errorString().isEmpty());
        ++completions[index];
        ++finished;
        // Match Qt's response ownership: completion schedules deletion on the
        // response thread, never from the worker or from cancel().
        response->deleteLater();
      }, Qt::DirectConnection);
      return true;
    };
    for (int index = 0; index < workers; ++index) {
      QVERIFY(request(index));
    }
    QTRY_COMPARE_WITH_TIMEOUT(helperStarts(), workers, 3000);
    for (int index = workers; index < total; ++index) {
      QVERIFY(request(index));
      // Shutdown must also complete queued requests that Qt has not canceled.
      if (action == QStringLiteral("cancel") || index % 2 == 0) {
        responses.back()->cancel();
      }
    }

    if (action == QStringLiteral("cancel")) {
      QVERIFY(writeFile(modePath(), "success"));
    } else {
      QElapsedTimer elapsed;
      elapsed.start();
      if (action == QStringLiteral("shutdown")) {
        provider->shutdown();
        provider->shutdown(); // Repeated shutdown is harmless.
      } else {
        provider.reset();
      }
      QVERIFY2(elapsed.elapsed() < 2000, "Shutdown waited for obsolete helpers");
    }
    QTRY_COMPARE_WITH_TIMEOUT(finished, total, 3000);
    QVERIFY(correctThread);
    QVERIFY(correctResult);
    for (const int count : completions) {
      QCOMPARE(count, 1);
    }
    QCOMPARE(helperStarts(), workers);
    if (action != QStringLiteral("cancel")) {
      QVERIFY(cacheFiles().isEmpty());
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const auto& response : responses) {
      QVERIFY(response.isNull());
    }
  }

private:
  QString logPath() const { return m_scratch->filePath(QStringLiteral("helper.log")); }
  QString modePath() const { return m_scratch->filePath(QStringLiteral("mode")); }
  QString mediaPath(const QString& name) const { return m_scratch->filePath(name); }
  int helperStarts() const { return readFile(logPath()).count('\n'); }
  QStringList cacheFiles() const {
    return QDir(ThumbnailCache::cacheDirectory()).entryList(QDir::Files);
  }
  static void restoreEnvironment(const char* name, const QByteArray& value) {
    if (value.isNull()) {
      qunsetenv(name);
    } else {
      qputenv(name, value);
    }
  }
  std::unique_ptr<QTemporaryDir> m_scratch;
  QByteArray m_oldPath, m_oldCache, m_oldLog, m_oldMode;
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().value(1) == QStringLiteral("--thumbnail-test-helper")) {
    return runHelper(app.arguments());
  }
  ThumbnailTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "tst_thumbnails.moc"
