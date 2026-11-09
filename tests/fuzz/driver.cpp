#include "app/AppSettings.h"
#include "app/LaunchRequest.h"
#include "app/SingleInstance.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"
#include "subtitles/SubtitleIndex.h"
#include "pdf/PdfSupport.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QBuffer>
#include <QImageReader>
#include <cstdlib>

void fuzzMatroska(const QByteArray&);
namespace {
struct Environment {
  int argc = 1;
  char name[8] = "fuzzer";
  char* argv[2] = {name, nullptr};
  QCoreApplication app{argc, argv};
  QTemporaryDir dir;
  Environment() {
    if (!dir.isValid()) std::abort();
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    // Warnings about rejected input are expected on nearly every run and
    // only slow the fuzzer down. Crashes and sanitizer reports still print.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
  }
};
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static Environment env;
  const QByteArray bytes(reinterpret_cast<const char*>(data), qsizetype(size));
#if FUZZ_KIND == 1
  (void)LaunchRequest::parseStart(QString::fromUtf8(bytes));
#elif FUZZ_KIND == 2
  QStringList paths, skipped; bool library; qint64 start;
  (void)SingleInstance::parseFrame(bytes, paths, library, start, skipped);
#elif FUZZ_KIND == 3
  fuzzMatroska(bytes);
#elif FUZZ_KIND == 4
  (void)PdfSupport::parsePageWords(bytes);
#elif FUZZ_KIND == 5
  QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  const ImageBudget::Estimate estimate = ImageBudget::estimate(reader);
  // The header's dimensions feed the memory arithmetic, so run that too.
  (void)ImageBudget::plan(estimate, 2560);
  (void)ImageBudget::bytesAt(estimate, estimate.stored / 3);
  (void)ImageBudget::decodedSize(estimate, estimate.stored / 3);
#else
  const QString path = env.dir.filePath(QStringLiteral("input.raw"));
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) std::abort();
  file.close();
#if FUZZ_KIND == 6
  (void)CameraRaw::readMetadata(path);
#elif FUZZ_KIND == 7
  SubtitleIndex::fuzzParse(path);
#elif FUZZ_KIND == 8
  AppSettings settings;
  (void)settings.importOrganization(path);
#endif
#endif
  return 0;
}
